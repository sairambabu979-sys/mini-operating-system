// SPDX-License-Identifier: MIT
/*
 * Process creation and death. See <proc/process.h>.
 */
#define pr_fmt(fmt) "proc: " fmt

#include <arch/cpu.h>
#include <arch/fpu.h>
#include <arch/paging.h>
#include <arch/segments.h>
#include <core/klog.h>
#include <core/panic.h>
#include <lib/errno.h>
#include <lib/string.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <proc/elf.h>
#include <proc/process.h>
#include <proc/uaccess.h>
#include <uapi/syscall.h>

#include "../sched/sched_internal.h"

struct raw_image {
    const char *name;
    const u8 *start;
    u64 size;
};

extern const struct raw_image user_images[];
extern const u64 user_images_count;

bool user_image_find(const char *name, struct user_image *out)
{
    for (u64 i = 0; i < user_images_count; i++) {
        if (strcmp(user_images[i].name, name) == 0) {
            *out =
                (struct user_image){user_images[i].name, user_images[i].start, user_images[i].size};
            return true;
        }
    }
    return false;
}

/* Maps one zeroed page at va, filled from [src, src + len) at offset `skip`. */
static int map_user_page(struct address_space *as, uptr va, const u8 *src, usize skip, usize len,
                         unsigned flags)
{
    u64 frame = pmm_alloc_page();

    if (frame == PMM_ALLOC_FAILED)
        return -ENOMEM;
    u8 *page = phys_to_virt(frame);

    memset(page, 0, PAGE_SIZE_4K);
    if (len)
        memcpy(page + skip, src, len);

    int err = vmm_map(as, va, frame, PAGE_SIZE_4K, flags | VM_USER);

    if (err)
        pmm_free_page(frame);
    return err;
}

/*
 * Copies each segment through the direct map into fresh frames, then maps
 * them with the segment's own permissions. The kernel never writes through
 * the user mapping, so SMAP stays on and read-only segments really are.
 */
static int load_segments(struct address_space *as, const struct elf_image *img, const u8 *file)
{
    for (u32 i = 0; i < img->nsegments; i++) {
        const struct elf_segment *s = &img->segments[i];
        unsigned flags = (s->writable ? VM_WRITE : 0) | (s->executable ? VM_EXEC : 0);
        u64 file_lo = s->vaddr, file_hi = s->vaddr + s->filesz;

        for (u64 page = ALIGN_DOWN(s->vaddr, PAGE_SIZE_4K); page < s->vaddr + s->memsz;
             page += PAGE_SIZE_4K) {
            /* The part of the file-backed range that falls into this page. */
            u64 lo = MAX(page, file_lo), hi = MIN(page + PAGE_SIZE_4K, file_hi);
            usize len = lo < hi ? (usize)(hi - lo) : 0;
            const u8 *src = len ? file + s->offset + (lo - s->vaddr) : NULL;
            int err = map_user_page(as, page, src, len ? (usize)(lo - page) : 0, len, flags);

            if (err)
                return err;
        }
    }
    return 0;
}

static int map_user_stack(struct address_space *as)
{
    for (uptr va = USER_STACK_TOP - USER_STACK_SIZE; va < USER_STACK_TOP; va += PAGE_SIZE_4K) {
        int err = map_user_page(as, va, NULL, 0, 0, VM_WRITE);

        if (err)
            return err;
    }
    return 0;
}

/* Kernel-side start of every process: drop to ring 3 at the ELF entry. */
static int process_start(void *unused)
{
    struct task *self = task_current();
    struct trap_frame frame;

    (void)unused;
    memset(&frame, 0, sizeof(frame));
    frame.rip = self->user_entry;
    frame.cs = GDT_USER_CODE | RPL_USER;
    frame.ss = GDT_USER_DATA | RPL_USER;
    frame.rsp = USER_STACK_TOP; /* 16-byte aligned, as the SysV ABI wants at _start */
    frame.rflags = RFLAGS_IF | RFLAGS_RESERVED;
    enter_user(&frame);
}

int process_spawn(const char *name)
{
    struct user_image image;
    const struct user_image *img = &image;
    struct elf_image elf;
    int err;

    if (!user_image_find(name, &image))
        return -ENOENT;
    err = elf_parse(img->data, img->size, USER_MIN, USER_TOP, &elf);
    if (err)
        return err;

    struct task *t = sched_task_alloc(process_start, NULL, name, &err);

    if (!t)
        return err;
    t->as = vmm_create_user_space();
    t->fpu_state = fpu_state_alloc();
    if (!t->as || !t->fpu_state) {
        sched_task_discard(t);
        return -ENOMEM;
    }
    err = load_segments(t->as, &elf, img->data);
    if (!err)
        err = map_user_stack(t->as);
    if (err) {
        sched_task_discard(t); /* frees the partially built address space too */
        return err;
    }
    t->user_entry = elf.entry;
    sched_admit(t, 0);
    return t->pid;
}

__noreturn void process_kill_on_fault(const struct trap_frame *f)
{
    struct task *self = task_current();

    if (f->vector == VEC_PAGE_FAULT)
        pr_warn("%s[%d]: killed by %s at %#lx (address %#lx, error %#lx)\n", self->name, self->pid,
                trap_name(f->vector), f->rip, read_cr2(), f->error_code);
    else
        pr_warn("%s[%d]: killed by %s at %#lx (error %#lx)\n", self->name, self->pid,
                trap_name(f->vector), f->rip, f->error_code);
    task_exit(EXIT_STATUS_FAULT((int)f->vector));
}

__noreturn void user_bad_iret(const u64 *iret_frame)
{
    struct task *self = task_current();

    if (!(iret_frame[1] & 3))
        panic("iretq to kernel mode faulted (rip %#lx)", iret_frame[0]);
    pr_warn("%s[%d]: killed: cannot return to user rip %#lx (#GP on iretq)\n", self->name,
            self->pid, iret_frame[0]);
    task_exit(EXIT_STATUS_FAULT(VEC_GENERAL_PROTECTION));
}
