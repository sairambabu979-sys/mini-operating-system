// SPDX-License-Identifier: MIT
/*
 * System call table and implementations. ABI: <uapi/syscall.h>.
 *
 * Every argument is untrusted. Pointers go through <proc/uaccess.h>,
 * never dereferenced directly; sizes are bounded before any work is done.
 */
#define pr_fmt(fmt) "syscall: " fmt

#include <arch/cpu.h>
#include <arch/msr.h>
#include <arch/segments.h>
#include <arch/trap.h>
#include <core/input.h>
#include <core/printk.h>
#include <core/time.h>
#include <fs/initrd.h>
#include <fs/tarfs.h>
#include <lib/errno.h>
#include <proc/process.h>
#include <proc/uaccess.h>
#include <sched/sched.h>
#include <uapi/syscall.h>

#define WRITE_CHUNK 256
#define READ_CHUNK  4096 /* file reads copy out this much at a time */
#define NAME_MAX    TASK_NAME_MAX

typedef s64 (*syscall_fn)(u64 a0, u64 a1, u64 a2);

void syscall_entry(void);
void syscall_dispatch(struct trap_frame *f);

static s64 sys_exit(u64 code, u64 a1, u64 a2)
{
    (void)a1;
    (void)a2;
    task_exit((int)code);
}

static s64 sys_write(u64 fd, u64 user_buf, u64 len)
{
    char chunk[WRITE_CHUNK];

    if (fd != 1 && fd != 2)
        return -EBADF;
    if (!user_range_ok(user_buf, len))
        return -EFAULT;
    for (u64 done = 0; done < len;) {
        usize n = (usize)MIN(len - done, (u64)WRITE_CHUNK);

        if (copy_from_user(chunk, (const void *)(user_buf + done), n) != 0)
            return done ? (s64)done : -EFAULT;
        console_write(chunk, n);
        done += n;
    }
    return (s64)len;
}

static s64 sys_yield(u64 a0, u64 a1, u64 a2)
{
    (void)a0;
    (void)a1;
    (void)a2;
    sched_yield();
    return 0;
}

static s64 sys_getpid(u64 a0, u64 a1, u64 a2)
{
    (void)a0;
    (void)a1;
    (void)a2;
    return task_current()->pid;
}

static s64 sys_sleep_ms(u64 ms, u64 a1, u64 a2)
{
    (void)a1;
    (void)a2;
    sleep_ms(MIN(ms, 3600ull * 1000)); /* cap at an hour: no tick-count overflow games */
    return 0;
}

/*
 * spawn(name, args): the name first (-EFAULT, -ENAMETOOLONG), then each
 * argument pointer and string in turn (-EFAULT, or -E2BIG past either
 * limit), all before anything is created.
 */
static s64 sys_spawn(u64 user_name, u64 user_args, u64 a2)
{
    char name[NAME_MAX];
    struct spawn_args args = {0};
    int len;

    (void)a2;
    len = strncpy_from_user(name, (const char *)user_name, sizeof(name));
    if (len < 0)
        return len;
    if (!user_args)
        return process_spawn(name);
    for (usize i = 0;; i++) {
        u64 arg;

        if (copy_from_user(&arg, (const void *)(user_args + i * sizeof(u64)), sizeof(arg)) != 0)
            return -EFAULT;
        if (!arg)
            break;
        if (args.argc == SPAWN_ARGS_MAX)
            return -E2BIG;
        len = strncpy_from_user(args.strings + args.len, (const char *)arg,
                                sizeof(args.strings) - args.len);
        if (len == -ENAMETOOLONG)
            return -E2BIG;
        if (len < 0)
            return len;
        args.len += (usize)len + 1;
        args.argc++;
    }
    return process_spawn_args(name, &args);
}

static s64 sys_wait(u64 pid, u64 user_status, u64 a2)
{
    int status = 0;

    (void)a2;
    /*
     * A bad pointer must not consume the child, and "in the user range" is
     * not enough: the page may be unmapped or read-only. So the status is
     * written once before anything is reaped (a waitpid() out-parameter, so
     * writing 0 is harmless). Processes are single-threaded: nothing can
     * unmap it between this write and the one after the wait.
     */
    if (user_status && copy_to_user((void *)user_status, &status, sizeof(status)) != 0)
        return -EFAULT;
    int r = task_wait((int)pid, &status);

    if (r > 0 && user_status && copy_to_user((void *)user_status, &status, sizeof(status)) != 0)
        return -EFAULT;
    return r;
}

