// SPDX-License-Identifier: MIT
/*
 * The ustar reader: archives built here header by header (so each
 * corruption is exact), the real initrd the build makes with Python's
 * tarfile, and a seeded fuzz of mutated archives that must never read
 * outside the buffer (ASan) or report an entry outside it.
 */
#include "tar_build.h"
#include "test.h"

#include <lib/errno.h>
#include <lib/tar.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* seal(), but summing the bytes as signed chars, as some old tars did. */
static void seal_signed(uint8_t *h)
{
    int sum = 0;

    memset(h + 148, ' ', 8);
    for (int i = 0; i < 512; i++)
        sum += (int8_t)h[i];
    snprintf((char *)h + 148, 8, "%06o", (unsigned)sum);
    h[155] = ' ';
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

/* Appends a pax record "LEN KEY=VALUE\n" to out (LEN counts its own digits); returns its length. */
static size_t pax_record(char *out, const char *kv)
{
    size_t body = strlen(kv) + 2, len = body + 1; /* " " and "\n", then at least one digit */

    while (len != body + (size_t)snprintf(NULL, 0, "%zu", len))
        len = body + (size_t)snprintf(NULL, 0, "%zu", len);
    return (size_t)sprintf(out, "%zu %s\n", len, kv);
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

    /* a skipped entry's size is checked too: it would move the walk past the buffer */
    snprintf((char *)a.buf + 124, 12, "%011o", 1u << 20);
    seal(a.buf);
    EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
    pos = a.len + 1; /* and a position past the end is refused, not read */
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), -EINVAL);
}

TEST(tar_reads_other_writers_headers)
{
    static struct archive a;
    struct tar_entry e;
    size_t pos = 0;
    uint8_t *h;
    char prefix[156], name[101], full[257];

    a.len = 0;
    add(&a, "", "./", '5', NULL, 0);  /* the archive's root, as "tar -C dir ." writes it */
    add(&a, "", "old", '\0', "o", 1); /* pre-POSIX type for a file */
    h = add(&a, "", "spaced", '0', "abc", 3);
    memcpy(h + 124, "    0000003\0", 12); /* leading spaces in a number */
    seal(h);
    h = add(&a, "junk", "gnu", '0', "g", 1);
    memcpy(h + 257, "ustar  \0", 8); /* GNU magic: offset 345 is not a prefix */
    seal(h);
    h = add(&a, "", "caf\xe9", '0', "c", 1);
    seal_signed(h); /* a signed sum differs from the unsigned one only above 0x7f */
    add(&a, "", "na\xefve", '0', "n", 1); /* and the POSIX unsigned sum of the same */
    memset(prefix, 'p', 155);
    prefix[155] = '\0';
    memset(name, 'n', 100);
    name[100] = '\0';
    add(&a, prefix, name, '0', NULL, 0); /* the longest name a ustar header holds */
    finish(&a);

    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 1);
    EXPECT_STREQ(e.name, "old");
    EXPECT_TRUE(!e.is_dir && e.size == 1);
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 1);
    EXPECT_STREQ(e.name, "spaced");
    EXPECT_TRUE(e.size == 3 && memcmp(e.data, "abc", 3) == 0);
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 1);
    EXPECT_STREQ(e.name, "gnu");
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 1);
    EXPECT_STREQ(e.name, "caf\xe9");
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 1);
    EXPECT_STREQ(e.name, "na\xefve");
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 1);
    snprintf(full, sizeof(full), "%s/%s", prefix, name);
    EXPECT_EQ(strlen(e.name), 256);
    EXPECT_STREQ(e.name, full);
    EXPECT_EQ(tar_next(a.buf, a.len, &pos, &e), 0);
}

