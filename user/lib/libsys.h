/* SPDX-License-Identifier: MIT */
/*
 * Minimal user-space runtime: system call wrappers and printf.
 */
#ifndef MINIOS_USER_LIBSYS_H
#define MINIOS_USER_LIBSYS_H

#include <compiler.h>
#include <types.h>
#include <uapi/syscall.h>

static inline long syscall3(long nr, long a0, long a1, long a2)
{
    long ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory");
    return ret;
}

__noreturn void sys_exit(int code);
long sys_write(int fd, const void *buf, usize len);
long sys_yield(void);
long sys_getpid(void);
long sys_sleep_ms(u64 ms);
long sys_spawn(const char *name);
/* With arguments: `args` is NULL-terminated; argv[0] is the name (see SPAWN_ARGS_MAX). */
long sys_spawnv(const char *name, const char *const *args);
long sys_wait(int pid, int *status);
long sys_time_ms(void);
long sys_read(int fd, void *buf, usize len);

int printf(const char *fmt, ...) __printf(1, 2);

#endif /* MINIOS_USER_LIBSYS_H */
