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
    expect("unknown syscall", syscall3(1000, 0, 0, 0), -ENOSYS);
    expect("negative syscall", syscall3(-1, 0, 0, 0), -ENOSYS);

    long pid = sys_spawn("hello");

    expect("wait(kernel status pointer)", sys_wait((int)pid, (int *)0xFFFFFFFF80100000ull),
           -EFAULT);
    expect("wait(hello) after a rejected wait", sys_wait((int)pid, &status), pid);
    expect("hello's exit status", status, 42);

    printf("badptr: %d failure(s)\n", failures);
    return failures;
}
