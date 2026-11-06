// SPDX-License-Identifier: MIT
/*
 * Checks writable files from ring 3: open()'s flags and the order of its
 * errors, the initrd refusing writes (-EROFS), /tmp's files made, written,
 * read, emptied, appended to and removed, a removed file that stays open,
 * names and paths, the space and file limits (-ENOSPC), buffers that fail
 * without losing a byte, and programs started with a /tmp file as stdin
 * and stdout. Leaves /tmp empty. Any wrong answer is printed (to fd 2) and
 * counted. Exit status is the number of failures. "tmptest fill" writes
 * /tmp/keep and exits, for the kernel's own test.
 */
#include "libsys.h"

#include <lib/errno.h>
#include <lib/printf.h>
#include <lib/string.h>

static int failures;

static void expect(const char *what, long got, long want)
{
    char line[160];

    if (got != want) {
        int n = k_snprintf(line, sizeof(line), "tmptest: FAIL %s: got %ld, want %ld\n", what, got,
                           want);

        sys_write(2, line, (usize)MIN(n, (int)sizeof(line) - 1));
        failures++;
    }
}

#define KPTR     ((void *)0xFFFF800000001000ull)
#define UNMAPPED ((void *)0x1000)
#define WR_NEW   (O_WRONLY | O_CREAT | O_TRUNC)

static const char read_only[16] = "rodata"; /* mapped, not writable */
static char big[16 * 1024];
extern char _end[]; /* user/linker.ld: the page after it is unmapped */

/* The whole of a file, read through a fresh fd, into buf: its length, or the error. */
static long slurp(const char *path, char *buf, usize max)
{
    long fd = sys_open(path, O_RDONLY), total = 0, n;

    if (fd < 0)
        return fd;
    while ((n = sys_read((int)fd, buf + total, max - (usize)total)) > 0)
        total += n;
    sys_close((int)fd);
    return n < 0 ? n : total;
}

/* Makes `path` hold exactly `text`. */
static void put(const char *path, const char *text)
{
    long fd = sys_open(path, WR_NEW), len = (long)strlen(text);

    expect("put: open", fd >= FD_FIRST, 1);
    expect("put: write", sys_write((int)fd, text, (usize)len), len);
    sys_close((int)fd);
}

static void same(const char *what, const char *path, const char *want)
{
    char buf[128];
    long n = slurp(path, buf, sizeof(buf));

    expect(what, n == (long)strlen(want) && memcmp(buf, want, (usize)n) == 0, 1);
}

static void flags(void)
{
    expect("open(mode 3)", sys_open("/tmp/x", 3), -EINVAL);
    expect("open(O_TRUNC, read-only)", sys_open("/tmp/x", O_RDONLY | O_TRUNC), -EINVAL);
    expect("open(O_APPEND, read-only)", sys_open("/tmp/x", O_RDONLY | O_APPEND), -EINVAL);
    expect("open(an unknown flag)", sys_open("/tmp/x", O_WRONLY | 0x8000), -EINVAL);
    expect("open(flags -1)", sys_open("/tmp/x", -1), -EINVAL);
    expect("open(bad path, bad flags): the path first", sys_open(KPTR, 3), -EFAULT);
    expect("open(missing, bad flags): the flags first", sys_open("/nope/x", 3), -EINVAL);
}

static void initrd(void)
{
    long fd;

    expect("open(an initrd file, O_WRONLY)", sys_open("/etc/motd", O_WRONLY), -EROFS);
    expect("open(an initrd file, O_RDWR)", sys_open("/etc/motd", O_RDWR), -EROFS);
    expect("open(an initrd file, to empty)", sys_open("/etc/motd", WR_NEW), -EROFS);
    expect("open(a new file in /etc)", sys_open("/etc/new", WR_NEW), -EROFS);
    expect("open(a new file at the root)", sys_open("/new", WR_NEW), -EROFS);
    expect("open(a new file below nothing)", sys_open("/nodir/x", WR_NEW), -ENOENT);
    expect("open(a new file below a file)", sys_open("/etc/motd/x", WR_NEW), -ENOTDIR);
    expect("open(a directory, to write): -EISDIR first", sys_open("/bin", O_WRONLY), -EISDIR);
    fd = sys_open("/etc/motd", O_RDONLY | O_CREAT); /* there already: nothing to create */
    expect("open(an initrd file, O_CREAT, to read)", fd >= FD_FIRST, 1);
    sys_close((int)fd);
    expect("unlink(an initrd file)", sys_unlink("/etc/motd"), -EROFS);
    expect("unlink(an initrd directory)", sys_unlink("/bin"), -EISDIR);
    expect("unlink(the root)", sys_unlink("/"), -EISDIR);
    expect("unlink(missing)", sys_unlink("/etc/nope"), -ENOENT);
    expect("unlink(relative)", sys_unlink("tmp/x"), -EINVAL);
    expect("unlink(kernel pointer)", sys_unlink(KPTR), -EFAULT);
    expect("unlink(unmapped)", sys_unlink(UNMAPPED), -EFAULT);
}

