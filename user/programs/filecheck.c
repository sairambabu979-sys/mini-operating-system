// SPDX-License-Identifier: MIT
/* Exercise the real syscall boundary, including offsets after failed copies. */
#include "libsys.h"

#include <lib/errno.h>
#include <lib/string.h>

static int failures;
#define CHECK(expr) do { if (!(expr)) { printf("filecheck: line %d failed\n", __LINE__); failures++; } } while (0)

int main(int argc, char **argv)
{
    char bytes[16];
    struct fs_stat st;
    struct fs_dirent entry;
    void *bad = (void *)0x12345000;

    /* The parent's descriptors are not inherited by spawn. */
    if (argc == 2 && strcmp(argv[1], "child") == 0)
        return sys_read(3, bytes, 1) == -EBADF ? 0 : 1;
    CHECK(sys_open(bad) == -EFAULT);
    CHECK(sys_open("relative") == -EINVAL);
    CHECK(sys_open("/bin/../etc") == -EINVAL);
    CHECK(sys_open("/not-here") == -ENOENT);
    char longpath[FS_PATH_MAX + 1];

    memset(longpath, 'x', sizeof(longpath));
    longpath[sizeof(longpath) - 1] = 0;
    CHECK(sys_open(longpath) == -ENAMETOOLONG);
    CHECK(sys_close(0) == -EBADF);
    CHECK(sys_close(-1) == -EBADF);
    CHECK(sys_read(FS_FD_MAX, bytes, 1) == -EBADF);

    int a = (int)sys_open("/bin/hello"), b = (int)sys_open("/bin/hello");

    CHECK(a == 3 && b == 4);
    CHECK(sys_fstat(a, &st) == 0 && st.type == FS_REGULAR && st.size > 4);
    CHECK(sys_fstat(a, bad) == -EFAULT);
    CHECK(sys_write(a, "x", 1) == -EROFS);
    CHECK(sys_readdir(a, &entry) == -ENOTDIR);
    CHECK(sys_read(a, bad, 4) == -EFAULT);
    CHECK(sys_read(a, (void *)"readonly", 4) == -EFAULT);
    CHECK(sys_seek(a, 0, FS_SEEK_CUR) == 0);
    CHECK(sys_read(a, bytes, 4) == 4 && memcmp(bytes, "\177ELF", 4) == 0);
    CHECK(sys_seek(b, 0, FS_SEEK_CUR) == 0);
    CHECK(sys_seek(a, -1, FS_SEEK_SET) == -EINVAL);
    CHECK(sys_seek(a, (s64)0x8000000000000000ull, FS_SEEK_CUR) == -EINVAL);
    CHECK(sys_seek(a, 0x7fffffffffffffffll, FS_SEEK_CUR) == -EINVAL);
    CHECK(sys_seek(a, 0, 99) == -EINVAL);
    CHECK(sys_seek(a, 0, FS_SEEK_CUR) == 4);
    CHECK(sys_seek(a, 0, FS_SEEK_END) == (long)st.size);
    CHECK(sys_read(a, bytes, sizeof(bytes)) == 0);
    CHECK(sys_seek(a, -2, FS_SEEK_END) == (long)st.size - 2);
    CHECK(sys_read(a, bytes, sizeof(bytes)) == 2);

    const char *args[] = {"child", NULL};
    int pid = (int)sys_spawnv("filecheck", args), status = -1;

    CHECK(pid > 0 && sys_wait(pid, &status) == pid && status == 0);
    CHECK(sys_close(a) == 0 && sys_close(a) == -EBADF);
    CHECK(sys_close(b) == 0);

    a = (int)sys_open("/bin");
    b = (int)sys_open("/bin");
    CHECK(sys_fstat(a, &st) == 0 && st.type == FS_DIRECTORY);
    CHECK(sys_read(a, bytes, 1) == -EISDIR);
    CHECK(sys_seek(a, 0, FS_SEEK_SET) == -EISDIR);
    CHECK(sys_readdir(a, bad) == -EFAULT);
    struct fs_dirent first;

    CHECK(sys_readdir(a, &first) == 1);
    CHECK(sys_readdir(b, &entry) == 1 && strcmp(entry.name, first.name) == 0);
    bool found = strcmp(first.name, "hello") == 0;
    long r;

    while ((r = sys_readdir(a, &entry)) > 0)
        found |= strcmp(entry.name, "hello") == 0 && entry.stat.type == FS_REGULAR;
    CHECK(r == 0 && found && sys_readdir(a, &entry) == 0);
    CHECK(sys_close(a) == 0 && sys_close(b) == 0);

    int fds[FS_FD_MAX - 3];

    for (usize i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
        fds[i] = (int)sys_open("/");
        CHECK(fds[i] >= 3);
    }
    CHECK(sys_open("/") == -EMFILE);
    CHECK(sys_close(fds[0]) == 0);
    CHECK(sys_open("/etc/motd") == fds[0]);
    /* Deliberately leave these open: process exit must reclaim the table. */
    return failures;
}
