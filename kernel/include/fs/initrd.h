/* SPDX-License-Identifier: MIT */
/*
 * The initrd: a ustar archive the loader leaves in RAM (boot_info v5),
 * read in place through the direct map. Its pages are reserved by the
 * PMM (mm/pmm.c) and never freed.
 */
#ifndef MINIOS_FS_INITRD_H
#define MINIOS_FS_INITRD_H

#include <boot/boot_info.h>
#include <types.h>

/*
 * Checks the initrd boot_info describes, after mm_init: it must lie in
 * usable RAM and be a valid archive (tar_validate). Logs what it found;
 * one that fails either check is reported and not used.
 */
void initrd_init(const struct boot_info *bi);

/* The archive, or NULL (and *len 0) when there is none to use. */
const u8 *initrd_get(usize *len);

#endif /* MINIOS_FS_INITRD_H */
