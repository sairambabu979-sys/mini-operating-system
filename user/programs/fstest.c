// SPDX-License-Identifier: MIT
/*
 * Checks the file calls from ring 3: open, read, close and readdir on the
 * initrd, each error in its documented order, and that a new process
 * starts with no files open ("fstest child"). Any wrong answer is printed
 * and counted. Exit status is the number of failures.
 */
#include "libsys.h"

#include <lib/errno.h>
#include <lib/string.h>

static int failures;

static void expect(const char *what, long got, long want)
{
    if (got != want) {
        printf("fstest: FAIL %s: got %ld, want %ld\n", what, got, want);
        failures++;
    }
}

#define KPTR     ((void *)0xFFFF800000001000ull)
#define UNMAPPED ((void *)0x1000)
#define MOTD     "Welcome to minios.\n"

static const char read_only[16] = "rodata";                /* mapped, not writable */
static const struct dirent read_only_dirent = {.size = 1}; /* nonzero: .rodata, not .bss */
static char too_long[PATH_MAX + 1]; /* "/aaa...", PATH_MAX bytes before its NUL */
static char big[64 * 1024];         /* a whole program, across several read chunks */
extern char _end[];                 /* user/linker.ld: the page after it is unmapped */

/* "fstest child": started with the parent's fd FD_FIRST open, it must see none. */
static int child(void)
{
    char c;

    expect("child: read(FD_FIRST)", sys_read(FD_FIRST, &c, 1), -EBADF);
    expect("child: close(FD_FIRST)", sys_close(FD_FIRST), -EBADF);
    return failures;
}

static void bad_paths(void)
{
    too_long[0] = '/';
    for (usize i = 1; i < PATH_MAX; i++)
        too_long[i] = 'a';
    expect("open(kernel pointer)", sys_open(KPTR), -EFAULT);
    expect("open(unmapped)", sys_open(UNMAPPED), -EFAULT);
    expect("open(no NUL in PATH_MAX bytes)", sys_open(too_long), -ENAMETOOLONG);
    expect("open(relative)", sys_open("etc/motd"), -EINVAL);
    expect("open(..)", sys_open("/bin/../etc/motd"), -EINVAL);
    expect("open(missing)", sys_open("/etc/nope"), -ENOENT);
    expect("open(a directory)", sys_open("/bin"), -EISDIR);
    expect("open(the root)", sys_open("/"), -EISDIR);
    expect("open(a file with a trailing /)", sys_open("/etc/motd/"), -ENOTDIR);
    expect("open(below a file)", sys_open("/etc/motd/x"), -ENOTDIR);
}

static void reads(void)
{
    char buf[128]; /* more than all of /etc/motd */
    long fd = sys_open("/etc/motd"), n;

    expect("open(/etc/motd) is the first file fd", fd, FD_FIRST);
    expect("read(len 0)", sys_read((int)fd, buf, 0), 0);
    expect("read(kernel pointer)", sys_read((int)fd, KPTR, 8), -EFAULT);
    expect("read(range wraps)", sys_read((int)fd, buf, ~0ul), -EFAULT);
    expect("read(read-only buffer)", sys_read((int)fd, (void *)read_only, 8), -EFAULT);
    /* none of those moved the position: the file still starts at its first byte */
    n = sys_read((int)fd, buf, 9);
    expect("read 9", n, 9);
    expect("read 9: the first 9 bytes", n == 9 && memcmp(buf, MOTD, 9) == 0, 1);
    n = sys_read((int)fd, buf + 9, sizeof(buf) - 9);
    expect("read the rest: starts where the last read ended",
           n > 10 && memcmp(buf, MOTD, strlen(MOTD)) == 0, 1);
    expect("read at the end", sys_read((int)fd, buf, sizeof(buf)), 0);
    expect("write(a file)", sys_write((int)fd, "x", 1), -EBADF);
    expect("close", sys_close((int)fd), 0);
    expect("close again", sys_close((int)fd), -EBADF);
    expect("read(closed)", sys_read((int)fd, buf, 1), -EBADF);
    expect("close(0)", sys_close(0), -EBADF);
    expect("close(1)", sys_close(1), -EBADF);
    expect("read(1)", sys_read(1, buf, 1), -EBADF);
    expect("close(past the table)", sys_close(FD_FIRST + FILES_MAX), -EBADF);

    struct dirent d;
    long total = 0;

    fd = sys_open("/bin/sh");
    expect("open(/bin/sh)", fd, FD_FIRST);
    while ((n = sys_read((int)fd, big + total, sizeof(big) - (usize)total)) > 0)
        total += n;
    expect("read(/bin/sh) to the end", n, 0);
    bool listed = false;

    for (u64 i = 0; sys_readdir("/bin", i, &d) > 0; i++) {
        if (strcmp(d.name, "sh") == 0) {
            listed = true;
            expect("read(/bin/sh): every byte readdir counts", total, (long)d.size);
        }
    }
    expect("readdir(/bin) lists sh", listed, 1);
    expect("read(/bin/sh): an ELF", total > 4 && big[0] == 0x7f && big[1] == 'E', 1);
    expect("read(/bin/sh): more than one page", total > 4096, 1);
    sys_close((int)fd);

    /*
     * A buffer that runs into an unmapped page: the read stops exactly
     * there, and the next goes on from the first byte not delivered.
     */
    char *edge = (char *)(((u64)_end + 4095) & ~4095ull);

    fd = sys_open("/bin/sh");
    expect("read(200 bytes, 100 before an unmapped page)", sys_read((int)fd, edge - 100, 200), 100);
    expect("read: those are the first 100 bytes", memcmp(edge - 100, big, 100) == 0, 1);
    expect("read: goes on from byte 100", sys_read((int)fd, buf, 8), 8);
    expect("read: bytes 100 to 107", memcmp(buf, big + 100, 8) == 0, 1);
    expect("read(all of it unmapped)", sys_read((int)fd, edge, 8), -EFAULT);
    expect("read: the fault moved nothing", sys_read((int)fd, buf, 8), 8);
    expect("read: bytes 108 to 115", memcmp(buf, big + 108, 8) == 0, 1);
    sys_close((int)fd);
}

