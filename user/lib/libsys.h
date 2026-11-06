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
/* And with stdio[0..2] as the child's fds 0-2 (NULL: this process's own). */
long sys_spawn_stdio(const char *name, const char *const *args, const int *stdio);
long sys_wait(int pid, int *status);
long sys_time_ms(void);
long sys_read(int fd, void *buf, usize len);
long sys_open(const char *path, int flags);
long sys_unlink(const char *path);
long sys_close(int fd);
long sys_readdir(const char *path, u64 index, struct dirent *out);
long sys_fstat(int fd, struct stat *out);
long sys_pipe(int fds[2]);

/*
 * Standard input for a program that reads it to its end (cat, wc):
 * read(0), unless fd 0 is the console, which has no end and never
 * echoes. There input is edited a line at a time (lineedit.h: at most
 * LE_LINE_MAX - 1 characters, more are ignored), echoed to fd 2 if that
 * is the console too, and handed over line by line, each with its "\n";
 * Ctrl-D on an empty line ends it, and every call after that returns 0.
 * An LF first of all is taken as the end of the CRLF that ended the
 * shell's line. -EIO if the console read fails without saying why.
 */
long stdin_read(void *buf, usize len);

/* To fd 1, and to fd 2 (errors, so they stay out of a pipe): one write each, 255 bytes at most. */
int printf(const char *fmt, ...) __printf(1, 2);
int eprintf(const char *fmt, ...) __printf(1, 2);

/* A short description of a negative errno, for messages ("not found"). */
const char *sys_strerror(long err);

#endif /* MINIOS_USER_LIBSYS_H */
