// SPDX-License-Identifier: MIT
/*
 * Prints each file named, in order, or with no names its standard input
 * to its end (at the console, line by line, 31 characters at most, until
 * Ctrl-D on an empty line: see stdin_read()). A file that cannot be opened or read is reported and
 * skipped. Exit status 0 if everything printed, 1 if anything failed.
 */
#include "libsys.h"

static long read_stdin(int fd, void *buf, usize len)
{
    (void)fd;
    return stdin_read(buf, len);
}

/* Copies fd to standard output with read_fn: 0 at its end, or read's error. */
static long copy_out(long (*read_fn)(int fd, void *buf, usize len), int fd)
{
    char buf[512];
    long n;

    while ((n = read_fn(fd, buf, sizeof(buf))) > 0)
        sys_write(1, buf, (usize)n);
    return n;
}

int main(int argc, char **argv)
{
    int status = 0;
    long n;

    if (argc < 2) {
        n = copy_out(read_stdin, 0);
        if (n < 0)
            printf("cat: standard input: %s\n", sys_strerror(n));
        return n < 0;
    }
    for (int i = 1; i < argc; i++) {
        long fd = sys_open(argv[i]);

        if (fd < 0) {
            printf("cat: %s: %s\n", argv[i], sys_strerror(fd));
            status = 1;
            continue;
        }
        n = copy_out(sys_read, (int)fd);
        if (n < 0) {
            printf("cat: %s: %s\n", argv[i], sys_strerror(n));
            status = 1;
        }
        sys_close((int)fd);
    }
    return status;
}
