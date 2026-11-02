// SPDX-License-Identifier: MIT
#include "libsys.h"

#include <lib/printf.h>

__noreturn void sys_exit(int code)
{
    syscall3(SYS_EXIT, code, 0, 0);
    __builtin_unreachable();
}

long sys_write(int fd, const void *buf, usize len)
{
    return syscall3(SYS_WRITE, fd, (long)buf, (long)len);
}

long sys_yield(void)
{
    return syscall3(SYS_YIELD, 0, 0, 0);
}

long sys_getpid(void)
{
    return syscall3(SYS_GETPID, 0, 0, 0);
}

long sys_sleep_ms(u64 ms)
{
    return syscall3(SYS_SLEEP_MS, (long)ms, 0, 0);
}

long sys_spawnv(const char *name, const char *const *args)
{
    return syscall3(SYS_SPAWN, (long)name, (long)args, 0);
}

long sys_spawn(const char *name)
{
    return sys_spawnv(name, NULL);
}

long sys_wait(int pid, int *status)
{
    return syscall3(SYS_WAIT, pid, (long)status, 0);
}

long sys_time_ms(void)
{
    return syscall3(SYS_TIME_MS, 0, 0, 0);
}

long sys_read(int fd, void *buf, usize len)
{
    return syscall3(SYS_READ, fd, (long)buf, (long)len);
}

long sys_open(const char *path)
{
    return syscall3(SYS_OPEN, (long)path, 0, 0);
}

long sys_close(int fd)
{
    return syscall3(SYS_CLOSE, fd, 0, 0);
}

long sys_fstat(int fd, struct fs_stat *out)
{
    return syscall3(SYS_FSTAT, fd, (long)out, 0);
}

long sys_readdir(int fd, struct fs_dirent *out)
{
    return syscall3(SYS_READDIR, fd, (long)out, 0);
}

long sys_seek(int fd, s64 offset, int whence)
{
    return syscall3(SYS_SEEK, fd, (long)offset, whence);
}

/* One write per printf call, so concurrent processes never interleave mid-line. */
int printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    int n = k_vsnprintf(buf, sizeof(buf), fmt, ap);

    va_end(ap);
    sys_write(1, buf, (usize)MIN(n, (int)sizeof(buf) - 1));
    return n;
}
