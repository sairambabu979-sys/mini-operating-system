// SPDX-License-Identifier: MIT
/*
 * The initrd. See <fs/initrd.h>.
 */
#define pr_fmt(fmt) "initrd: " fmt

#include <arch/paging.h>
#include <core/klog.h>
#include <fs/initrd.h>
#include <lib/tar.h>
#include <mm/mm.h>

static const u8 *archive;
static usize archive_len;

/* True if every byte of [start, end) is usable RAM (the regions are sorted and disjoint). */
static bool all_usable(u64 start, u64 end)
{
    usize n;
    const struct mem_region *r = mm_regions(&n);

    for (usize i = 0; i < n && start < end; i++)
        if (r[i].type == E820_USABLE && r[i].base <= start && r[i].end > start)
            start = r[i].end;
    return start >= end;
}

void initrd_init(const struct boot_info *bi)
{
    u64 phys = bi->initrd_phys, size = bi->initrd_size;

    if (size == 0) {
        pr_info("none\n");
        return;
    }
    if (!all_usable(phys, phys + size)) { /* validate_boot_info: no overflow */
        pr_err("[%#llx-%#llx) is not all usable RAM: not used\n", (unsigned long long)phys,
               (unsigned long long)(phys + size));
        return;
    }
    const u8 *p = phys_to_virt(phys);
    int entries = tar_validate(p, (usize)size);

    if (entries < 0) {
        pr_err("[%#llx-%#llx) is not a valid ustar archive: not used\n", (unsigned long long)phys,
               (unsigned long long)(phys + size));
        return;
    }
    archive = p;
    archive_len = (usize)size;
    pr_info("%d entries, %llu KiB at %#llx\n", entries, (unsigned long long)(size + 1023) / 1024,
            (unsigned long long)phys);
}

const u8 *initrd_get(usize *len)
{
    *len = archive_len;
    return archive;
}
