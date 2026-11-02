// SPDX-License-Identifier: MIT
/*
 * The ustar reader: archives built here header by header (so each
 * corruption is exact), the real initrd the build makes with Python's
 * tarfile, and a seeded fuzz of mutated archives that must never read
 * outside the buffer (ASan) or report an entry outside it.
 */
#include "test.h"

#include <lib/errno.h>
#include <lib/tar.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARCHIVE_MAX (64 * 1024)

struct archive {
    uint8_t buf[ARCHIVE_MAX];
    size_t len;
};

/* Writes the header checksum the way tar does: six octal digits, NUL, space. */
static void seal(uint8_t *h)
{
    unsigned sum = 0;

    memset(h + 148, ' ', 8);
    for (int i = 0; i < 512; i++)
        sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
    h[155] = ' ';
}

/* Appends one entry; returns its header, which a test may then corrupt (and reseal). */
static uint8_t *add(struct archive *a, const char *prefix, const char *name, char type,
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

static void finish(struct archive *a)
{
    memset(a->buf + a->len, 0, 1024); /* two zero blocks */
    a->len += 1024;
}

static void sample(struct archive *a)
{
    a->len = 0;
    add(a, "", "bin/", '5', NULL, 0);
    add(a, "", "bin/hello", '0', "HELLO", 5);
    add(a, "", "etc/motd", '0', "welcome\n", 8);
    add(a, "", "empty", '0', NULL, 0);
    finish(a);
}

TEST(tar_reads_files_and_directories_in_order)
{
    static struct archive a;
    struct tar_entry e;
    size_t pos = 0;

    sample(&a);
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 1);
    EXPECT_STREQ(e.name, "bin");
    EXPECT_TRUE(e.is_dir);
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 1);
    EXPECT_STREQ(e.name, "bin/hello");
    EXPECT_TRUE(!e.is_dir && e.size == 5 && memcmp(e.data, "HELLO", 5) == 0);
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 1);
    EXPECT_STREQ(e.name, "etc/motd");
    EXPECT_TRUE(e.size == 8 && memcmp(e.data, "welcome\n", 8) == 0);
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 1);
    EXPECT_STREQ(e.name, "empty");
    EXPECT_EQ(e.size, 0);
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 0);
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 0); /* and stays at the end */
    EXPECT_EQ(tar_validate(a.buf, a.len), 4);
}

TEST(tar_find_finds_files_not_directories)
{
    static struct archive a;
    struct tar_entry e;

    sample(&a);
    EXPECT_TRUE(tar_find(a.buf, a.len, "etc/motd", &e) && e.size == 8);
    EXPECT_TRUE(!tar_find(a.buf, a.len, "bin", &e));
    EXPECT_TRUE(!tar_find(a.buf, a.len, "etc/mot", &e));
    EXPECT_TRUE(!tar_find(a.buf, a.len, "nothing", &e));
}

TEST(tar_names_join_prefix_and_drop_dot_slash)
{
    static struct archive a;
    struct tar_entry e;
    size_t pos = 0;
    char long_name[101];

    a.len = 0;
    add(&a, "", "./bin/x", '0', "x", 1);
    add(&a, "usr/share", "doc/a", '0', "a", 1);
    memset(long_name, 'n', 100); /* fills the field: no NUL */
    long_name[100] = '\0';
    add(&a, "", long_name, '0', NULL, 0);
    finish(&a);
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 1);
    EXPECT_STREQ(e.name, "bin/x");
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 1);
    EXPECT_STREQ(e.name, "usr/share/doc/a");
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 1);
    EXPECT_STREQ(e.name, long_name);
}

TEST(tar_ends_at_a_zero_block_or_the_buffer_end)
{
    static struct archive a;
    struct tar_entry e;
    size_t pos = 0;

    a.len = 0;
    add(&a, "", "f", '0', "abc", 3);
    EXPECT_EQ(tar_validate(a.buf, a.len), 1);         /* no end blocks at all */
    EXPECT_EQ(tar_validate(a.buf, 512 + 3), 1);       /* nor the last entry's padding */
    EXPECT_EQ(tar_validate(a.buf, 512 + 2), -EINVAL); /* but its data must all be there */
    EXPECT_EQ(tar_validate(a.buf, 0), 0);             /* an empty archive */
    EXPECT_EQ(tar_validate(a.buf, 300), -EINVAL);     /* a part of a header */
    finish(&a);
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 1);
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 0);
}

