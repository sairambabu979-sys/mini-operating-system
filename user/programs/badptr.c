// SPDX-License-Identifier: MIT
/*
 * Hands the kernel hostile arguments. Every call must fail cleanly with
 * the right errno; any other result is counted as a failure. Exit status
 * is the number of failures.
 */
#include "libsys.h"

#include <lib/errno.h>

static int failures;

static void expect(const char *what, long got, long want)
{
    if (got != want) {
        printf("badptr: FAIL %s: got %ld, want %ld\n", what, got, want);
        failures++;
    }
}

static const int read_only = 1; /* nonzero: .rodata, mapped but not writable */
static char big[4096];          /* mapped for far more than read() would ever fill */

int main(void)
{
    char buf[8] = "ok\n";
    int status;

    expect("write(kernel pointer)", sys_write(1, (void *)0xFFFFFFFF80100000ull, 8), -EFAULT);
    expect("write(unmapped)", sys_write(1, (void *)0x1000, 8), -EFAULT);
    expect("write(range wraps)", sys_write(1, buf, ~0ul), -EFAULT);
    expect("write(range crosses USER_TOP)", sys_write(1, (void *)0x7FFFFFFFFFF0ull, 64), -EFAULT);
    expect("write(bad fd)", sys_write(7, buf, 3), -EBADF);
    expect("write(len 0)", sys_write(1, buf, 0), 0);
    expect("spawn(kernel pointer)", sys_spawn((const char *)0xFFFF800000001000ull), -EFAULT);
    expect("spawn(no such image)", sys_spawn("no-such-program"), -ENOENT);
    expect("spawn(name too long)", sys_spawn("aaaaaaaaaaaaaaaaaaaaaaaa"), -ENAMETOOLONG);
    expect("wait(no children)", sys_wait(-1, &status), -ECHILD);
    /* read() rejects all of these before it could block or take a byte. */
    expect("read(bad fd)", sys_read(7, buf, 3), -EBADF);
    expect("read(stdout)", sys_read(1, buf, 3), -EBADF);
    expect("read(len 0)", sys_read(0, buf, 0), 0);
    expect("read(kernel pointer)", sys_read(0, (void *)0xFFFFFFFF80100000ull, 8), -EFAULT);
    expect("read(range wraps)", sys_read(0, buf, ~0ul), -EFAULT);
    expect("read(range crosses USER_TOP)", sys_read(0, (void *)0x7FFFFFFFFFF0ull, 64), -EFAULT);
    /* The range is judged on the whole length, not on the part read() would use. */
    expect("read(valid buffer, length wraps)", sys_read(0, big, ~0ul), -EFAULT);
    expect("read(valid buffer, length past USER_TOP)", sys_read(0, big, 1ull << 47), -EFAULT);
    expect("read(unmapped)", sys_read(0, (void *)0x1000, 8), -EFAULT);
    /* The probe covers every byte read() could fill: here the last 4 run past the stack. */
    expect("read(straddles the stack top)", sys_read(0, (void *)(0x7FFFFFFF0000ull - 4), 8),
           -EFAULT);
    /* The checks come in the documented order: the fd, then the range, then len 0. */
    expect("read(bad fd and kernel pointer)", sys_read(7, (void *)0xFFFFFFFF80100000ull, 8),
           -EBADF);
    expect("read(kernel pointer, len 0)", sys_read(0, (void *)0xFFFFFFFF80100000ull, 0), -EFAULT);
    expect("read(read-only)", sys_read(0, (void *)(unsigned long)&read_only, 4), -EFAULT);
    expect("unknown syscall", syscall3(1000, 0, 0, 0), -ENOSYS);
    expect("negative syscall", syscall3(-1, 0, 0, 0), -ENOSYS);

    long pid = sys_spawn("hello");

    expect("wait(kernel status pointer)", sys_wait((int)pid, (int *)0xFFFFFFFF80100000ull),
           -EFAULT);
    /* In range but unusable: the child must survive these too. */
    expect("wait(unmapped status pointer)", sys_wait((int)pid, (int *)0x10000000ull), -EFAULT);
    expect("wait(read-only status pointer)", sys_wait((int)pid, (int *)(unsigned long)&read_only),
           -EFAULT);
    expect("wait(hello) after rejected waits", sys_wait((int)pid, &status), pid);
    expect("hello's exit status", status, 42);

    printf("badptr: %d failure(s)\n", failures);
    return failures;
}