static void directory(void)
{
    struct dirent d;

    expect("readdir(/, 0) is tmp/",
           sys_readdir("/", 0, &d) == 1 && strcmp(d.name, "tmp") == 0 && d.type == DIRENT_DIR, 1);
    expect("readdir(/tmp) starts empty", sys_readdir("/tmp", 0, &d), 0);
    expect("readdir(/tmp/)", sys_readdir("/tmp/", 0, &d), 0);
    expect("open(/tmp)", sys_open("/tmp", O_RDONLY), -EISDIR);
    expect("open(/tmp/, to make)", sys_open("/tmp/", WR_NEW), -EISDIR);
    expect("unlink(/tmp)", sys_unlink("/tmp"), -EISDIR);
    expect("readdir(/tmp/missing)", sys_readdir("/tmp/missing", 0, &d), -ENOENT);
}

static void files(void)
{
    struct stat st = {0};
    struct dirent d;
    char buf[64];
    long fd = sys_open("/tmp/a", O_WRONLY | O_CREAT);

    expect("open(/tmp/a, to make)", fd, FD_FIRST);
    expect("write 5", sys_write((int)fd, "hello", 5), 5);
    expect("fstat: a file of 5",
           sys_fstat((int)fd, &st) == 0 && st.type == STAT_FILE && st.size == 5, 1);
    expect("read(write-only)", sys_read((int)fd, buf, 1), -EBADF);
    sys_close((int)fd);
    fd = sys_open("/tmp/a", O_RDONLY);
    expect("read: all of it", sys_read((int)fd, buf, sizeof(buf)), 5);
    expect("read: hello", memcmp(buf, "hello", 5), 0);
    expect("read: then the end", sys_read((int)fd, buf, sizeof(buf)), 0);
    expect("write(read-only)", sys_write((int)fd, "x", 1), -EBADF);
    sys_close((int)fd);
    expect("readdir(/tmp): a, 5 bytes",
           sys_readdir("/tmp", 0, &d) == 1 && strcmp(d.name, "a") == 0 && d.size == 5 &&
               d.type == DIRENT_FILE,
           1);
    expect("readdir(/tmp, 1)", sys_readdir("/tmp", 1, &d), 0);
    expect("readdir(a file)", sys_readdir("/tmp/a", 0, &d), -ENOTDIR);
    expect("open(/tmp/a/)", sys_open("/tmp/a/", O_RDONLY), -ENOTDIR);
    expect("open(/tmp/a/b)", sys_open("/tmp/a/b", WR_NEW), -ENOTDIR);
    expect("open(/tmp/nope/b, to make)", sys_open("/tmp/nope/b", WR_NEW), -ENOENT);
    expect("open(/tmp/missing)", sys_open("/tmp/missing", O_RDONLY), -ENOENT);
    expect("open(/tmp/../etc/motd)", sys_open("/tmp/../etc/motd", O_RDONLY), -EINVAL);
    expect("open(/tmp//a)", sys_open("/tmp//a", O_RDONLY), -EINVAL);

    /* O_TRUNC, O_APPEND from two fds, O_RDWR, and a plain write over the start */
    fd = sys_open("/tmp/a", O_WRONLY | O_TRUNC);
    expect("O_TRUNC: empty", sys_fstat((int)fd, &st) == 0 && st.size == 0, 1);
    expect("write abc", sys_write((int)fd, "abc", 3), 3);
    sys_close((int)fd);
    long f1 = sys_open("/tmp/a", O_WRONLY | O_APPEND), f2 = sys_open("/tmp/a", O_WRONLY | O_APPEND);

    sys_write((int)f1, "1", 1);
    sys_write((int)f2, "2", 1);
    sys_write((int)f1, "3", 1);
    sys_close((int)f1);
    sys_close((int)f2);
    same("O_APPEND: each write at the end", "/tmp/a", "abc123");
    fd = sys_open("/tmp/a", O_RDWR);
    expect("O_RDWR: write XY", sys_write((int)fd, "XY", 2), 2);
    expect("O_RDWR: read on from there", sys_read((int)fd, buf, sizeof(buf)), 4);
    expect("O_RDWR: c123", memcmp(buf, "c123", 4), 0);
    sys_close((int)fd);
    fd = sys_open("/tmp/a", O_WRONLY);
    sys_write((int)fd, "Z", 1);
    sys_close((int)fd);
    same("O_WRONLY: over the start, the size kept", "/tmp/a", "ZYc123");
    expect("unlink(/tmp/a)", sys_unlink("/tmp/a"), 0);
    expect("unlink it again", sys_unlink("/tmp/a"), -ENOENT);
}

