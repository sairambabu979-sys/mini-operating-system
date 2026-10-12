/* SPDX-License-Identifier: MIT */
/*
 * General-purpose kernel heap.
 *
 * Small requests (<= 2 KiB) come from per-size-class slabs, one page each;
 * larger ones are whole buddy blocks from the PMM, reached via the direct
 * map. Every allocation is 16-byte aligned; large ones are page-aligned.
 *
 * Debug builds poison freed objects and verify the poison on reuse, which
 * turns use-after-free writes into a panic naming the allocation site's
 * slab, and detect double frees.
 */
#ifndef MINIOS_MM_KMALLOC_H
#define MINIOS_MM_KMALLOC_H

#include <compiler.h>
#include <types.h>

#define KMALLOC_MAX_SMALL 2048
#define KMALLOC_POISON    0x6B /* fill byte of freed objects (debug builds) */

__must_check void *kmalloc(usize size);
__must_check void *kzalloc(usize size);
/* Overflow-checked n * size, zeroed. */
__must_check void *kcalloc(usize n, usize size);
void kfree(void *ptr);

struct kmalloc_stats {
    u64 small_live; /* objects currently allocated from slabs */
    u64 large_live; /* page allocations currently live */
    u64 slabs;      /* slab pages held */
};

void kmalloc_get_stats(struct kmalloc_stats *out);

#endif /* MINIOS_MM_KMALLOC_H */
