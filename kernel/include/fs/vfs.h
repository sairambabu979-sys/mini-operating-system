/* SPDX-License-Identifier: MIT */
#ifndef MINIOS_FS_VFS_H
#define MINIOS_FS_VFS_H
#include <types.h>
#include <uapi/fs.h>

/* One immutable root mounted at boot. The archive must outlive all lookups.
 * Mount validates the entire namespace before publishing it. Parents must
 * be explicit directories; duplicates and dot components are rejected.
 * Unsupported archive entries (including links) are never exposed.
 * A failed mount leaves no filesystem available. No allocation is needed. */
int vfs_mount(const void *archive, usize len);
/* Absolute, canonical paths only; no empty, '.' or '..' components. Root is '/'. */
int vfs_lookup(const char *path);
int vfs_stat(int node, struct fs_stat *out);
/* Immutable bytes for the ELF loader. Directories return -EISDIR. */
int vfs_data(int node, const u8 **data, usize *size);
/* Returns 1 for an entry, 0 at the end, or a negative errno. */
int vfs_readdir(int node, usize index, struct fs_dirent *out);
#endif
