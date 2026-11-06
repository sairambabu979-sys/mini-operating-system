// SPDX-License-Identifier: MIT
/*
 * The file system at /tmp (kernel/fs/tmpfs.c): path rules, a file's life
 * (linked, open, both, neither), the limits, out of memory at each
 * allocation, the root listing over an archive, and 20,000 random
 * operations checked against a model, byte for byte and page for page.
 */
#include "tar_build.h"
#include "test.h"

#include <fs/tmpfs.h>
#include <lib/errno.h>

#include <stdlib.h>
#include <string.h>

static long live;         /* allocations not yet freed */
static long fail_in = -1; /* allocations before the one that fails, or -1 */

static void *t_alloc(usize size)
{
    if (fail_in >= 0 && fail_in-- == 0)
        return NULL;
    live++;
    return calloc(1, size);
}

static void t_free(void *p)
{
    if (p) {
        live--;
        free(p);
    }
}

static void fresh(struct tmpfs *fs)
{
    memset(fs, 0, sizeof(*fs));
    fs->alloc = t_alloc;
    fs->free = t_free;
    fail_in = -1;
}

static usize min_usize(usize a, usize b)
{
    return a < b ? a : b;
}

/* Writes as the kernel does: page by page, moving the size past what it wrote. */
static int fs_write(struct tmpfs *fs, struct tmpfs_inode *in, u64 pos, const u8 *src, usize n)
{
    while (n) {
        u8 *page;
        usize off = pos % TMP_PAGE, k = min_usize(n, TMP_PAGE - off);
        int err = tmpfs_page(fs, in, pos, true, &page);

        if (err)
            return err;
        memcpy(page + off, src, k);
        pos += k;
        src += k;
        n -= k;
        if (pos > in->size)
            in->size = pos;
    }
    return 0;
}

/* Reads all of the file into out; a missing page reads as zeros. */
static void fs_read(struct tmpfs *fs, struct tmpfs_inode *in, u8 *out)
{
    for (u64 pos = 0; pos < in->size;) {
        u8 *page;
        usize off = pos % TMP_PAGE, k = min_usize(in->size - pos, TMP_PAGE - off);

        EXPECT_EQ(tmpfs_page(fs, in, pos, false, &page), 0);
        if (page)
            memcpy(out + pos, page + off, k);
        else
            memset(out + pos, 0, k);
        pos += k;
    }
}