TEST(tar_skips_harmless_pax_headers_and_refuses_the_rest)
{
    static struct archive a;
    static const char *const refused[] = {"path=bin/real", "linkpath=x", "size=99"};
    char rec[64];
    size_t n;

    a.len = 0;
    n = pax_record(rec, "mtime=1700000000.5");
    add(&a, "", "PaxHeaders/f", 'x', rec, n);
    add(&a, "", "f", '0', "z", 1);
    n = pax_record(rec, "comment=all of them");
    add(&a, "", "GlobalHead", 'g', rec, n);
    finish(&a);
    EXPECT_EQ(tar_validate(a.buf, a.len), 1);

    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        a.len = 0;
        n = pax_record(rec, refused[i]);
        add(&a, "", "PaxHeaders/f", i == 2 ? 'g' : 'x', rec, n);
        add(&a, "", "f", '0', "z", 1);
        finish(&a);
        EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
    }

    static const char *const malformed[] = {
        "99 mtime=1\n",   /* longer than the header */
        "12 mtime=1\n",   /* LEN one long: past the header's end */
        "10 mtime=1\n",   /* LEN one short: the record does not end in "\n" */
        "6 a=bX6 c=d\n",  /* the first record does not end in "\n", the second does */
        " mtime=1\n",     /* no length */
        "6 a=b\n0 c=d\n", /* a LEN of 0 would never move the walk on */
        "18446744073709551646 a=bbbbbb\n", /* 2^64 + 30: wraps to this record's length */
        "10mtime=1\n",                     /* no space */
        "8 mtime\n",                       /* no "=" */
        "5 =1\n",                          /* no key */
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        a.len = 0;
        add(&a, "", "PaxHeaders/f", 'x', malformed[i], strlen(malformed[i]));
        add(&a, "", "f", '0', "z", 1);
        finish(&a);
        EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
    }

    /* a LEN past the header's end, where the padding happens to hold the "\n" */
    a.len = 0;
    add(&a, "", "PaxHeaders/f", 'x', "12 mtime=1\n", 11);
    a.buf[512 + 11] = '\n';
    add(&a, "", "f", '0', "z", 1);
    finish(&a);
    EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
}

TEST(tar_refuses_names_it_cannot_read)
{
    static struct archive a;

    a.len = 0;
    add(&a, "", "././@LongLink", 'L', "a-very-long-name", 17); /* GNU: the next header's name */
    add(&a, "", "a-very-lo", '0', "z", 1);
    finish(&a);
    EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
    a.len = 0;
    add(&a, "", "././@LongLink", 'K', "target", 7); /* and a link's target */
    add(&a, "", "l", '2', NULL, 0);
    finish(&a);
    EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
    a.len = 0;
    add(&a, "", ".", '0', "z", 1); /* a file named for the root */
    finish(&a);
    EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
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

    /* names no path could mean: an empty, "." or ".." component */
    static const char *const not_plain[] = {"a//b", "a/./b", "../x", "x/..", "a/.", "x/../y"};

    for (size_t i = 0; i < sizeof(not_plain) / sizeof(not_plain[0]); i++) {
        memset(h, 0, 100);
        memcpy(h, not_plain[i], strlen(not_plain[i]));
        seal(h);
        EXPECT_EQ(tar_validate(a.buf, a.len), -EINVAL);
    }
    memset(h, 0, 100);
    memcpy(h, "a/b.c/..d", 9); /* dots inside a name are fine */
    seal(h);
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
    bool found = tar_find(buf, len, "bin/hello", &e);

    EXPECT_TRUE(found);
    if (found)
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
    char rec[64];

    srand(1234);
    base.len = 0; /* one of every kind of header the reader handles */
    add(&base, "", "bin/", '5', NULL, 0);
    add(&base, "", "bin/hello", '0', "HELLO", 5);
    add(&base, "", "link", '2', NULL, 0);
    add(&base, "", "PaxHeaders/x", 'x', rec, pax_record(rec, "mtime=1"));
    add(&base, "usr/share", "doc/a", '0', "a", 1);
    add(&base, "", "old", '\0', "welcome\n", 8);
    finish(&base);
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
