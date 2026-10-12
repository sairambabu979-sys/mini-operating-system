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
long sys_wait(int pid, int *status);
long sys_time_ms(void);

int printf(const char *fmt, ...) __printf(1, 2);

#endif /* MINIOS_USER_LIBSYS_H */