/* The open file behind fd, or NULL: fds below FD_FIRST are the console's. */
static struct task_file *file_of(u64 fd)
{
    struct task *self = task_current();
    struct task_file *f;

    /* No test can see an off-by-one here (one past the table is the task's next field). */
    if (fd < FD_FIRST || fd - FD_FIRST >= ARRAY_SIZE(self->files))
        return NULL;
    f = &self->files[fd - FD_FIRST];
    return f->open ? f : NULL;
}

/* Copies a path in: -EFAULT, or -ENAMETOOLONG at PATH_MAX bytes with no NUL. */
static int path_from_user(char path[PATH_MAX], u64 user_path)
{
    int len = strncpy_from_user(path, (const char *)user_path, PATH_MAX);

    return len < 0 ? len : 0;
}

/* Looks a path up in the initrd (none: nothing exists). */
static int lookup(const char *path, struct tarfs_node *node)
{
    usize len;
    const u8 *rd = initrd_get(&len);

    return rd ? tarfs_lookup(rd, len, path, node) : -ENOENT;
}

/*
 * read(fd >= FD_FIRST, ...): from the file's position, as much as fits,
 * 0 at its end. -EFAULT for the range, then 0 for len 0; an unwritable
 * page part-way through ends the read there (-EFAULT if nothing was
 * copied), and the position moves by exactly what was copied, so no byte
 * is lost.
 */
static s64 read_file(struct task_file *f, u64 user_buf, u64 len)
{
    u64 want, done = 0;

    if (!user_range_ok(user_buf, len))
        return -EFAULT;
    want = MIN(len, f->size - f->pos);
    while (done < want) {
        usize n = (usize)MIN(want - done, (u64)READ_CHUNK);

        if (copy_to_user((void *)(user_buf + done), f->data + f->pos + done, n) != 0)
            break;
        done += n;
    }
    f->pos += done;
    return done || want == 0 ? (s64)done : -EFAULT;
}

/*
 * read(0, buf, len): console input, raw bytes, at least one. The order of
 * the checks is part of the ABI: -EBADF, then -EFAULT for the range, then
 * 0 for an empty read, then -EFAULT for a buffer that is not mapped and
 * writable, then -ENODEV with no input device. Every failure comes before
 * anything is taken off the queue, and the buffer is probed, its contents
 * kept, rather than merely range-checked (the sys_wait precedent): a bad
 * buffer must not swallow a keystroke. The process is single-threaded, so
 * nothing can unmap the buffer between the probe and the copy out.
 * A file's fd reads the file (read_file).
 */
static s64 sys_read(u64 fd, u64 user_buf, u64 len)
{
    u8 buf[INPUT_READ_MAX];
    usize n;
    struct task_file *f = file_of(fd);

    if (f)
        return read_file(f, user_buf, len);
    if (fd != 0)
        return -EBADF;
    if (!user_range_ok(user_buf, len))
        return -EFAULT;
    n = (usize)MIN(len, (u64)INPUT_READ_MAX);
    if (n == 0)
        return 0;
    if (copy_from_user(buf, (const void *)user_buf, n) != 0 ||
        copy_to_user((void *)user_buf, buf, n) != 0)
        return -EFAULT;
    if (!input_present())
        return -ENODEV;
    n = input_read(buf, n);
    if (copy_to_user((void *)user_buf, buf, n) != 0)
        return -EFAULT; /* cannot happen: probed above */
    return (s64)n;
}

/*
 * open(path): the path first (-EFAULT, -ENAMETOOLONG), then the lookup
 * (-EINVAL, -ENOENT), -EISDIR for a directory, -EMFILE with FILES_MAX
 * open. The lowest free fd wins.
 */
