// SPDX-License-Identifier: MIT
/*
 * Paths over an archive (kernel/fs/tarfs.c): lookups, the path rules,
 * and directory listings, on archives built here header by header.
 */
#include "tar_build.h"
#include "test.h"

#include <fs/tarfs.h>
#include <lib/errno.h>

#include <string.h>

/* bin/ with two programs, etc/ with a file, a file at the top, and a link (skipped). */
static void tree(struct archive *a)
{
    a->len = 0;
    add(a, "", "bin/", '5', NULL, 0);
    add(a, "", "bin/sh", '0', "SH", 2);
    add(a, "", "bin/hello", '0', "HELLO", 5);
    add(a, "", "etc/", '5', NULL, 0);
    add(a, "", "etc/motd", '0', "hi\n", 3);
    add(a, "", "etc/link", '2', NULL, 0);
    add(a, "", "README", '0', "r", 1);
    finish(a);
}

TEST(tarfs_looks_up_files_and_directories)
{
    static struct archive a;
    struct tarfs_node n;

    tree(&a);
    EXPECT_EQ(tarfs_lookup(a.buf, a.len, "/etc/motd", &n), 0);
    EXPECT_TRUE(!n.is_dir && n.size == 3 && memcmp(n.data, "hi\n", 3) == 0);
    EXPECT_EQ(tarfs_lookup(a.buf, a.len, "/bin", &n), 0);
    EXPECT_TRUE(n.is_dir && n.size == 0 && n.data == NULL);
    EXPECT_EQ(tarfs_lookup(a.buf, a.len, "/bin/", &n), 0); /* one trailing "/" */
    EXPECT_TRUE(n.is_dir);
    EXPECT_EQ(tarfs_lookup(a.buf, a.len, "/", &n), 0);
    EXPECT_TRUE(n.is_dir);
    EXPECT_EQ(tarfs_lookup(a.buf, a.len, "/README", &n), 0);
    EXPECT_EQ(n.size, 1);
    EXPECT_EQ(tarfs_lookup(a.buf, a.len, "/etc/nope", &n), -ENOENT);
    EXPECT_EQ(tarfs_lookup(a.buf, a.len, "/etc/link", &n), -ENOENT); /* links are not reported */
    EXPECT_EQ(tarfs_lookup(a.buf, a.len, "/bin/sh/x", &n), -ENOENT);
}

TEST(tarfs_refuses_paths_that_are_not_plain_and_absolute)
{
    static struct archive a;
    static const char *const bad[] = {
        "", "bin/sh", "//", "//bin", "/bin//sh", "/bin//", "/./bin", "/bin/.", "/bin/..", "/../etc",
    };
    struct tarfs_node n;
    char long_path[300];

    tree(&a);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        EXPECT_EQ(tarfs_lookup(a.buf, a.len, bad[i], &n), -EINVAL);
    long_path[0] = '/';
    memset(long_path + 1, 'x', 256); /* an archive name holds 256: fits, not found */
    long_path[257] = '\0';
    EXPECT_EQ(tarfs_lookup(a.buf, a.len, long_path, &n), -ENOENT);
    memset(long_path + 1, 'x', 257);
    long_path[258] = '\0';
    EXPECT_EQ(tarfs_lookup(a.buf, a.len, long_path, &n), -ENAMETOOLONG);
}

TEST(tarfs_lists_a_directory_in_archive_order)
{
    static struct archive a;
    struct dirent d;

    tree(&a);
    EXPECT_EQ(tarfs_readdir(a.buf, a.len, "/", 0, &d), 1);
    EXPECT_STREQ(d.name, "bin");
    EXPECT_TRUE(d.type == DIRENT_DIR && d.size == 0);
    EXPECT_EQ(tarfs_readdir(a.buf, a.len, "/", 1, &d), 1);
    EXPECT_STREQ(d.name, "etc");
    EXPECT_EQ(tarfs_readdir(a.buf, a.len, "/", 2, &d), 1);
    EXPECT_STREQ(d.name, "README");
    EXPECT_TRUE(d.type == DIRENT_FILE && d.size == 1 && d.reserved == 0);
    EXPECT_EQ(tarfs_readdir(a.buf, a.len, "/", 3, &d), 0);
    EXPECT_EQ(tarfs_readdir(a.buf, a.len, "/", ~0ull, &d), 0);

    EXPECT_EQ(tarfs_readdir(a.buf, a.len, "/bin/", 1, &d), 1);
    EXPECT_STREQ(d.name, "hello");
    EXPECT_EQ(d.size, 5);
    EXPECT_EQ(tarfs_readdir(a.buf, a.len, "/bin", 2, &d), 0);
    EXPECT_EQ(tarfs_readdir(a.buf, a.len, "/etc", 0, &d), 1);
    EXPECT_STREQ(d.name, "motd");
    EXPECT_EQ(tarfs_readdir(a.buf, a.len, "/etc", 1, &d), 0); /* the link is not listed */

    EXPECT_EQ(tarfs_readdir(a.buf, a.len, "/etc/motd", 0, &d), -ENOTDIR);
    EXPECT_EQ(tarfs_readdir(a.buf, a.len, "/var", 0, &d), -ENOENT);
    EXPECT_EQ(tarfs_readdir(a.buf, a.len, "var", 0, &d), -EINVAL);
}

/* "binary" starts with "bin" but is not under it, and a name with a prefix lists under it. */
TEST(tarfs_matches_whole_components)
{
    static struct archive a;
    struct dirent d;
    struct tarfs_node n;

    a.len = 0;
    add(&a, "", "bin/", '5', NULL, 0);
    add(&a, "", "binary", '0', "b", 1);
    add(&a, "", "bin/x", '0', "x", 1);
    add(&a, "usr/share", "doc", '5', NULL, 0);
    add(&a, "usr/share", "doc/a", '0', "a", 1);
    finish(&a);
    EXPECT_EQ(tarfs_readdir(a.buf, a.len, "/bin", 0, &d), 1);
    EXPECT_STREQ(d.name, "x");
    EXPECT_EQ(tarfs_readdir(a.buf, a.len, "/bin", 1, &d), 0);
    EXPECT_EQ(tarfs_readdir(a.buf, a.len, "/usr/share/doc", 0, &d), 1);
    EXPECT_STREQ(d.name, "a");
    /* no entry for usr/ or usr/share/: not directories here, though their files resolve */
    EXPECT_EQ(tarfs_readdir(a.buf, a.len, "/usr", 0, &d), -ENOENT);
    EXPECT_EQ(tarfs_lookup(a.buf, a.len, "/usr/share/doc/a", &n), 0);
}
