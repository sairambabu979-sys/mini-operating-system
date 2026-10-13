// SPDX-License-Identifier: MIT
/*
 * Memory bring-up order. Each step depends on the previous one:
 *
 *   memmap_sanitize   pure, needs only boot_info
 *   vmm_init          page tables from a static pool; direct map goes live
 *   pmm_init          metadata reached through the direct map
 *   vmm_init_late     PMM-backed tables for the rest of the kernel half
 */
#include <arch/cpu.h>
#include <arch/paging.h>
#include <mm/mm.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

static struct mem_region regions[BOOT_MMAP_MAX];
static usize nregions;

const struct mem_region *mm_regions(usize *count)
{
    *count = nregions;
    return regions;
}

void mm_init(const struct boot_info *bi)
{
    nregions = memmap_sanitize(bi->mmap, bi->mmap_count, regions, ARRAY_SIZE(regions));
    vmm_init(regions, nregions, bi);
    pmm_init(regions, nregions, bi);
    vmm_init_late();
}
