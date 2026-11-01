// SPDX-License-Identifier: MIT
/*
 * Prints each file named, in order. A file that cannot be opened or read
 * is reported and skipped. Exit status 0 if every file printed, 1 if any
 * failed, 2 with no arguments.
 */
#include "libsys.h"

int main(int argc, char **argv)
{
    char buf[512];
    int status = 0;

    if (argc < 2) {
        printf("usage: cat FILE...\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        long fd = sys_open(argv[i]);
        long n;

        if (fd < 0) {
            printf("cat: %s: %s\n", argv[i], sys_strerror(fd));
            status = 1;
            continue;
        }
        while ((n = sys_read((int)fd, buf, sizeof(buf))) > 0)
            sys_write(1, buf, (usize)n);
        if (n < 0) {
            printf("cat: %s: %s\n", argv[i], sys_strerror(n));
            status = 1;
        }
        sys_close((int)fd);
    }
    return status;
}
