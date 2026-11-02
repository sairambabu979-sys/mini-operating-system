// SPDX-License-Identifier: MIT
#include "test.h"
#include <fs/vfs.h>
#include <lib/errno.h>
#include <stdio.h>
#include <string.h>

/* Independent tar fixture, with controllable names/order and no host tar dependency. */
static unsigned char archive[300 * 1024];
static size_t used;

static void add(const char *name, char type, const char *contents)
{
    unsigned char *h = archive + used;
    size_t size = contents ? strlen(contents) : 0;

    memset(h, 0, 1024);
    memcpy(h, name, strlen(name));
    snprintf((char *)h + 124, 12, "%011zo", size);
    h[156] = (unsigned char)type;
    memcpy(h + 257, "ustar\0" "00", 8);
    memset(h + 148, ' ', 8);
    unsigned sum = 0;

    for (int i = 0; i < 512; i++)
        sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
    h[155] = ' ';
    if (size)
        memcpy(h + 512, contents, size);
    used += 512 + (size + 511) / 512 * 512;
}

TEST(vfs_mounts_out_of_order_parents_and_lists_immediate_children)
{
    used = 0;
    add("etc/motd", '0', "hello");
    add("bin", '5', NULL);
    add("etc", '5', NULL);
    add("empty", '0', "");
    add("link", '2', NULL); /* unsupported entries are not exposed */
    EXPECT_EQ(vfs_mount(archive, used), 0);
    int file = vfs_lookup("/etc/motd"), dir = vfs_lookup("/etc");
    const u8 *data;
    usize size;
    struct fs_stat st;
    struct fs_dirent entry;

    EXPECT_TRUE(file > 0 && dir > 0);
    EXPECT_EQ(vfs_data(file, &data, &size), 0);
    EXPECT_TRUE(size == 5 && memcmp(data, "hello", 5) == 0);
    EXPECT_EQ(vfs_stat(file, &st), 0);
    EXPECT_EQ(st.type, FS_REGULAR);
    EXPECT_EQ(st.size, 5);
    EXPECT_EQ(vfs_data(dir, &data, &size), -EISDIR);
    EXPECT_EQ(vfs_readdir(file, 0, &entry), -ENOTDIR);
    EXPECT_EQ(vfs_readdir(dir, 0, &entry), 1);
    EXPECT_STREQ(entry.name, "motd");
    EXPECT_EQ(vfs_readdir(dir, 1, &entry), 0);
    EXPECT_EQ(vfs_readdir(vfs_lookup("/"), 2, &entry), 1);
    EXPECT_STREQ(entry.name, "empty");
    EXPECT_EQ(vfs_readdir(vfs_lookup("/"), 3, &entry), 0);
    EXPECT_EQ(vfs_lookup("/link"), -ENOENT);
    EXPECT_EQ(vfs_stat(-1, &st), -EBADF);
    EXPECT_EQ(vfs_data(999, &data, &size), -EBADF);
}

TEST(vfs_rejects_noncanonical_lookups)
{
    const char *bad[] = {"", "bin", "//", "/bin/", "/./bin", "/bin/..", "/bin//x"};

    used = 0;
    add("bin", '5', NULL);
    EXPECT_EQ(vfs_mount(archive, used), 0);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        EXPECT_EQ(vfs_lookup(bad[i]), -EINVAL);
}

TEST(vfs_bad_namespace_never_publishes_a_partial_mount)
{
    const char *bad[] = {"bin/../x", "/absolute", "bin//x", "orphan/x", "bin"};

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        used = 0;
        add("bin", '5', NULL);
        add(bad[i], '0', "x");
        EXPECT_EQ(vfs_mount(archive, used), -EINVAL);
        EXPECT_EQ(vfs_lookup("/bin"), -ENOENT);
        EXPECT_EQ(vfs_lookup("/"), -ENOENT);
    }
    used = 0;
    add("bin", '0', "x");
    add("bin/prog", '0', "y");
    EXPECT_EQ(vfs_mount(archive, used), -EINVAL);
    archive[used - 1024] ^= 1; /* corrupt the later file header */
    EXPECT_EQ(vfs_mount(archive, used), -EINVAL);
}

TEST(vfs_node_capacity_fails_cleanly)
{
    used = 0;
    for (int i = 0; i < 255; i++) {
        char name[32];

        snprintf(name, sizeof(name), "file%d", i);
        add(name, '0', NULL);
    }
    EXPECT_EQ(vfs_mount(archive, used), 0);
    add("overflow", '0', NULL);
    EXPECT_EQ(vfs_mount(archive, used), -ENOSPC);
    EXPECT_EQ(vfs_lookup("/file0"), -ENOENT);
}
