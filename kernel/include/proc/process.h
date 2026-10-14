/* SPDX-License-Identifier: MIT */
/*
 * User processes: a task with its own address space, loaded from one of
 * the program images embedded in the kernel.
 *
 * User address-space layout:
 *
 *   0x0000000000010000 -        program segments (from the ELF; NULL page unmapped)
 *   0x00007FFFFFFE0000 - ...F0000  user stack, 64 KiB, unmapped guard below
 *   0x00007FFFFFFF0000 - USER_TOP  free (programs may place segments here)
 */
#ifndef MINIOS_PROC_PROCESS_H
#define MINIOS_PROC_PROCESS_H

#include <arch/trap.h>
#include <compiler.h>
#include <sched/sched.h>
#include <types.h>

#define USER_MIN        0x10000ull
#define USER_STACK_TOP  0x00007FFFFFFF0000ull
#define USER_STACK_SIZE (64ull * 1024)

struct user_image {
    const char *name;
    const u8 *data;
    u64 size;
};

/* Looks up an image embedded at build time (build/<config>/gen/user_images.asm). */
bool user_image_find(const char *name, struct user_image *out);

/*
 * Starts the named program as a child of the current task. Returns its
 * pid, or -ENOENT (no such image), -ENOEXEC/-EINVAL (bad ELF), -ENOMEM.
 */
int process_spawn(const char *name);

/* A user-mode exception: report it and terminate the process. */
__noreturn void process_kill_on_fault(const struct trap_frame *f);

/* Programs the SYSCALL MSRs. */
void syscall_init(void);

/* Syscall returns that took the iretq path because SYSRET was unsafe. */
extern u64 syscall_slow_exits;

#endif /* MINIOS_PROC_PROCESS_H */
