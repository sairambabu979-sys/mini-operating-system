/* SPDX-License-Identifier: MIT */
/*
 * Physical memory manager: the kernel's owner of every RAM page frame.
 *
 * Built on the buddy allocator (<mm/buddy.h>). Physical address 0 is never
 * handed out (the low 1 MiB is permanently reserved), so 0 doubles as the
 * failure value.
 */
#ifndef MINIOS_MM_PMM_H
#define MINIOS_MM_PMM_H

#include <boot/boot_info.h>
#include <compiler.h>
#include <mm/buddy.h>
#include <mm/memmap.h>
#include <types.h>

#define PMM_ALLOC_FAILED 0ull

/* Memory below this is never managed: IVT, BIOS data, EBDA, loader, ROMs. */
#define PMM_LOW_RESERVED_END 0x100000ull

struct pmm_stats {
    u64 managed_frames; /* frames under PMM control */
    u64 free_frames;
    u64 metadata_bytes; /* the struct page array */
};

/* Called by mm_init() once the direct map is live. */
void pmm_init(const struct mem_region *map, usize nmap, const struct boot_info *bi);

/* Returns the physical address of 2^order contiguous, aligned frames, or 0. */
__must_check u64 pmm_alloc_pages(unsigned order);

/*
 * For tests: after `n` more successful allocations, every allocation
 * fails, until it is called again; -1 turns that off (the default).
 * Returns the previous setting. The caller keeps other tasks from
 * allocating meanwhile (preempt_disable()).
 */
long pmm_test_fail_after(long n);

static inline __must_check u64 pmm_alloc_page(void)
{
    return pmm_alloc_pages(0);
}

/* Panics on a double free or mismatched order: those are always bugs. */
void pmm_free_pages(u64 phys, unsigned order);

static inline void pmm_free_page(u64 phys)
{
    pmm_free_pages(phys, 0);
}

/* Order of the allocated block at phys, or -1 if phys is not a block head. */
int pmm_block_order(u64 phys);

/*
 * Ownership tags on allocated blocks, so a free can check its pointer
 * really came from its allocator (kfree must not take a page table or a
 * user frame back). A new block starts as PMM_OWNER_NONE.
 */
enum { PMM_OWNER_NONE = 0, PMM_OWNER_KMALLOC = 1, PMM_OWNER_SLAB = 2 };
void pmm_set_owner(u64 phys, u16 owner);
/* The tag of the allocated block whose head is phys; -1 if phys is not one. */
int pmm_block_owner(u64 phys);

void pmm_get_stats(struct pmm_stats *out);

/* Full allocator consistency check (O(frames)); 0 if consistent. */
int pmm_verify(const char **reason);

#endif /* MINIOS_MM_PMM_H */