TEST(tmpfs_resolves_paths)
{
    static struct tmpfs fs;
    struct tmpfs_inode *f, *a;
    char name[TMP_NAME_MAX], longname[80] = "/tmp/";

    fresh(&fs);
    EXPECT_EQ(tmpfs_resolve(&fs, "/etc/motd", name, &f), 1);
    EXPECT_EQ(tmpfs_resolve(&fs, "/tmpx", name, &f), 1);
    EXPECT_EQ(tmpfs_resolve(&fs, "/tm", name, &f), 1);
    EXPECT_EQ(tmpfs_resolve(&fs, "/", name, &f), 1);
    EXPECT_EQ(tmpfs_resolve(&fs, "tmp/a", name, &f), 1); /* relative: the initrd's -EINVAL */
    EXPECT_EQ(tmpfs_resolve(&fs, "/tmp", name, &f), 0);
    EXPECT_TRUE(name[0] == '\0' && f == NULL);
    EXPECT_EQ(tmpfs_resolve(&fs, "/tmp/", name, &f), 0);
    EXPECT_TRUE(name[0] == '\0' && f == NULL);
    static const char *const bad[] = {"/tmp//",   "/tmp/.",  "/tmp/..",   "/tmp/../etc",
                                      "/tmp/a//", "/tmp//a", "/tmp/a/..", "/tmp/a/./b"};

    for (usize i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        EXPECT_EQ(tmpfs_resolve(&fs, bad[i], name, &f), -EINVAL);
    EXPECT_EQ(tmpfs_resolve(&fs, "/tmp/a", name, &f), 0);
    EXPECT_TRUE(strcmp(name, "a") == 0 && f == NULL); /* missing: may be made */
    EXPECT_EQ(tmpfs_create(&fs, "a", &a), 0);
    EXPECT_EQ(tmpfs_resolve(&fs, "/tmp/a", name, &f), 0);
    EXPECT_TRUE(f == a && strcmp(name, "a") == 0);
    EXPECT_EQ(tmpfs_resolve(&fs, "/tmp/ab", name, &f), 0); /* a prefix is not a match */
    EXPECT_TRUE(f == NULL);
    EXPECT_EQ(tmpfs_resolve(&fs, "/tmp/a/", name, &f), -ENOTDIR);
    EXPECT_EQ(tmpfs_resolve(&fs, "/tmp/a/b", name, &f), -ENOTDIR);
    EXPECT_EQ(tmpfs_resolve(&fs, "/tmp/b/", name, &f), -ENOENT);
    EXPECT_EQ(tmpfs_resolve(&fs, "/tmp/b/c", name, &f), -ENOENT);
    memset(longname + 5, 'n', TMP_NAME_MAX - 1); /* the longest name */
    longname[5 + TMP_NAME_MAX - 1] = '\0';
    EXPECT_EQ(tmpfs_resolve(&fs, longname, name, &f), 0);
    EXPECT_EQ((long)strlen(name), TMP_NAME_MAX - 1);
    strcat(longname, "n");
    EXPECT_EQ(tmpfs_resolve(&fs, longname, name, &f), -ENAMETOOLONG);
    tmpfs_unlink(&fs, a);
    EXPECT_EQ(live, 0);
}

TEST(tmpfs_files_live_while_linked_or_open)
{
    static struct tmpfs fs;
    struct tmpfs_inode *a, *b, *c;
    struct dirent d;

    fresh(&fs);
    EXPECT_EQ(tmpfs_create(&fs, "a", &a), 0);
    EXPECT_EQ(tmpfs_create(&fs, "b", &b), 0);
    EXPECT_EQ(tmpfs_create(&fs, "c", &c), 0);
    EXPECT_EQ(fs_write(&fs, b, 0, (const u8 *)"bee", 3), 0);
    tmpfs_open(b);
    tmpfs_unlink(&fs, b); /* open: it stays, nameless */
    EXPECT_EQ(fs.nlinked, 2);
    EXPECT_EQ(fs.ninodes, 3);
    EXPECT_EQ(tmpfs_readdir(&fs, 0, &d), 1);
    EXPECT_TRUE(strcmp(d.name, "a") == 0 && d.type == DIRENT_FILE && d.size == 0);
    EXPECT_EQ(tmpfs_readdir(&fs, 1, &d), 1);
    EXPECT_TRUE(strcmp(d.name, "c") == 0); /* the order is kept */
    EXPECT_EQ(tmpfs_readdir(&fs, 2, &d), 0);
    u8 buf[3];

    fs_read(&fs, b, buf);
    EXPECT_TRUE(memcmp(buf, "bee", 3) == 0); /* still readable */
    tmpfs_close(&fs, b);                     /* the last close frees it */
    EXPECT_EQ(fs.ninodes, 2);
    EXPECT_EQ(fs.npages, 0);
    tmpfs_open(a);
    tmpfs_open(a);
    tmpfs_close(&fs, a); /* linked and open once more: stays */
    tmpfs_close(&fs, a); /* linked, closed: stays */
    EXPECT_EQ(fs.ninodes, 2);
    tmpfs_unlink(&fs, a); /* not open: goes now */
    tmpfs_unlink(&fs, c);
    EXPECT_EQ(fs.ninodes, 0);
    EXPECT_EQ(live, 0);
}

TEST(tmpfs_stops_at_its_limits)
{
    static struct tmpfs fs;
    static struct tmpfs_inode *f[TMP_FILES_MAX], *g;
    char name[16];
    u8 *page;

    fresh(&fs);
    for (int i = 0; i < TMP_FILES_MAX; i++) {
        snprintf(name, sizeof(name), "f%d", i);
        EXPECT_EQ(tmpfs_create(&fs, name, &f[i]), 0);
    }
    EXPECT_EQ(tmpfs_create(&fs, "one-more", &g), -ENOSPC);
    tmpfs_open(f[0]);
    tmpfs_unlink(&fs, f[0]); /* still open: still counted */
    EXPECT_EQ(tmpfs_create(&fs, "one-more", &g), -ENOSPC);
    tmpfs_close(&fs, f[0]);
    EXPECT_EQ(tmpfs_create(&fs, "one-more", &g), 0);

    /* every page in one file, the last byte of the last one included */
    for (u64 pos = 0; pos < TMP_BYTES_MAX; pos += TMP_PAGE)
        EXPECT_EQ(tmpfs_page(&fs, f[1], pos, true, &page), 0);
    EXPECT_EQ(fs.npages, TMPFS_PAGES_MAX);
    EXPECT_EQ(tmpfs_page(&fs, f[1], TMP_BYTES_MAX - 1, true, &page), 0);   /* there already */
    EXPECT_EQ(tmpfs_page(&fs, f[1], TMP_BYTES_MAX, true, &page), -ENOSPC); /* past the file */
    EXPECT_EQ(tmpfs_page(&fs, f[2], 0, true, &page), -ENOSPC);             /* past them all */
    EXPECT_TRUE(page == NULL);
    EXPECT_EQ(tmpfs_page(&fs, f[2], 0, false, &page), 0); /* a hole reads as zeros */
    EXPECT_TRUE(page == NULL);
    EXPECT_EQ(tmpfs_page(&fs, f[2], (u64)1 << 40, false, &page), 0);
    EXPECT_TRUE(page == NULL);
    tmpfs_truncate(&fs, f[1]);
    EXPECT_EQ(fs.npages, 0);
    EXPECT_EQ(tmpfs_page(&fs, f[2], 0, true, &page), 0);
    for (int i = 1; i < TMP_FILES_MAX; i++)
        tmpfs_unlink(&fs, f[i]);
    tmpfs_unlink(&fs, g);
    EXPECT_EQ(live, 0);
}

TEST(tmpfs_out_of_memory_changes_nothing)
{
    static struct tmpfs fs;
    struct tmpfs_inode *a, *b = NULL;
    u8 *page;

    fresh(&fs);
    fail_in = 0;
    EXPECT_EQ(tmpfs_create(&fs, "a", &a), -ENOMEM);
    EXPECT_TRUE(fs.ninodes == 0 && fs.nlinked == 0 && live == 0);
    EXPECT_EQ(tmpfs_create(&fs, "a", &a), 0);
    fail_in = 0;
    EXPECT_EQ(tmpfs_page(&fs, a, 5000, true, &page), -ENOMEM);
    EXPECT_TRUE(fs.npages == 0 && a->pages[1] == NULL && live == 1);
    EXPECT_EQ(tmpfs_page(&fs, a, 5000, true, &page), 0);
    EXPECT_TRUE(page == a->pages[1] && fs.npages == 1);
    (void)b;
    tmpfs_unlink(&fs, a);
    EXPECT_EQ(live, 0);
}

TEST(tmpfs_gives_back_a_dropped_page)
{
    static struct tmpfs fs;
    struct tmpfs_inode *a;
    u8 *page;

    fresh(&fs);
    EXPECT_EQ(tmpfs_create(&fs, "a", &a), 0);
    EXPECT_EQ(tmpfs_page(&fs, a, 9000, true, &page), 0);
    EXPECT_EQ(fs.npages, 1);
    tmpfs_drop_page(&fs, a, 8192); /* the same page */
    EXPECT_TRUE(fs.npages == 0 && a->pages[2] == NULL && live == 1);
    tmpfs_drop_page(&fs, a, 8192);          /* not there: nothing */
    tmpfs_drop_page(&fs, a, TMP_BYTES_MAX); /* past the file: nothing */
    EXPECT_EQ(fs.npages, 0);
    tmpfs_unlink(&fs, a);
    EXPECT_EQ(live, 0);
}

TEST(tmpfs_root_lists_tmp_then_the_archive)
{
    static struct archive with, without;
    struct dirent d;
    const char *want[] = {"tmp", "bin", "etc", "README"};

    with.len = 0;
    add(&with, "", "bin/", '5', NULL, 0);
    add(&with, "", "tmp/", '5', NULL, 0); /* hidden by /tmp */
    add(&with, "", "tmp/x", '0', "x", 1);
    add(&with, "", "etc/", '5', NULL, 0);
    add(&with, "", "README", '0', "r", 1);
    finish(&with);
    for (u64 i = 0; i < 4; i++) {
        EXPECT_EQ(tmpfs_root_readdir(with.buf, with.len, i, &d), 1);
        EXPECT_STREQ(d.name, want[i]);
        EXPECT_EQ(d.type, i == 3 ? DIRENT_FILE : DIRENT_DIR);
    }
    EXPECT_EQ(tmpfs_root_readdir(with.buf, with.len, 4, &d), 0);
    EXPECT_EQ(tmpfs_root_readdir(with.buf, with.len, 1000, &d), 0);

    without.len = 0;
    add(&without, "", "bin/", '5', NULL, 0);
    finish(&without);
    EXPECT_EQ(tmpfs_root_readdir(without.buf, without.len, 0, &d), 1);
    EXPECT_STREQ(d.name, "tmp");
    EXPECT_EQ(tmpfs_root_readdir(without.buf, without.len, 1, &d), 1);
    EXPECT_STREQ(d.name, "bin");
    EXPECT_EQ(tmpfs_root_readdir(without.buf, without.len, 2, &d), 0);
    EXPECT_EQ(tmpfs_root_readdir(NULL, 0, 0, &d), 1); /* no initrd: /tmp alone */
    EXPECT_EQ(tmpfs_root_readdir(NULL, 0, 1, &d), 0);
}

/* The model of one file for the random run. */
#define MODEL_MAX (3 * TMP_PAGE + 100)
struct model {
    struct tmpfs_inode *in;
    u8 data[MODEL_MAX];
    usize size;
    int opens;
    bool linked;
    char name[16];
};

TEST(tmpfs_random_operations_match_a_model)
{
    static struct tmpfs fs;
    static struct model m[TMP_FILES_MAX];
    static u8 buf[MODEL_MAX], got[MODEL_MAX];
    int nm = 0;

    fresh(&fs);
    srand(1234);
    for (int step = 0; step < 20000; step++) {
        int op = rand() % 7, i = nm ? rand() % nm : -1;
        char name[16];

        snprintf(name, sizeof(name), "f%d", rand() % 10);
        if (op == 0) { /* create a missing name */
            char out[TMP_NAME_MAX];
            struct tmpfs_inode *found, *in;
            char path[32];

            snprintf(path, sizeof(path), "/tmp/%s", name);
            EXPECT_EQ(tmpfs_resolve(&fs, path, out, &found), 0);
            if (found)
                continue;
            int err = tmpfs_create(&fs, name, &in);

            EXPECT_EQ(err, nm == TMP_FILES_MAX ? -ENOSPC : 0);
            if (err)
                continue;
            m[nm] = (struct model){.in = in, .linked = true};
            strcpy(m[nm].name, name);
            nm++;
        } else if (i < 0) {
            continue;
        } else if (op == 1 && m[i].linked) {
            tmpfs_unlink(&fs, m[i].in);
            m[i].linked = false;
        } else if (op == 2 && m[i].linked) {
            tmpfs_open(m[i].in);
            m[i].opens++;
        } else if (op == 3 && m[i].opens) {
            tmpfs_close(&fs, m[i].in);
            m[i].opens--;
        } else if (op == 4) {
            tmpfs_truncate(&fs, m[i].in);
            m[i].size = 0;
        } else if (op >= 5) { /* write at a place up to two pages past the end: a hole */
            usize pos = (usize)rand() % (m[i].size + 2 * TMP_PAGE), n = (usize)rand() % 3000;

            if (pos > MODEL_MAX)
                pos = MODEL_MAX;
            if (pos + n > MODEL_MAX)
                n = MODEL_MAX - pos;
            for (usize k = 0; k < n; k++)
                buf[k] = (u8)rand();
            EXPECT_EQ(fs_write(&fs, m[i].in, pos, buf, n), 0);
            if (n && pos > m[i].size) /* the hole reads as zeros; writing nothing makes none */
                memset(m[i].data + m[i].size, 0, pos - m[i].size);
            memcpy(m[i].data + pos, buf, n);
            if (n && pos + n > m[i].size)
                m[i].size = pos + n;
        }
        if (i >= 0 && !m[i].linked && m[i].opens == 0) /* freed: drop it from the model */
            m[i] = m[--nm];

        /* every file, byte for byte; the listing; what is held */
        long pages = 0;
        u32 linked = 0;

        for (int k = 0; k < nm; k++) {
            EXPECT_EQ(m[k].in->size, m[k].size);
            fs_read(&fs, m[k].in, got);
            EXPECT_TRUE(memcmp(got, m[k].data, m[k].size) == 0);
            for (usize p = 0; p < TMPFS_PAGES_MAX; p++)
                pages += m[k].in->pages[p] != NULL;
            linked += m[k].linked;
        }
        EXPECT_EQ(fs.ninodes, (u32)nm);
        EXPECT_EQ(fs.nlinked, linked);
        EXPECT_EQ(fs.npages, pages);
        EXPECT_EQ(live, nm + pages);
    }
    for (int k = 0; k < nm; k++) {
        while (m[k].opens--)
            tmpfs_close(&fs, m[k].in);
        if (m[k].linked)
            tmpfs_unlink(&fs, m[k].in);
    }
    EXPECT_EQ(live, 0);
}
