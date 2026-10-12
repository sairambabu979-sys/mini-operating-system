// SPDX-License-Identifier: MIT
/*
 * Physical memory manager. See <mm/pmm.h>.
 *
 * Init:
 *   1. sanitize the E820 map,
 *   2. size the struct page array for the highest usable frame and place
 *      it in usable RAM above the kernel image (inside the boot window),
 *   3. hand every usable frame to the buddy allocator except the reserved
 *      ranges: low 1 MiB, kernel image, and the page array itself.
 *
 * Locking: none yet. Nothing runs concurrently until interrupts handlers
 * and threads exist (M5/M6), which add the lock in the same change.
 */
#define pr_fmt(fmt) "pmm: " fmt

#include <arch/cpu.h>
#include <core/klog.h>
#include <core/panic.h>
#include <mm/pmm.h>

/* The bootloader maps only physical 0-1 GiB until the VMM takes over. */
#define BOOT_WINDOW_END (1ull << 30)

struct phys_range {
    u64 start, end;
};

static struct buddy buddy;
static struct mem_region regions[BOOT_MMAP_MAX];
static usize nregions;
static u64 metadata_bytes;

const struct mem_region *pmm_regions(usize *count)
{
    *count = nregions;
    return regions;
}

static u64 pfn_of(u64 phys)
{
    return phys >> PAGE_SHIFT;
}

/* Lowest page-aligned spot of `bytes` in usable RAM within [lo, hi). */
static u64 find_free_area(u64 bytes, u64 lo, u64 hi)
{
    for (usize i = 0; i < nregions; i++) {
        if (regions[i].type != E820_USABLE)
            continue;
        u64 start = MAX(regions[i].base, lo);
        u64 end = MIN(regions[i].end, hi);

        start = ALIGN_UP(start, PAGE_SIZE);
        if (start < end && end - start >= bytes)
            return start;
    }
    return 0;
}

/* Adds [start, end) to the allocator minus the (sorted, disjoint) reserved ranges. */
static void add_usable(u64 start, u64 end, const struct phys_range *rsv, usize nrsv)
{
    for (usize i = 0; i < nrsv && start < end; i++) {
        if (rsv[i].end <= start || rsv[i].start >= end)
            continue;
        if (rsv[i].start > start)
            BUG_ON(buddy_add_range(&buddy, pfn_of(start), pfn_of(rsv[i].start)) != 0);
        start = MAX(start, rsv[i].end);
    }
    if (start < end)
        BUG_ON(buddy_add_range(&buddy, pfn_of(start), pfn_of(end)) != 0);
}

void pmm_init(const struct boot_info *bi)
{
    nregions = memmap_sanitize(bi->mmap, bi->mmap_count, regions, ARRAY_SIZE(regions));

    u64 top = 0;

    for (usize i = 0; i < nregions; i++) {
        if (regions[i].type == E820_USABLE)
            top = MAX(top, regions[i].end);
    }
    if (top == 0)
        panic("pmm: firmware reports no usable memory");

    u64 nframes = pfn_of(top);
    u64 kernel_end = ALIGN_UP(bi->kernel_phys_end, PAGE_SIZE);

    metadata_bytes = ALIGN_UP(nframes * sizeof(struct page), PAGE_SIZE);

    u64 meta = find_free_area(metadata_bytes, kernel_end, BOOT_WINDOW_END);

    if (meta == 0)
        panic("pmm: no room for %llu KiB of page metadata below 1 GiB",
              (unsigned long long)(metadata_bytes / 1024));

    buddy_init(&buddy, phys_to_kimage(meta), nframes);

    /* Sorted and disjoint: low memory < kernel image < metadata. */
    const struct phys_range reserved[] = {
        {0, PMM_LOW_RESERVED_END},
        {ALIGN_DOWN(bi->kernel_phys_start, PAGE_SIZE), kernel_end},
        {meta, meta + metadata_bytes},
    };

    for (usize i = 0; i < nregions; i++) {
        if (regions[i].type == E820_USABLE)
            add_usable(regions[i].base, regions[i].end, reserved, ARRAY_SIZE(reserved));
    }

    pr_info("%llu MiB managed, %llu KiB metadata at %#llx, highest frame %#llx\n",
            (unsigned long long)(buddy.managed_frames * PAGE_SIZE >> 20),
            (unsigned long long)(metadata_bytes >> 10), (unsigned long long)meta,
            (unsigned long long)(top - PAGE_SIZE));

#ifdef CONFIG_DEBUG
    const char *why;

    if (buddy_verify(&buddy, &why) != 0)
        panic("pmm: allocator inconsistent after init: %s", why);
#endif
}

u64 pmm_alloc_pages(unsigned order)
{
    u64 pfn = buddy_alloc(&buddy, order);

    return pfn == BUDDY_NONE ? PMM_ALLOC_FAILED : pfn << PAGE_SHIFT;
}

void pmm_free_pages(u64 phys, unsigned order)
{
    if (!IS_ALIGNED(phys, PAGE_SIZE) || buddy_free(&buddy, pfn_of(phys), order) != 0)
        panic("pmm: invalid free of %#llx (order %u): double free or wrong order?",
              (unsigned long long)phys, order);
}

void pmm_get_stats(struct pmm_stats *out)
{
    out->managed_frames = buddy.managed_frames;
    out->free_frames = buddy.free_frames;
    out->metadata_bytes = metadata_bytes;
}

int pmm_verify(const char **reason)
{
    return buddy_verify(&buddy, reason);
}
