/* SPDX-License-Identifier: MIT */
/*
 * User processes: a task with its own address space, loaded from a
 * program in the initrd (bin/NAME).
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
#include <uapi/syscall.h>

#define USER_MIN        0x10000ull
#define USER_STACK_TOP  0x00007FFFFFFF0000ull
#define USER_STACK_SIZE (64ull * 1024)

struct user_image {
    const char *name; /* the caller's string, not a copy */
    const u8 *data;
    u64 size;
};

/*
 * Looks up the program bin/NAME in the initrd. False for no initrd, a name
 * of TASK_NAME_MAX characters or more, or no such file. The image is read
 * in place: the initrd is never freed.
 */
bool user_image_find(const char *name, struct user_image *out);

/* Arguments for a program, after its name (see SPAWN_ARGS_MAX in <uapi/syscall.h>). */
struct spawn_args {
    int argc;                      /* how many */
    usize len;                     /* bytes of strings used, NULs included */
    char strings[SPAWN_ARGS_SIZE]; /* NUL-terminated, back to back */
};

/* Appends one argument: 0, or -E2BIG if it would pass either limit. */
int spawn_args_add(struct spawn_args *a, const char *arg);

struct file;

/*
 * Starts the named program as a child of the current task, as main(argc,
 * argv) with argv[0] the name and `args` (NULL for none) after it, and
 * with std[0..2] as its fds 0-2 (NULL: the console's). Returns its pid,
 * or -ENAMETOOLONG (a name of TASK_NAME_MAX characters or more), -ENOENT
 * (no such image), -ENOEXEC/-EINVAL (bad ELF), -ENOMEM, or -EAGAIN
 * (TASK_MAX tasks exist).
 */
int process_spawn_args(const char *name, const struct spawn_args *args, struct file *const *std);

/* The same, without arguments, on the console. */
int process_spawn(const char *name);

/* A user-mode exception: report it and terminate the process. */
__noreturn void process_kill_on_fault(const struct trap_frame *f);

/* Programs the SYSCALL MSRs. */
void syscall_init(void);

/* Syscall returns that took the iretq path because SYSRET was unsafe. */
extern u64 syscall_slow_exits;

#endif /* MINIOS_PROC_PROCESS_H */