TEST(tar_skips_links_and_other_types)
{
    static struct archive a;
    struct tar_entry e;
    size_t pos = 0;

    a.len = 0;
    add(&a, "", "link", '2', NULL, 0);
    add(&a, "", "fifo", '6', NULL, 0);
    add(&a, "", "file", '0', "z", 1);
    finish(&a);
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 1);
    EXPECT_STREQ(e.name, "file");
    EXPECT_EQ(tar_validate(a.buf, a.len), 1);
}

TEST(tar_rejects_bad_headers)
{
    static struct archive a;
    uint8_t *h;

    a.len = 0;
    h = add(&a, "", "f", '0', "abc", 3);
    finish(&a);
    EXPECT_EQ(tar_validate(a.buf, a.len), 1);

    h[257] = 'X'; /* magic, resealed */
    seal(h);
    EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
    memcpy(h + 257, "ustar", 5);
    seal(h);
    h[0] ^= 1; /* any byte changed after sealing */
    EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
    h[0] ^= 1;
    EXPECT_EQ(tar_validate(a.buf, a.len), 1);

    snprintf((char *)h + 124, 12, "%011o", 4096u); /* past the end of the buffer */
    seal(h);
    EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
    memcpy(h + 124, "0000000009\0", 12); /* not octal */
    seal(h);
    EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
    memcpy(h + 124, "0000000003x", 12); /* the right size, then junk */
    seal(h);
    EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
    {
        static struct archive b; /* no data after it: only the field itself can fail */
        uint8_t *hb;

        b.len = 0;
        hb = add(&b, "", "e", '0', NULL, 0);
        finish(&b);
        EXPECT_EQ(tar_validate(b.buf, b.len), 1);
        memset(hb + 124, 0, 12); /* no digits at all */
        seal(hb);
        EXPECT_EQ(tar_validate(b.buf, b.len), -EINVAL);
    }
    memset(h + 124, 0, 12); /* base-256 */
    h[124] = 0x80;
    h[135] = 3;
    seal(h);
    EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
    memcpy(h + 124, "77777777777", 12); /* about 8 GiB: larger than any buffer here */
    seal(h);
    EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);

    snprintf((char *)h + 124, 12, "%011o", 3u);
    memset(h, 0, 100);
    memcpy(h, "./", 2); /* a name that is nothing once "./" is dropped */
    seal(h);
    EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
}

TEST(tar_rejects_unknown_magic_versions_and_corrupt_suffixes)
{
    static struct archive a;
    struct tar_entry e;

    a.len = 0;
    uint8_t *h = add(&a, "", "file", '0', "x", 1);
    finish(&a);
    memcpy(h + 257, "ustarXYZ", 8);
    seal(h);
    EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);

    sample(&a);
    a.buf[1536] ^= 1; /* etc/motd, after bin/hello: lookup must reject the whole archive */
    EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
    EXPECT_TRUE(!tar_find(a.buf, a.len, "bin/hello", &e));
}

TEST(tar_gnu_prefix_and_signed_checksum)
{
    static struct archive a;
    struct tar_entry e;
    size_t pos = 0;

    a.len = 0;
    uint8_t *h = add(&a, "1234567", "file", '0', "x", 1);

    memcpy(h + 257, "ustar  \0", 8);
    seal(h);
    finish(&a);
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 1);
    EXPECT_STREQ(e.name, "file"); /* GNU stores timestamps where POSIX stores a prefix */

    h[0] = 0xFF;
    memset(h + 148, ' ', 8);
    int sum = 0;

    for (int i = 0; i < 512; i++)
        sum += (int8_t)h[i];
    snprintf((char *)h + 148, 8, "%06o", (unsigned)sum);
    h[155] = ' ';
    EXPECT_EQ(tar_validate(a.buf, a.len), 1);
}

TEST(tar_root_directories_are_skipped_but_root_files_are_rejected)
{
    static struct archive a;

    a.len = 0;
    add(&a, "", "./", '5', NULL, 0);
    add(&a, "", ".", '5', NULL, 0);
    add(&a, "", "file", '0', "x", 1);
    finish(&a);
    EXPECT_EQ(tar_validate(a.buf, a.len), 1);
    a.len = 0;
    add(&a, "", ".", '0', "x", 1);
    finish(&a);
    EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
}

