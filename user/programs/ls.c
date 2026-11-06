// SPDX-License-Identifier: MIT
/*
 * Lists each directory named ("/" with none), one entry per line in the
 * archive's order: a directory with a trailing "/", a file with its size.
 * A file named is printed as it was given. With several paths, each
 * listing is headed "PATH:". A path that fails is reported on fd 2. Exit
 * status 0, or 1 if any path failed.
 */
#include "libsys.h"

#include <lib/errno.h>

static int list(const char *path, bool heading)
{
    struct dirent d;
    long r = sys_readdir(path, 0, &d);

    if (r == -ENOTDIR) { /* a file: there is nothing under it to list */
        long fd = sys_open(path, O_RDONLY);

        if (fd < 0) {
            eprintf("ls: %s: %s\n", path, sys_strerror(fd));
            return 1;
        }
        sys_close((int)fd);
        printf("%s\n", path);
        return 0;
    }
    if (r < 0) {
        eprintf("ls: %s: %s\n", path, sys_strerror(r));
        return 1;
    }
    if (heading)
        printf("%s:\n", path);
    for (u64 i = 1; r > 0; i++) {
        if (d.type == DIRENT_DIR)
            printf("%s/\n", d.name);
        else
            printf("%-16s %llu\n", d.name, d.size);
        r = sys_readdir(path, i, &d);
    }
    if (r < 0) {
        eprintf("ls: %s: %s\n", path, sys_strerror(r));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int status = 0;

    if (argc < 2)
        return list("/", false);
    for (int i = 1; i < argc; i++)
        status |= list(argv[i], argc > 2);
    return status;
}
