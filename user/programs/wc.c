// SPDX-License-Identifier: MIT
/*
 * Counts the lines, words and bytes of each file named, or with no names
 * of its standard input (at the console, until Ctrl-D on an empty line:
 * see stdin_read()), and prints them as "LINES WORDS BYTES", a file's
 * name after its own, and a total after more than one. A line is a "\n";
 * a word is a run of bytes that are not space, tab, CR or LF. A file that
 * cannot be opened or read is reported on fd 2 and left out; standard
 * output failing is reported and ends it. Exit status 0, or 1 if anything
 * failed.
 */
#include "libsys.h"

struct counts {
    u64 lines, words, bytes;
};

static long read_stdin(int fd, void *buf, usize len)
{
    (void)fd;
    return stdin_read(buf, len);
}

/* Adds what read_fn gives from fd to *c: 0 at its end, or read's error. */
static long count(long (*read_fn)(int fd, void *buf, usize len), int fd, struct counts *c)
{
    char buf[512];
    bool in_word = false;
    long n;

    while ((n = read_fn(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++) {
            char b = buf[i];
            bool space = b == ' ' || b == '\t' || b == '\r' || b == '\n';

            c->lines += b == '\n';
            c->words += !space && !in_word;
            in_word = !space;
        }
        c->bytes += (u64)n;
    }
    return n;
}

/* Prints one line of counts: false, after saying why, if standard output failed. */
static bool report(const struct counts *c, const char *name)
{
    int r = name ? printf("%lu %lu %lu %s\n", c->lines, c->words, c->bytes, name)
                 : printf("%lu %lu %lu\n", c->lines, c->words, c->bytes);

    if (r < 0)
        eprintf("wc: standard output: %s\n", sys_strerror(r));
    return r >= 0;
}

int main(int argc, char **argv)
{
    struct counts total = {0};
    int status = 0;
    long n;

    if (argc < 2) {
        n = count(read_stdin, 0, &total);
        if (n < 0) {
            eprintf("wc: standard input: %s\n", sys_strerror(n));
            return 1;
        }
        return report(&total, NULL) ? 0 : 1;
    }
    for (int i = 1; i < argc; i++) {
        struct counts c = {0};
        long fd = sys_open(argv[i], O_RDONLY);

        if (fd < 0) {
            eprintf("wc: %s: %s\n", argv[i], sys_strerror(fd));
            status = 1;
            continue;
        }
        n = count(sys_read, (int)fd, &c);
        sys_close((int)fd);
        if (n < 0) {
            eprintf("wc: %s: %s\n", argv[i], sys_strerror(n));
            status = 1;
            continue;
        }
        if (!report(&c, argv[i]))
            return 1;
        total.lines += c.lines;
        total.words += c.words;
        total.bytes += c.bytes;
    }
    if (argc > 2 && !report(&total, "total"))
        return 1;
    return status;
}
