// SPDX-License-Identifier: MIT
/*
 * Physical memory manager. See <mm/pmm.h>.
 *
 * Init (runs after vmm_init, so all RAM is reachable through the direct map):
 *   1. size the struct page array for the highest usable frame and place
 *      it in usable RAM above the kernel image, clear of the initrd,
 *   2. hand every usable frame to the buddy allocator except the reserved
 *      ranges: low 1 MiB, kernel image, initrd, and the page array itself.
 *
 * The initrd is reserved wherever the loader says it is, valid or not:
 * the map calls its pages usable, and they must not be handed out under
 * it. A range that is not usable RAM reserves nothing.
 *
 * Locking: one IRQ-safe spinlock around every buddy operation.
 */
#define pr_fmt(fmt) "pmm: " fmt

#include <arch/paging.h>
#include <core/klog.h>
#include <core/panic.h>
#include <core/spinlock.h>
#include <mm/pmm.h>

struct phys_range {
    u64 start, end;
};

static struct buddy buddy;
static struct spinlock pmm_lock = SPINLOCK_INIT("pmm");
static const struct mem_region *regions;
static usize nregions;
static u64 metadata_bytes;

static u64 pfn_of(u64 phys)
{
    return phys >> PAGE_SHIFT;
}

/*
 * Sorts ranges by start, dropping empty ones; returns the count. They may
 * still overlap (a loader could put the initrd on the kernel): both users
 * below only ever move forward past a range's end, which handles that.
 */
static usize sort_ranges(struct phys_range *r, usize n)
{
    usize out = 0;

    for (usize i = 0; i < n; i++)
        if (r[i].start < r[i].end)
            r[out++] = r[i];
    for (usize i = 1; i < out; i++)
        for (usize j = i; j > 0 && r[j].start < r[j - 1].start; j--) {
            struct phys_range t = r[j];

            r[j] = r[j - 1];
            r[j - 1] = t;
        }
    return out;
}

/* Lowest page-aligned spot of `bytes` in usable RAM within [lo, hi), clear of the (sorted) reserved ranges. */
static u64 find_free_area(u64 bytes, u64 lo, u64 hi, const struct phys_range *rsv, usize nrsv)
{
    for (usize i = 0; i < nregions; i++) {
        if (regions[i].type != E820_USABLE)
            continue;
        u64 start = ALIGN_UP(MAX(regions[i].base, lo), PAGE_SIZE);
        u64 end = MIN(regions[i].end, hi);

        for (usize j = 0; j < nrsv; j++) /* sorted by start: step past each one in the way */
            if (rsv[j].start < start + bytes && rsv[j].end > start)
                start = ALIGN_UP(rsv[j].end, PAGE_SIZE);
        if (start < end && end - start >= bytes)
            return start;
    }
    return 0;
}

/* Adds [start, end) to the allocator minus the reserved ranges (sorted by start). */
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

