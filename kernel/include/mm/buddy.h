/* SPDX-License-Identifier: MIT */
/*
 * Binary buddy allocator over page frames.
 *
 * Pure algorithm, independent of hardware: it manages page *numbers* and
 * keeps all bookkeeping in a caller-provided `struct page` array, never in
 * the frames themselves. Consequences:
 *   - it can manage memory that is not mapped anywhere,
 *   - it is unit-tested and fuzzed on the host,
 *   - a stray write into a free frame cannot corrupt the allocator.
 *
 * A block of order k is 2^k frames, aligned to 2^k frames. Its buddy is the
 * block whose frame number differs only in bit k; two free buddies always
 * merge, so the free lists never hold both halves of a free block.
 *
 * Not thread-safe; the kernel wrapper (mm/pmm.c) serializes access.
 */
#ifndef MINIOS_MM_BUDDY_H
#define MINIOS_MM_BUDDY_H

#include <lib/list.h>
#include <types.h>

#define BUDDY_MAX_ORDER 10 /* 2^10 frames = 4 MiB with 4 KiB pages */

enum page_state {
    PAGE_RESERVED = 0, /* not managed: firmware, kernel image, holes */
    PAGE_FREE_HEAD,    /* first frame of a free block */
    PAGE_ALLOC_HEAD,   /* first frame of an allocated block */
    PAGE_TAIL,         /* any other frame of a free or allocated block */
};

struct page {
    struct list_head link; /* free-list link, valid for PAGE_FREE_HEAD */
    u8 state;              /* enum page_state */
    u8 order;              /* valid for heads */
    u16 reserved;
    u32 refcount; /* for users of allocated pages (e.g. shared page tables) */
};

struct buddy {
    struct page *pages; /* one per frame, indexed by frame number */
    u64 nframes;        /* frames covered by `pages`: [0, nframes) */
    struct list_head free_lists[BUDDY_MAX_ORDER + 1];
    u64 free_blocks[BUDDY_MAX_ORDER + 1];
    u64 free_frames;
    u64 managed_frames; /* frames ever handed to buddy_add_range */
};

#define BUDDY_NONE (~0ull)

/* All frames start out reserved. `pages` must hold `nframes` entries. */
void buddy_init(struct buddy *b, struct page *pages, u64 nframes);

/* Hands [start, end) to the allocator. Frames must currently be reserved. */
int buddy_add_range(struct buddy *b, u64 start, u64 end);

/* Returns the first frame of a 2^order block, or BUDDY_NONE. */
u64 buddy_alloc(struct buddy *b, unsigned order);

/*
 * Returns a block. -EINVAL if `pfn` is not the head of an allocated block of
 * exactly this order (double free, wrong order, interior pointer).
 */
int buddy_free(struct buddy *b, u64 pfn, unsigned order);

/*
 * Exhaustive consistency check: frame states, list membership, alignment,
 * counters, and the fully-merged property. Returns 0 or a negative errno and
 * writes a reason. O(nframes); for tests and debug builds.
 */
int buddy_verify(const struct buddy *b, const char **reason);

#endif /* MINIOS_MM_BUDDY_H */
