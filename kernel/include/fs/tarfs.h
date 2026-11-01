/* SPDX-License-Identifier: MIT */
/*
 * Paths over a ustar archive: the read-only file system the initrd
 * provides. Pure code over <lib/tar.h>, host-tested.
 *
 * A path is absolute: "/" is the root, "/bin/sh" the archive entry
 * "bin/sh", and one trailing "/" is allowed ("/bin/"). An empty, "." or
 * ".." component is -EINVAL, as is a path that does not start with "/";
 * one too long for an archive name is -ENAMETOOLONG. A directory is one
 * the archive has an entry for (mkinitrd and tar write one for every
 * parent), or the root; the first entry of a name wins.
 */
#ifndef MINIOS_FS_TARFS_H
#define MINIOS_FS_TARFS_H

#include <types.h>
#include <uapi/syscall.h>

struct tarfs_node {
    const u8 *data; /* inside the archive; NULL for a directory */
    usize size;     /* 0 for a directory */
    bool is_dir;
};

/* Finds `path`: 0 with *out filled, or -EINVAL, -ENAMETOOLONG, -ENOENT. */
int tarfs_lookup(const u8 *archive, usize len, const char *path, struct tarfs_node *out);

/*
 * Entry `index` of directory `path`, counted from 0 in archive order:
 * 1 with *out filled, 0 past the last, or lookup's errors and -ENOTDIR.
 */
int tarfs_readdir(const u8 *archive, usize len, const char *path, u64 index, struct dirent *out);

#endif /* MINIOS_FS_TARFS_H */