static s64 sys_open(u64 user_path, u64 a1, u64 a2)
{
    char path[PATH_MAX];
    struct tarfs_node node;
    struct task *self = task_current();
    int err;

    (void)a1;
    (void)a2;
    err = path_from_user(path, user_path);
    if (!err)
        err = lookup(path, &node);
    if (err)
        return err;
    if (node.is_dir)
        return -EISDIR;
    for (int i = 0; i < FILES_MAX; i++) {
        if (!self->files[i].open) {
            self->files[i] = (struct task_file){node.data, node.size, 0, true};
            return FD_FIRST + i;
        }
    }
    return -EMFILE;
}

static s64 sys_close(u64 fd, u64 a1, u64 a2)
{
    struct task_file *f = file_of(fd);

    (void)a1;
    (void)a2;
    if (!f)
        return -EBADF; /* the console's fds included: they stay open */
    f->open = false;
    return 0;
}

/*
 * readdir(path, index, out): the path (-EFAULT, -ENAMETOOLONG), then
 * -EFAULT unless `out` is in the user range, then the listing's errors
 * (-EINVAL, -ENOENT, -ENOTDIR). 1 with *out written, or 0 past the end.
 */
static s64 sys_readdir(u64 user_path, u64 index, u64 user_out)
{
    char path[PATH_MAX];
    struct dirent d;
    usize len;
    const u8 *rd = initrd_get(&len);
    int r = path_from_user(path, user_path);

    if (r)
        return r;
    if (!user_range_ok(user_out, sizeof(d)))
        return -EFAULT;
    r = rd ? tarfs_readdir(rd, len, path, index, &d) : -ENOENT;
    if (r == 1 && copy_to_user((void *)user_out, &d, sizeof(d)) != 0)
        return -EFAULT;
    return r;
}

static s64 sys_time_ms(u64 a0, u64 a1, u64 a2)
{
    (void)a0;
    (void)a1;
    (void)a2;
    return (s64)(time_ns() / 1000000);
}

static const syscall_fn syscall_table[SYS_COUNT] = {
    [SYS_EXIT] = sys_exit,     [SYS_WRITE] = sys_write,       [SYS_YIELD] = sys_yield,
    [SYS_GETPID] = sys_getpid, [SYS_SLEEP_MS] = sys_sleep_ms, [SYS_SPAWN] = sys_spawn,
    [SYS_WAIT] = sys_wait,     [SYS_TIME_MS] = sys_time_ms,   [SYS_READ] = sys_read,
    [SYS_OPEN] = sys_open,     [SYS_CLOSE] = sys_close,       [SYS_READDIR] = sys_readdir,
};

/* Called from syscall.asm with interrupts enabled. Result goes in RAX. */
void syscall_dispatch(struct trap_frame *f)
{
    u64 nr = f->rax;

    if (nr >= SYS_COUNT || !syscall_table[nr]) {
        f->rax = (u64)-ENOSYS;
        return;
    }
    f->rax = (u64)syscall_table[nr](f->rdi, f->rsi, f->rdx);
}

void syscall_init(void)
{
    /*
     * STAR[47:32]: SYSCALL loads CS = 0x08, SS = 0x10.
     * STAR[63:48]: SYSRET loads CS = 0x18 + 16 = 0x28 (|3), SS = 0x18 + 8 = 0x20 (|3).
     */
    wrmsr(MSR_STAR, ((u64)(GDT_USER_CODE32 | RPL_USER) << 48) | ((u64)GDT_KERNEL_CODE << 32));
    wrmsr(MSR_LSTAR, (u64)(uptr)syscall_entry);
    /*
     * Every flag ring 3 can set is cleared on entry; sysret restores the
     * user's own from R11. Above all IF (off until the stack switch), DF,
     * TF, AC (SMAP stays on) and NT: the kernel does not save RFLAGS across
     * context switches, so a user's NT would reach the next iretq of some
     * other task and #GP it, kernel threads included.
     */
    wrmsr(MSR_FMASK, RFLAGS_CF | RFLAGS_PF | RFLAGS_AF | RFLAGS_ZF | RFLAGS_SF | RFLAGS_TF |
                         RFLAGS_IF | RFLAGS_DF | RFLAGS_OF | RFLAGS_IOPL | RFLAGS_NT | RFLAGS_RF |
                         RFLAGS_AC | RFLAGS_ID);
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}
