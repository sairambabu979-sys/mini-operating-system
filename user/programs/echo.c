// SPDX-License-Identifier: MIT
/*
 * Prints its arguments, separated by single spaces, and a newline, in one
 * write. Exit status 0, or 1 if the write failed (reported on fd 2).
 */
#include "libsys.h"

#include <lib/string.h>

int main(int argc, char **argv)
{
    char line[SPAWN_ARGS_SIZE + 1]; /* the arguments, their NULs become spaces and a newline */
    usize n = 0;

    for (int i = 1; i < argc; i++) {
        usize len = strlen(argv[i]);

        if (i > 1)
            line[n++] = ' ';
        memcpy(line + n, argv[i], len);
        n += len;
    }
    line[n++] = '\n';
    long written = write_all(1, line, n);

    if (written < 0) {
        eprintf("echo: standard output: %s\n", sys_strerror(written));
        return 1;
    }
    return 0;
}