static void names(void)
{
    char path[8 + TMP_NAME_MAX] = "/tmp/";

    memset(path + 5, 'n', TMP_NAME_MAX - 1);
    path[5 + TMP_NAME_MAX - 1] = '\0';
    long fd = sys_open(path, WR_NEW);

    expect("open(the longest name)", fd >= FD_FIRST, 1);
    sys_close((int)fd);
    expect("unlink(the longest name)", sys_unlink(path), 0);
    path[5 + TMP_NAME_MAX - 1] = 'n'; /* one more */
    path[5 + TMP_NAME_MAX] = '\0';
    expect("open(a name one too long)", sys_open(path, WR_NEW), -ENAMETOOLONG);
}

/* A removed file lives on through its fds; the name is free at once. */
static void removed_but_open(void)
{
    struct dirent d;
    struct stat st = {0};
    char buf[16];
    long w, r, again;

    put("/tmp/b", "data");
    w = sys_open("/tmp/b", O_WRONLY | O_APPEND);
    r = sys_open("/tmp/b", O_RDONLY);
    expect("unlink(open twice)", sys_unlink("/tmp/b"), 0);
    expect("gone from the listing", sys_readdir("/tmp", 0, &d), 0);
    expect("gone by name", sys_open("/tmp/b", O_RDONLY), -ENOENT);
    expect("write to it still", sys_write((int)w, "++", 2), 2);
    expect("read it still", sys_read((int)r, buf, sizeof(buf)), 6);
    expect("read: data++", memcmp(buf, "data++", 6), 0);
    again = sys_open("/tmp/b", WR_NEW);
    expect("a new /tmp/b: empty", sys_fstat((int)again, &st) == 0 && st.size == 0, 1);
    sys_close((int)w);
    sys_close((int)r);
    sys_close((int)again);
    expect("unlink(the new /tmp/b)", sys_unlink("/tmp/b"), 0);
}

/* Across tmp pages and user pages, in one write and in uneven reads. */
static void large(void)
{
    struct stat st = {0};
    long fd = sys_open("/tmp/big", WR_NEW), got = 0, n;
    bool same_bytes = true;

    for (usize i = 0; i <= 10000; i++) /* the write takes big[1..10000] */
        big[i] = (char)(i * 7 + 3);
    expect("write 10000 from an odd offset", sys_write((int)fd, big + 1, 10000), 10000);
    expect("fstat: 10000", sys_fstat((int)fd, &st) == 0 && st.size == 10000, 1);
    sys_close((int)fd);
    fd = sys_open("/tmp/big", O_RDONLY);
    while ((n = sys_read((int)fd, big + 12000 - got % 3, 3000)) > 0) { /* uneven places */
        for (long i = 0; i < n; i++)
            same_bytes &= big[12000 - got % 3 + i] == (char)((got + i + 1) * 7 + 3);
        got += n;
    }
    expect("read back: 10000", got, 10000);
    expect("read back: the same bytes", same_bytes, 1);
    sys_close((int)fd);
    sys_unlink("/tmp/big");
}

static void space(void)
{
    char name[24];
    long fd = sys_open("/tmp/full", WR_NEW), total = 0, n, other;

    memset(big, 'f', sizeof(big));
    while ((n = sys_write((int)fd, big, sizeof(big))) > 0)
        total += n;
    expect("fill: -ENOSPC at the end", n, -ENOSPC);
    expect("fill: exactly TMP_BYTES_MAX", total, TMP_BYTES_MAX);
    expect("fill: not one byte more", sys_write((int)fd, big, 1), -ENOSPC);
    other = sys_open("/tmp/other", WR_NEW);
    expect("another file: made (no data yet)", other >= FD_FIRST, 1);
    expect("another file: no room for a byte", sys_write((int)other, "x", 1), -ENOSPC);
    sys_close((int)fd);
    sys_unlink("/tmp/full");
    expect("room again once it is gone", sys_write((int)other, "x", 1), 1);
    sys_close((int)other);
    sys_unlink("/tmp/other");

    for (int i = 0; i < TMP_FILES_MAX; i++) {
        k_snprintf(name, sizeof(name), "/tmp/f%d", i);
        fd = sys_open(name, WR_NEW);
        expect("make TMP_FILES_MAX files", fd >= FD_FIRST, 1);
        sys_close((int)fd);
    }
    expect("one file too many", sys_open("/tmp/one-more", WR_NEW), -ENOSPC);
    expect("and it took no fd", sys_open("/etc/motd", O_RDONLY), FD_FIRST);
    sys_close(FD_FIRST);
    for (int i = 0; i < TMP_FILES_MAX; i++) {
        k_snprintf(name, sizeof(name), "/tmp/f%d", i);
        expect("unlink them", sys_unlink(name), 0);
    }
}

