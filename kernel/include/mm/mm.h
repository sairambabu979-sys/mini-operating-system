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
 * the PMM and freezes the kernel half of the address space. Takes the
 * boot_info *physical* address because the boot window it is reached
 * through disappears halfway.
 */
void mm_init(u64 boot_info_phys);

/* The sanitized memory map. */
const struct mem_region *mm_regions(usize *count);

#endif /* MINIOS_MM_MM_H */
