// SPDX-License-Identifier: MIT
#include "libsys.h"

int main(int argc, char **argv)
{
    if (argc > 2) {
        printf("usage: ls [/path]\n");
        return 1;
    }
    const char *path = argc == 2 ? argv[1] : "/";
    int fd = (int)sys_open(path);
    struct fs_dirent entry;
    long r;

    if (fd < 0) {
        printf("ls: %s: error %d\n", path, fd);
        return 1;
    }
    while ((r = sys_readdir(fd, &entry)) > 0)
        printf("%s%s\n", entry.name, entry.stat.type == FS_DIRECTORY ? "/" : "");
    sys_close(fd);
    if (r < 0)
        printf("ls: %s: error %ld\n", path, r);
    return r < 0;
}