/* With every fd taken, making a file is -EMFILE, and makes nothing. */
static void no_fd(void)
{
    struct dirent d;
    long fds[FILES_MAX];

    for (int i = 0; i < FILES_MAX; i++)
        fds[i] = sys_open("/etc/motd", O_RDONLY);
    expect("open(to make, no fd free)", sys_open("/tmp/nofd", WR_NEW), -EMFILE);
    expect("open(missing, no fd free): -ENOENT first", sys_open("/tmp/nofd", O_RDONLY), -ENOENT);
    expect("open(/tmp, no fd free): -EISDIR first", sys_open("/tmp", O_RDONLY), -EISDIR);
    expect("open(initrd, to write, no fd free): -EROFS first", sys_open("/etc/motd", O_WRONLY),
           -EROFS);
    for (int i = 0; i < FILES_MAX; i++)
        sys_close((int)fds[i]);
    expect("nothing was made", sys_readdir("/tmp", 0, &d), 0);
}

static void bad_buffers(void)
{
    char *edge = (char *)(((u64)_end + 4095) & ~4095ull);
    struct stat st = {0};
    char buf[16];
    long w = sys_open("/tmp/bad", WR_NEW), r;

    expect("write(kernel pointer)", sys_write((int)w, KPTR, 3), -EFAULT);
    expect("write(unmapped)", sys_write((int)w, UNMAPPED, 3), -EFAULT);
    expect("those wrote nothing", sys_fstat((int)w, &st) == 0 && st.size == 0, 1);
    for (int i = 0; i < 100; i++)
        edge[i - 100] = (char)('a' + i % 26);
    expect("write(100 bytes before an unmapped page)", sys_write((int)w, edge - 100, 200), 100);
    expect("the 100 went in", sys_fstat((int)w, &st) == 0 && st.size == 100, 1);
    r = sys_open("/tmp/bad", O_RDONLY);
    expect("read(read-only buffer)", sys_read((int)r, (void *)read_only, 8), -EFAULT);
    expect("read(kernel pointer)", sys_read((int)r, KPTR, 8), -EFAULT);
    expect("read: the faults moved nothing", sys_read((int)r, buf, 8), 8);
    expect("read: bytes 0 to 7", memcmp(buf, "abcdefgh", 8), 0);
    expect("read(50 bytes before an unmapped page)", sys_read((int)r, edge - 50, 100), 50);
    expect("read: on from byte 58", sys_read((int)r, buf, 8), 8);
    expect("read: bytes 58 to 65", buf[0] == 'a' + 58 % 26 && buf[7] == 'a' + 65 % 26, 1);
    sys_close((int)w);
    sys_close((int)r);
    sys_unlink("/tmp/bad");
}

/* What the shell will do for "echo > file" and "cat < a > b": a /tmp file as stdin and stdout. */
static void as_stdio(void)
{
    static const char *const echo_args[] = {"hi", "there", NULL};
    int status = -1;
    long out = sys_open("/tmp/out", WR_NEW), pid;

    pid = sys_spawn_stdio("echo", echo_args, (int[3]){0, (int)out, 2});
    expect("echo > /tmp/out", sys_wait((int)pid, &status) == pid && status == 0, 1);
    sys_close((int)out);
    same("echo's line is in the file", "/tmp/out", "hi there\n");

    long in = sys_open("/tmp/out", O_RDONLY), copy = sys_open("/tmp/copy", WR_NEW);

    pid = sys_spawn_stdio("cat", NULL, (int[3]){(int)in, (int)copy, 2});
    expect("cat < /tmp/out > /tmp/copy", sys_wait((int)pid, &status) == pid && status == 0, 1);
    sys_close((int)in);
    sys_close((int)copy);
    same("the copy is the same", "/tmp/copy", "hi there\n");
    sys_unlink("/tmp/out");
    sys_unlink("/tmp/copy");
}

int main(int argc, char **argv)
{
    struct dirent d;

    if (argc == 2 && strcmp(argv[1], "fill") == 0) {
        put("/tmp/keep", "keep me");
        return failures;
    }
    flags();
    initrd();
    directory();
    files();
    names();
    removed_but_open();
    large();
    space();
    no_fd();
    bad_buffers();
    as_stdio();
    expect("/tmp is empty at the end", sys_readdir("/tmp", 0, &d), 0);
    return failures;
}
