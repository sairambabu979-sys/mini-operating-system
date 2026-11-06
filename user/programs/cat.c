// SPDX-License-Identifier: MIT
/*
 * Prints each file named, in order, or with no names its standard input
 * to its end (at the console, line by line, 31 characters at most, until
 * Ctrl-D on an empty line: see stdin_read()). A file that cannot be
 * opened or read is reported on fd 2 and skipped; a write that fails
 * (-EPIPE: the reader of a pipe is gone) is reported and ends it. Exit
 * status 0 if everything printed, 1 if anything failed.
 */
#include "libsys.h"

#include <lib/errno.h>

static long read_stdin(int fd, void *buf, usize len)
{
    (void)fd;
    return stdin_read(buf, len);
}

#define WRITE_FAILED 1 /* copy_out(): standard output failed, and it said so */

/* Copies fd to standard output with read_fn: 0 at its end, read's error, or WRITE_FAILED. */
static long copy_out(long (*read_fn)(int fd, void *buf, usize len), int fd)
{
    char buf[512];
    long n;

    while ((n = read_fn(fd, buf, sizeof(buf))) > 0) {
        long written = sys_write(1, buf, (usize)n);

        if (written != n) { /* a pipe whose reader went part-way is -EPIPE too */
            eprintf("cat: standard output: %s\n", sys_strerror(written < 0 ? written : -EPIPE));
            return WRITE_FAILED;
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    int status = 0;
    long n;

    if (argc < 2) {
        n = copy_out(read_stdin, 0);
        if (n < 0)
            eprintf("cat: standard input: %s\n", sys_strerror(n));
        return n != 0;
    }
    for (int i = 1; i < argc; i++) {
        long fd = sys_open(argv[i], O_RDONLY);

        if (fd < 0) {
            eprintf("cat: %s: %s\n", argv[i], sys_strerror(fd));
            status = 1;
            continue;
        }
        n = copy_out(sys_read, (int)fd);
        sys_close((int)fd);
        if (n == WRITE_FAILED)
            return 1;
        if (n < 0) {
            eprintf("cat: %s: %s\n", argv[i], sys_strerror(n));
            status = 1;
        }
    }
    return status;
}
