/* SPDX-License-Identifier: MIT */
/*
 * Memory subsystem bring-up.
 */
#ifndef MINIOS_MM_MM_H
#define MINIOS_MM_MM_H

#include <mm/memmap.h>
#include <types.h>

/*
 * Sanitizes the firmware map, switches to the kernel's page tables, starts
 * the PMM and freezes the kernel half of the address space. `bi` must be
 * the kernel's copy: the boot window onto the loader's page disappears
 * halfway.
 */
void mm_init(const struct boot_info *bi);

/* The sanitized memory map. */
const struct mem_region *mm_regions(usize *count);

#endif /* MINIOS_MM_MM_H */
