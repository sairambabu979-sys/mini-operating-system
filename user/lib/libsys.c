// SPDX-License-Identifier: MIT
#include "libsys.h"

#include "lineedit.h"

#include <lib/errno.h>
#include <lib/printf.h>
#include <lib/string.h>

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

long sys_spawn_stdio(const char *name, const char *const *args, const int *stdio)
{
    return syscall3(SYS_SPAWN, (long)name, (long)args, (long)stdio);
}

long sys_spawnv(const char *name, const char *const *args)
{
    return sys_spawn_stdio(name, args, NULL);
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

long sys_open(const char *path, int flags)
{
    return syscall3(SYS_OPEN, (long)path, flags, 0);
}

long sys_unlink(const char *path)
{
    return syscall3(SYS_UNLINK, (long)path, 0, 0);
}

long sys_close(int fd)
{
    return syscall3(SYS_CLOSE, fd, 0, 0);
}

long sys_readdir(const char *path, u64 index, struct dirent *out)
{
    return syscall3(SYS_READDIR, (long)path, (long)index, (long)out);
}

long sys_fstat(int fd, struct stat *out)
{
    return syscall3(SYS_FSTAT, fd, (long)out, 0);
}

long sys_pipe(int fds[2])
{
    return syscall3(SYS_PIPE, (long)fds, 0, 0);
}

static bool is_console(int fd)
{
    struct stat st = {0};

    return sys_fstat(fd, &st) == 0 && st.type == STAT_CONSOLE;
}

long stdin_read(void *buf, usize len)
{
    static enum { UNKNOWN, CONSOLE, OTHER } kind = UNKNOWN;
    static bool echo, ended;
    static struct lineedit le;
    static char line[LE_LINE_MAX]; /* a line of at most LE_LINE_MAX - 1 characters, and its \n */
    static usize line_len, line_off;

    if (kind == UNKNOWN) {
        kind = is_console(0) ? CONSOLE : OTHER;
        echo = is_console(2);
        le_reset(&le);
        /*
         * The shell ends a line at its CR and starts the program at once, so
         * the LF of a terminal's CRLF may still be queued. As the CR's pair
         * it is no line of ours: a first byte LF is skipped, like any LF
         * after a CR.
         */
        le.last_cr = true;
    }
    if (kind == OTHER)
        return sys_read(0, buf, len);
    if (len == 0)
        return 0;
    while (line_off == line_len && !ended) {
        char out[LE_ECHO_MAX];
        usize out_len;
        u8 c;
        long got = sys_read(0, &c, 1); /* a byte at a time: what follows is left queued */

        if (got != 1)
            return got < 0 ? got : -EIO;
        enum le_event ev = le_feed(&le, c, out, &out_len);

        if (echo && out_len)
            sys_write(2, out, out_len);
        if (ev == LE_DONE) {
            memcpy(line, le.buf, le.len);
            line[le.len] = '\n';
            line_len = le.len + 1;
            line_off = 0;
        }
        ended = ev == LE_EOF;
    }
    if (line_off == line_len)
        return 0;
    usize n = MIN(len, line_len - line_off);

    memcpy(buf, line + line_off, n);
    line_off += n;
    return (long)n;
}

const char *sys_strerror(long err)
{
    switch (-err) {
    case ENOENT:
        return "not found";
    case EISDIR:
        return "is a directory";
    case ENOTDIR:
        return "not a directory";
    case EINVAL:
        return "bad path (absolute, no empty, . or .. parts)";
    case ENAMETOOLONG:
        return "path too long";
    case EMFILE:
        return "too many open files";
    case EROFS:
        return "read-only file system";
    case ENOSPC:
        return "no space left";
    case EFAULT:
        return "bad address";
    case EBADF:
        return "bad file descriptor";
    case EPIPE:
        return "broken pipe";
    case ENOMEM:
        return "out of memory";
    default:
        return "error";
    }
}

/*
 * One write per call, so concurrent processes never interleave mid-line
 * (on a pipe too: 256 bytes is PIPE_ATOMIC, and the NUL is not written).
 */
static int vdprintf(int fd, const char *fmt, va_list ap)
{
    char buf[256];
    int n = k_vsnprintf(buf, sizeof(buf), fmt, ap);

    sys_write(fd, buf, (usize)MIN(n, (int)sizeof(buf) - 1));
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    int n = vdprintf(1, fmt, ap);

    va_end(ap);
    return n;
}

int eprintf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    int n = vdprintf(2, fmt, ap);

    va_end(ap);
    return n;
}