TEST(tar_refuses_long_names_and_pax_name_or_size_overrides)
{
    static struct archive a;
    const char *records[] = {"12 path=foo\n", "16 linkpath=foo\n", "10 size=1\n",
                             "999999999999999999999999999 path=x\n", "7 bad=x\n", "7 =abc\n"};

    for (size_t i = 0; i < sizeof(records) / sizeof(records[0]); i++) {
        for (int global = 0; global < 2; global++) {
            a.len = 0;
            add(&a, "", "pax", global ? 'g' : 'x', records[i], strlen(records[i]));
            add(&a, "", "file", '0', "x", 1);
            finish(&a);
            EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
        }
    }
    for (int link = 0; link < 2; link++) {
        a.len = 0;
        add(&a, "", "long", link ? 'K' : 'L', "name", 5);
        add(&a, "", "file", '0', "x", 1);
        finish(&a);
        EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
    }
    a.len = 0;
    add(&a, "", "pax", 'x', "11 mtime=1\n", 11);
    add(&a, "", "file", '0', "x", 1);
    finish(&a);
    EXPECT_EQ(tar_validate(a.buf, a.len), 1);
}

#ifdef INITRD_PATH
/* The archive the build makes with tools/mkinitrd.py, which uses Python's tarfile. */
TEST(tar_reads_the_built_initrd)
{
    static uint8_t buf[8 << 20];
    FILE *f = fopen(INITRD_PATH, "rb");
    struct tar_entry e;

    EXPECT_TRUE(f != NULL);
    if (!f)
        return;
    size_t len = fread(buf, 1, sizeof(buf), f);

    fclose(f);
    EXPECT_TRUE(len > 0 && len < sizeof(buf));
    EXPECT_TRUE(tar_validate(buf, len) > 10);
    EXPECT_TRUE(tar_find(buf, len, "bin/hello", &e));
    EXPECT_TRUE(e.size > 4 && memcmp(e.data,
                                     "\x7f"
                                     "ELF",
                                     4) == 0);
    EXPECT_TRUE(tar_find(buf, len, "bin/sh", &e));
}
#endif

/*
 * Mutated copies of a valid archive: a few bytes changed, some headers
 * resealed so the corruption gets past the checksum, and cut at random.
 * Every walk must end, and every entry must lie inside the buffer.
 */
TEST(tar_fuzz_never_leaves_the_buffer)
{
    static struct archive base, a;
    unsigned results[3] = {0}; /* walks ending cleanly, with -EINVAL, entries seen */

    srand(1234);
    sample(&base);
    for (int iter = 0; iter < 20000; iter++) {
        memcpy(a.buf, base.buf, base.len);
        a.len = base.len;
        int flips = 1 + rand() % 4;

        for (int i = 0; i < flips; i++) {
            size_t at = (size_t)rand() % a.len;

            a.buf[at] = rand() % 2 ? (uint8_t)rand() : (uint8_t)(a.buf[at] ^ (1u << (rand() % 8)));
            if (rand() % 2)
                seal(a.buf + at / 512 * 512);
        }
        if (rand() % 4 == 0)
            a.len = (size_t)rand() % (a.len + 1);
        uint8_t *copy = malloc(a.len ? a.len : 1); /* exactly a.len bytes: ASan sees any overread */

        memcpy(copy, a.buf, a.len);
        struct tar_entry e;
        size_t pos = 0;
        int r, steps = 0;

        while ((r = tar_next(copy, a.len, &pos, &e)) > 0) {
            results[2]++;
            if (e.data < copy || e.data + e.size > copy + a.len || strlen(e.name) == 0 ||
                strlen(e.name) >= TAR_NAME_MAX) {
                test_fail(__FILE__, __LINE__, "iteration %d: entry outside the archive", iter);
                break;
            }
            if (++steps > 1000) {
                test_fail(__FILE__, __LINE__, "iteration %d: the walk does not end", iter);
                break;
            }
        }
        results[r < 0]++;
        free(copy);
    }
    EXPECT_TRUE(results[0] > 0 && results[1] > 0 &&
                results[2] > 0); /* the fuzz reached all three */
}
