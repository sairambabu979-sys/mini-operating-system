/* SPDX-License-Identifier: MIT */
/*
 * The file system at /tmp: a flat directory of files held in RAM, gone at
 * reboot. Pure code, host-tested. The caller supplies the allocator, holds
 * one lock around every call, and moves data in and out itself, through
 * the pages tmpfs_page() hands it; this code never touches user memory.
 *
 * A file is a name, a size, and up to TMP_BYTES_MAX / TMP_PAGE pages,
 * allocated as writes reach them (zeroed, so a page holds zeros past the
 * file's size). It lives while it is linked (named in /tmp) or open:
 * tmpfs_unlink() takes the name away, and the last tmpfs_close() frees it.
 *
 * Limits (<uapi/syscall.h>): TMP_FILES_MAX files, linked or still open,
 * and TMP_BYTES_MAX bytes of pages in all; -ENOSPC past either.
 */
#ifndef MINIOS_FS_TMPFS_H
#define MINIOS_FS_TMPFS_H

#include <compiler.h>
#include <types.h>
#include <uapi/syscall.h>

#define TMPFS_PAGES_MAX (TMP_BYTES_MAX / TMP_PAGE) /* in one file, and in all */

struct tmpfs_inode {
    char name[TMP_NAME_MAX];
    u64 size;
    u32 opens;   /* tmpfs_open() not yet closed */
    bool linked; /* named in /tmp */
    u8 *pages[TMPFS_PAGES_MAX];
};

struct tmpfs {
    void *(*alloc)(usize size); /* zeroed memory, or NULL */
    void (*free)(void *p);
    struct tmpfs_inode *dir[TMP_FILES_MAX]; /* the linked files, in the order they were made */
    u32 nlinked;
    u32 ninodes; /* linked or open */
    u32 npages;
};

/*
 * Where `path` (NUL-terminated, from "/") lands in /tmp:
 *   1          not under /tmp: the initrd's to look up;
 *   0          under /tmp: *found is the file, or NULL. With name[0] '\0'
 *              the path is /tmp itself; otherwise name is the file's,
 *              and a missing one may be created;
 *   -EINVAL    an empty, "." or ".." component anywhere in the path;
 *   -ENAMETOOLONG a name of TMP_NAME_MAX bytes or more;
 *   -ENOTDIR   a file named as a directory ("/tmp/f/", "/tmp/f/x");
 *   -ENOENT    a missing one named as a directory ("/tmp/d/x").
 */
int tmpfs_resolve(struct tmpfs *fs, const char *path, char name[TMP_NAME_MAX],
                  struct tmpfs_inode **found);

/* Makes an empty, linked, unopened file `name` (which must be missing): 0, -ENOSPC, -ENOMEM. */
int tmpfs_create(struct tmpfs *fs, const char *name, struct tmpfs_inode **out);

void tmpfs_open(struct tmpfs_inode *inode);

/* Drops one open; frees a file that is neither linked nor open any more. */
void tmpfs_close(struct tmpfs *fs, struct tmpfs_inode *inode);

/* Takes the file's name away (it must be linked); frees it if it is not open. */
void tmpfs_unlink(struct tmpfs *fs, struct tmpfs_inode *inode);

/* Empties the file, giving its pages back. */
void tmpfs_truncate(struct tmpfs *fs, struct tmpfs_inode *inode);

/*
 * The page holding byte `pos` of the file, in *page. With `make`, a
 * missing page is allocated (zeroed): -ENOSPC past TMP_BYTES_MAX, in the
 * file or in all, and -ENOMEM. Without it, a missing page is NULL (it
 * reads as zeros). The caller moves size past what it writes.
 */
int tmpfs_page(struct tmpfs *fs, struct tmpfs_inode *inode, u64 pos, bool make, u8 **page);

/* Gives back the page holding byte `pos`, if there is one: for a page made for a write that failed. */
void tmpfs_drop_page(struct tmpfs *fs, struct tmpfs_inode *inode, u64 pos);

/* Entry `index` of /tmp, in the order the files were made: 1 with *out filled, 0 past the last. */
int tmpfs_readdir(const struct tmpfs *fs, u64 index, struct dirent *out);

/*
 * Entry `index` of the root: "tmp" first, then the initrd's root (the
 * archive's), leaving out any entry of its own named "tmp", which /tmp
 * hides. 1 with *out filled, 0 past the last.
 */
int tmpfs_root_readdir(const u8 *archive, usize len, u64 index, struct dirent *out);

#endif /* MINIOS_FS_TMPFS_H */
