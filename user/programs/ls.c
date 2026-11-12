// SPDX-License-Identifier: MIT
/*
 * Lists each directory named ("/" with none), one entry per line in the
 * archive's order: a directory with a trailing "/", a file with its size.
 * A file named is printed as it was given. With several paths, each
 * listing is headed "PATH:". A path that fails is reported on fd 2;
 * standard output failing is reported and ends it. Exit status 0, or 1 if
 * anything failed.
 */
#include "libsys.h"

#include <lib/errno.h>

#define OUTPUT_FAILED 2 /* list(): standard output failed, and it said so */

/* printf()'s result: 0, or OUTPUT_FAILED after saying why. */
static int output(int r)
{
    if (r >= 0)
        return 0;
    eprintf("ls: standard output: %s\n", sys_strerror(r));
    return OUTPUT_FAILED;
}

/* Lists one path: 0, 1 if the path failed, or OUTPUT_FAILED. */
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
        return output(printf("%s\n", path));
    }
    if (r < 0) {
        eprintf("ls: %s: %s\n", path, sys_strerror(r));
        return 1;
    }
    if (heading && output(printf("%s:\n", path)))
        return OUTPUT_FAILED;
    for (u64 i = 1; r > 0; i++) {
        int w =
            d.type == DIRENT_DIR ? printf("%s/\n", d.name) : printf("%-16s %llu\n", d.name, d.size);

        if (output(w))
            return OUTPUT_FAILED;
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
        return list("/", false) ? 1 : 0;
    for (int i = 1; i < argc; i++) {
        int r = list(argv[i], argc > 2);

        if (r == OUTPUT_FAILED)
            return 1;
        status |= r;
    }
    return status;
}