void pmm_init(const struct mem_region *map, usize nmap, const struct boot_info *bi)
{
    regions = map;
    nregions = nmap;

    u64 top = 0, beyond = 0;

    /* Every frame handed out is used through the direct map: none above it. */
    for (usize i = 0; i < nregions; i++) {
        if (regions[i].type != E820_USABLE)
            continue;
        if (regions[i].base < HHDM_SIZE) /* else the page array would grow for nothing */
            top = MAX(top, MIN(regions[i].end, HHDM_SIZE));
        if (regions[i].end > HHDM_SIZE)
            beyond += regions[i].end - MAX(regions[i].base, HHDM_SIZE);
    }
    if (top == 0)
        panic("pmm: firmware reports no usable memory");
    if (beyond)
        pr_warn("%llu MiB of RAM above the %llu GiB direct map left unused\n",
                (unsigned long long)(beyond >> 20), (unsigned long long)(HHDM_SIZE >> 30));

    u64 nframes = pfn_of(top);
    u64 kernel_end = ALIGN_UP(bi->kernel_phys_end, PAGE_SIZE);

    metadata_bytes = ALIGN_UP(nframes * sizeof(struct page), PAGE_SIZE);

    struct phys_range reserved[4] = {
        {0, PMM_LOW_RESERVED_END},
        {ALIGN_DOWN(bi->kernel_phys_start, PAGE_SIZE), kernel_end},
        {0, 0}, /* the initrd, if any */
        {0, 0}, /* the page array, once placed */
    };

    if (bi->initrd_size) /* validate_boot_info: no overflow, inside the direct map */
        reserved[2] = (struct phys_range){ALIGN_DOWN(bi->initrd_phys, PAGE_SIZE),
                                          ALIGN_UP(bi->initrd_phys + bi->initrd_size, PAGE_SIZE)};
    usize nreserved = sort_ranges(reserved, 3);
    u64 meta = find_free_area(metadata_bytes, kernel_end, HHDM_SIZE, reserved, nreserved);

    if (meta == 0)
        panic("pmm: no room for %llu KiB of page metadata",
              (unsigned long long)(metadata_bytes / 1024));

    buddy_init(&buddy, phys_to_virt(meta), nframes);
    reserved[nreserved++] = (struct phys_range){meta, meta + metadata_bytes};
    nreserved = sort_ranges(reserved, nreserved);

    for (usize i = 0; i < nregions; i++) {
        if (regions[i].type == E820_USABLE && regions[i].base < HHDM_SIZE)
            add_usable(regions[i].base, MIN(regions[i].end, HHDM_SIZE), reserved, nreserved);
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

static long test_fail_after = -1; /* pmm_test_fail_after(): successes left, or -1 */

u64 pmm_alloc_pages(unsigned order)
{
    irqflags_t flags = spin_lock_irqsave(&pmm_lock);
    u64 pfn = test_fail_after == 0 ? BUDDY_NONE : buddy_alloc(&buddy, order);

    if (pfn != BUDDY_NONE && test_fail_after > 0)
        test_fail_after--;
    spin_unlock_irqrestore(&pmm_lock, flags);

    return pfn == BUDDY_NONE ? PMM_ALLOC_FAILED : pfn << PAGE_SHIFT;
}

long pmm_test_fail_after(long n)
{
    irqflags_t flags = spin_lock_irqsave(&pmm_lock);
    long previous = test_fail_after;

    test_fail_after = n;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return previous;
}

void pmm_free_pages(u64 phys, unsigned order)
{
    irqflags_t flags = spin_lock_irqsave(&pmm_lock);
    int err = IS_ALIGNED(phys, PAGE_SIZE) ? buddy_free(&buddy, pfn_of(phys), order) : -1;

    spin_unlock_irqrestore(&pmm_lock, flags);
    if (err != 0)
        panic("pmm: invalid free of %#llx (order %u): double free or wrong order?",
              (unsigned long long)phys, order);
}

void pmm_set_owner(u64 phys, u16 owner)
{
    irqflags_t flags = spin_lock_irqsave(&pmm_lock);

    BUG_ON(buddy_block_order(&buddy, pfn_of(phys)) < 0);
    buddy.pages[pfn_of(phys)].owner = owner;
    spin_unlock_irqrestore(&pmm_lock, flags);
}

int pmm_block_owner(u64 phys)
{
    irqflags_t flags = spin_lock_irqsave(&pmm_lock);
    int owner = IS_ALIGNED(phys, PAGE_SIZE) && buddy_block_order(&buddy, pfn_of(phys)) >= 0
                    ? buddy.pages[pfn_of(phys)].owner
                    : -1;

    spin_unlock_irqrestore(&pmm_lock, flags);
    return owner;
}

int pmm_block_order(u64 phys)
{
    irqflags_t flags = spin_lock_irqsave(&pmm_lock);
    int order = IS_ALIGNED(phys, PAGE_SIZE) ? buddy_block_order(&buddy, pfn_of(phys)) : -1;

    spin_unlock_irqrestore(&pmm_lock, flags);
    return order;
}

void pmm_get_stats(struct pmm_stats *out)
{
    irqflags_t flags = spin_lock_irqsave(&pmm_lock);

    out->managed_frames = buddy.managed_frames;
    out->free_frames = buddy.free_frames;
    out->metadata_bytes = metadata_bytes;
    spin_unlock_irqrestore(&pmm_lock, flags);
}

int pmm_verify(const char **reason)
{
    irqflags_t flags = spin_lock_irqsave(&pmm_lock);
    int err = buddy_verify(&buddy, reason);

    spin_unlock_irqrestore(&pmm_lock, flags);
    return err;
}
