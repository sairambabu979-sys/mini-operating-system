/* SPDX-License-Identifier: MIT */
/*
 * Firmware memory map sanitizing.
 *
 * BIOS E820 maps may be unsorted, overlapping, and not page-aligned. The
 * sanitizer turns one into a sorted list of disjoint regions in which every
 * byte has exactly one type. Overlaps resolve toward the more restrictive
 * type (RAM that is also listed as reserved is reserved), and usable
 * regions shrink inward to whole pages.
 *
 * Pure code: no hardware access, unit-tested on the host.
 */
#ifndef MINIOS_MM_MEMMAP_H
#define MINIOS_MM_MEMMAP_H

#include <boot/boot_info.h>
#include <types.h>

#define PAGE_SIZE  4096ull
#define PAGE_SHIFT 12

struct mem_region {
    u64 base;
    u64 end;  /* exclusive */
    u32 type; /* enum e820_type; unknown types become E820_RESERVED */
};

/*
 * Sanitizes `n` firmware entries into `out` (capacity `max`). Returns the
 * number of regions written. If `out` is too small the map is truncated at
 * the highest addresses, which only ever loses memory, never invents it.
 */
usize memmap_sanitize(const struct e820_entry *in, usize n, struct mem_region *out, usize max);

#endif /* MINIOS_MM_MEMMAP_H */
