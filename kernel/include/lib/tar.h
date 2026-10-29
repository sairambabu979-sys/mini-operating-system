/* SPDX-License-Identifier: MIT */
/*
 * A read-only walk over a ustar archive in memory (the initrd).
 *
 * Pure code (host-tested and fuzzed), which trusts nothing in the
 * archive: every header must carry the ustar magic and a checksum that
 * matches, its size must be plain octal, and the entry must lie inside
 * the buffer; anything else is -EINVAL. The archive ends at a zero block,
 * or exactly at the end of the buffer.
 *
 * Only regular files and directories are reported; links, devices and
 * the like are skipped. A name is the header's prefix and name joined
 * with "/", without a leading "./" or a directory's trailing "/".
 */
#ifndef MINIOS_LIB_TAR_H
#define MINIOS_LIB_TAR_H

#include <types.h>

#define TAR_BLOCK    512
#define TAR_NAME_MAX 257 /* prefix 155 + "/" + name 100 + NUL */

struct tar_entry {
    char name[TAR_NAME_MAX];
    const u8 *data; /* inside the archive */
    usize size;
    bool is_dir;
};

/*
 * Reads the entry at *pos (0 for the first) and moves *pos past it.
 * Returns 1 with *e filled, 0 at the end of the archive, or -EINVAL.
 */
int tar_next(const u8 *archive, usize len, usize *pos, struct tar_entry *e);

/* Checks the whole archive: the number of entries reported, or -EINVAL. */
int tar_validate(const u8 *archive, usize len);

/* Finds the regular file `name`: true with *e filled, false if absent or the archive is bad. */
bool tar_find(const u8 *archive, usize len, const char *name, struct tar_entry *e);

#endif /* MINIOS_LIB_TAR_H */
