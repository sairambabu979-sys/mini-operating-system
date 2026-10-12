// SPDX-License-Identifier: MIT
/*
 * Physical memory manager tests, run against the real machine's memory.
 */
#include <arch/cpu.h>
#include <core/boot.h>
#include <core/ktest.h>
#include <mm/pmm.h>

static bool in_usable_ram(u64 phys, u64 len)
{
    usize n;
    const struct mem_region *r = pmm_regions(&n);

    for (usize i = 0; i < n; i++) {
        if (r[i].type == E820_USABLE && r[i].base <= phys && phys + len <= r[i].end)
            return true;
    }
    return false;
}

KTEST(pmm_is_consistent_after_init)
{
    const char *why = NULL;

    KEXPECT_EQ(pmm_verify(&why), 0);
}

KTEST(pmm_manages_nearly_all_usable_ram)
{
    struct pmm_stats st;
    usize n;
    const struct mem_region *r = pmm_regions(&n);
    u64 usable = 0;

    for (usize i = 0; i < n; i++) {
        if (r[i].type == E820_USABLE)
            usable += r[i].end - r[i].base;
    }
    pmm_get_stats(&st);
    u64 managed = st.managed_frames * PAGE_SIZE;
    const struct boot_info *bi = boot_info_get();
    u64 overhead =
        PMM_LOW_RESERVED_END + (bi->kernel_phys_end - bi->kernel_phys_start) + st.metadata_bytes;

    KEXPECT(managed <= usable);
    KEXPECT(managed + overhead + 2 * PAGE_SIZE >= usable); /* only the known reservations */
    KEXPECT(st.free_frames <= st.managed_frames);
}

KTEST(pmm_allocations_avoid_reserved_memory)
{
    const struct boot_info *bi = boot_info_get();

    for (unsigned order = 0; order <= 4; order++) {
        u64 p = pmm_alloc_pages(order);
        u64 len = PAGE_SIZE << order;

        KEXPECT(p != PMM_ALLOC_FAILED);
        KEXPECT(IS_ALIGNED(p, len));
        KEXPECT(p >= PMM_LOW_RESERVED_END);
        KEXPECT(p + len <= bi->kernel_phys_start || p >= bi->kernel_phys_end);
        KEXPECT(in_usable_ram(p, len));
        pmm_free_pages(p, order);
    }
}

/*
 * Drain the allocator page by page, threading a list through the pages
 * themselves (which also proves each one is real, writable RAM), then give
 * everything back and require the exact starting state.
 */
KTEST(pmm_exhaust_and_restore)
{
    struct pmm_stats before, drained, after;
    u64 head = 0, count = 0;
    const char *why = NULL;

    pmm_get_stats(&before);
    for (;;) {
        u64 p = pmm_alloc_page();

        if (p == PMM_ALLOC_FAILED)
            break;
        if (p >= (1ull << 30)) { /* outside the boot window: cannot touch yet */
            pmm_free_page(p);
            break;
        }
        *(volatile u64 *)phys_to_kimage(p) = head;
        head = p;
        count++;
    }
    pmm_get_stats(&drained);
    KEXPECT_EQ(count, before.free_frames);
    KEXPECT_EQ(drained.free_frames, 0);

    while (head != 0) {
        u64 next = *(volatile u64 *)phys_to_kimage(head);

        pmm_free_page(head);
        head = next;
    }
    pmm_get_stats(&after);
    KEXPECT_EQ(after.free_frames, before.free_frames);
    KEXPECT_EQ(pmm_verify(&why), 0);
}
