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

#define KPTR ((const char *)0xFFFF800000001000ull)

static const char *const kernel_arg[] = {KPTR, NULL};
static const char *const nine_args[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", NULL};
static const char *const one_arg[] = {"x", NULL};
static const char *const big_arg[] = {big, NULL}; /* filled with 256 'a's: one byte too many */
static char full[SPAWN_ARGS_MAX][SPAWN_ARGS_SIZE / SPAWN_ARGS_MAX]; /* 8 x 31 characters + NUL */
static const char *full_args[SPAWN_ARGS_MAX + 1];

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
    /* Bad arguments fail before anything is created: wait(no children) below checks it. */
    for (int i = 0; i < SPAWN_ARGS_SIZE; i++)
        big[i] = 'a';
    expect("spawn(args at a kernel pointer)", sys_spawnv("hello", (const char *const *)KPTR),
           -EFAULT);
    expect("spawn(an argument at a kernel pointer)", sys_spawnv("hello", kernel_arg), -EFAULT);
    expect("spawn(9 arguments)", sys_spawnv("hello", nine_args), -E2BIG);
    expect("spawn(257 bytes of arguments)", sys_spawnv("hello", big_arg), -E2BIG);
    expect("spawn(bad name, then bad arguments)", sys_spawnv(KPTR, nine_args), -EFAULT);
    expect("spawn(no such image, with arguments)", sys_spawnv("no-such-program", one_arg), -ENOENT);
    big[0] = '\0';
    /* Exactly both limits: 8 arguments, 256 bytes in all. It must run. */
    for (int i = 0; i < SPAWN_ARGS_MAX; i++) {
        for (usize j = 0; j + 1 < sizeof(full[i]); j++)
            full[i][j] = (char)('a' + i);
        full_args[i] = full[i];
    }
    long full_pid = sys_spawnv("echo", full_args);

    expect("spawn(8 arguments, 256 bytes)", full_pid > 0, 1);
    if (full_pid > 0)
        expect("wait(that echo)", sys_wait((int)full_pid, &status), full_pid);
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
