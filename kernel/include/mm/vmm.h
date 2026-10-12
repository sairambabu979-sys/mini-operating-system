/* SPDX-License-Identifier: MIT */
/*
 * Virtual memory manager: page tables and address spaces.
 *
 * Invariants (checked where cheap, documented everywhere):
 *   - W^X: no mapping is both writable and executable.
 *   - The direct map (HHDM) covers firmware-described RAM only (never MMIO
 *     holes), is never executable, and maps the kernel image read-only.
 *   - After vmm_init_late(), the kernel-half PML4 entries [256, 511] never
 *     change, so every address space can share them by copying them once.
 */
#ifndef MINIOS_MM_VMM_H
#define MINIOS_MM_VMM_H

#include <boot/boot_info.h>
#include <compiler.h>
#include <mm/memmap.h>
#include <types.h>

/* Mapping permissions. Readable is implied. */
enum vm_flags {
    VM_WRITE = 1u << 0,
    VM_EXEC = 1u << 1,
    VM_USER = 1u << 2,
    VM_NOCACHE = 1u << 3, /* device memory */
    VM_GLOBAL = 1u << 4,  /* kernel mappings: survive CR3 switches in the TLB */
};

struct address_space {
    u64 pml4_phys;
};

struct address_space *vmm_kernel_space(void);

/*
 * Phase 1: builds the kernel's own page tables from a static pool (no
 * allocator needed), enables NX/WP/PGE/SMEP/SMAP, and switches CR3. The
 * bootloader's identity map is gone afterwards.
 */
void vmm_init(const struct mem_region *regions, usize nregions, const struct boot_info *bi);

/* Phase 2 (needs the PMM): pre-allocates every kernel-half PDPT. */
void vmm_init_late(void);

/* True once vmm_init() has switched to the kernel's page tables. */
bool vmm_is_active(void);

/*
 * Maps [va, va + size) to [pa, pa + size) with 4 KiB pages. All arguments
 * page-aligned. -EINVAL on misalignment or a W+X request, -EEXIST if any
 * page is already mapped, -ENOMEM if a page table cannot be allocated.
 */
__must_check int vmm_map(struct address_space *as, uptr va, u64 pa, usize size, unsigned flags);

/* Unmaps one 4 KiB page, flushing the TLB entry. Returns its frame in *pa. */
__must_check int vmm_unmap_page(struct address_space *as, uptr va, u64 *pa);

/* Looks up va; on success stores the physical address and the leaf entry. */
bool vmm_translate(const struct address_space *as, uptr va, u64 *pa, u64 *pte);

#endif /* MINIOS_MM_VMM_H */