static void fd_table(void)
{
    long fds[FILES_MAX];

    for (int i = 0; i < FILES_MAX; i++) {
        fds[i] = sys_open("/etc/motd");
        expect("open: the lowest free fd", fds[i], FD_FIRST + i);
    }
    expect("open(one too many)", sys_open("/etc/motd"), -EMFILE);
    expect("close(a middle one)", sys_close((int)fds[2]), 0);
    expect("open: reuses it", sys_open("/etc/motd"), fds[2]);
    for (int i = 0; i < FILES_MAX; i++)
        expect("close all", sys_close((int)fds[i]), 0);
}

static void listings(void)
{
    struct dirent d;
    bool bin = false, etc = false;
    long r;

    for (u64 i = 0; (r = sys_readdir("/", i, &d)) > 0; i++) {
        bin |= strcmp(d.name, "bin") == 0 && d.type == DIRENT_DIR && d.size == 0;
        etc |= strcmp(d.name, "etc") == 0 && d.type == DIRENT_DIR;
    }
    expect("readdir(/) ends with 0", r, 0);
    expect("readdir(/): bin/ and etc/", bin && etc, 1);
    expect("readdir(/etc, 0) is a file", sys_readdir("/etc", 0, &d) == 1 && d.type == DIRENT_FILE,
           1);
    expect("readdir(far past the end)", sys_readdir("/bin", 1000000, &d), 0);
    expect("readdir(a file)", sys_readdir("/etc/motd", 0, &d), -ENOTDIR);
    expect("readdir(missing)", sys_readdir("/nope", 0, &d), -ENOENT);
    expect("readdir(relative)", sys_readdir("bin", 0, &d), -EINVAL);
    expect("readdir(path unmapped)", sys_readdir(UNMAPPED, 0, &d), -EFAULT);
    expect("readdir(no NUL in PATH_MAX bytes)", sys_readdir(too_long, 0, &d), -ENAMETOOLONG);
    expect("readdir(a file with a trailing /)", sys_readdir("/etc/motd/", 0, &d), -ENOTDIR);
    expect("readdir(path at a kernel pointer)", sys_readdir(KPTR, 0, &d), -EFAULT);
    expect("readdir(out at a kernel pointer)", sys_readdir("/", 0, KPTR), -EFAULT);
    expect("readdir(out at a kernel pointer, path missing): the pointer first",
           sys_readdir("/nope", 0, KPTR), -EFAULT);
    expect("readdir(out unmapped)", sys_readdir("/", 0, UNMAPPED), -EFAULT);
    expect("readdir(out read-only)", sys_readdir("/", 0, (struct dirent *)&read_only_dirent),
           -EFAULT);
}

int main(int argc, char **argv)
{
    static const char *const child_args[] = {"child", NULL};
    int status = -1;

    if (argc == 2 && strcmp(argv[1], "child") == 0)
        return child();
    bad_paths();
    reads();
    fd_table();
    listings();

    long fd = sys_open("/etc/motd"), pid;

    expect("open before spawn", fd, FD_FIRST);
    pid = sys_spawnv("fstest", child_args);
    expect("child pid", pid > 0, 1);
    expect("wait(child)", sys_wait((int)pid, &status), pid);
    expect("child: no files inherited", status, 0);
    sys_close((int)fd);
    return failures;
}
