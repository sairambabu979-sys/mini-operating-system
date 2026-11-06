/* SPDX-License-Identifier: MIT */
/*
 * Paths to files: /tmp (fs/tmpfs.c, in RAM, writable) and everything else
 * in the initrd (fs/tarfs.c, read-only). The rules and the order of the
 * errors are <uapi/syscall.h>'s; the system calls copy the path in and
 * hand out fds, and these do the rest.
 */
#ifndef MINIOS_FS_VFS_H
#define MINIOS_FS_VFS_H

#include <compiler.h>
#include <types.h>
#include <uapi/syscall.h>

struct file;

/*
 * open(): a new open file for `path`, with one reference, in *out. The
 * flags first (-EINVAL), then the lookup, -EISDIR, -EROFS; then -EMFILE
 * unless `have_fd` (the caller has an fd for it); then creating (-ENOSPC,
 * -ENOMEM). O_TRUNC empties a /tmp file only once nothing else can fail.
 */
__must_check int vfs_open(const char *path, u32 flags, bool have_fd, struct file **out);

/* unlink(): the lookup's errors, -EISDIR for a directory, -EROFS outside /tmp. */
__must_check int vfs_unlink(const char *path);

/* readdir(): entry `index` of the directory `path`: 1, 0 past the last, or the lookup's errors. */
__must_check int vfs_readdir(const char *path, u64 index, struct dirent *out);

/* For the tests: files in /tmp, linked or open, and the pages they hold. */
u32 vfs_tmp_files(void);
u32 vfs_tmp_pages(void);

#endif /* MINIOS_FS_VFS_H */
