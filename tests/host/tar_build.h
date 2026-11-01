/* SPDX-License-Identifier: MIT */
/*
 * Builds ustar archives header by header for the host tests (test_tar.c,
 * test_tarfs.c), so each test controls every byte.
 */
#ifndef MINIOS_TESTS_HOST_TAR_BUILD_H
#define MINIOS_TESTS_HOST_TAR_BUILD_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARCHIVE_MAX (64 * 1024)

struct archive {
    uint8_t buf[ARCHIVE_MAX];
    size_t len;
};

/* Writes the header checksum the way tar does: six octal digits, NUL, space. */
static inline void seal(uint8_t *h)
{
    unsigned sum = 0;

    memset(h + 148, ' ', 8);
    for (int i = 0; i < 512; i++)
        sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
    h[155] = ' ';
}

/* Appends one entry; returns its header, which a test may then corrupt (and reseal). */
static inline uint8_t *add(struct archive *a, const char *prefix, const char *name, char type,
                           const void *data, size_t size)
{
    uint8_t *h = a->buf + a->len;

    memset(h, 0, 512);
    memcpy(h, name, strnlen(name, 100));
    memcpy(h + 345, prefix, strnlen(prefix, 155));
    snprintf((char *)h + 100, 8, "%07o", 0644);
    snprintf((char *)h + 124, 12, "%011zo", size);
    h[156] = (uint8_t)type;
    memcpy(h + 257, "ustar", 6); /* POSIX: "ustar\0", then version "00" */
    memcpy(h + 263, "00", 2);
    seal(h);
    a->len += 512;
    if (size)
        memcpy(a->buf + a->len, data, size);
    a->len += (size + 511) / 512 * 512;
    return h;
}

static inline void finish(struct archive *a)
{
    memset(a->buf + a->len, 0, 1024); /* two zero blocks */
    a->len += 1024;
}

#endif /* MINIOS_TESTS_HOST_TAR_BUILD_H */
