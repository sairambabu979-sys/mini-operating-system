/* SPDX-License-Identifier: MIT */
/*
 * Virtually contiguous kernel allocations, each preceded by an unmapped
 * guard page. Backing frames need not be physically contiguous.
 *
 * Main client: kernel stacks. A stack overflow runs into the guard page
 * and faults immediately (escalating to #DF, handled on an IST stack)
 * instead of silently corrupting whatever lies below.
 */
#ifndef MINIOS_MM_VMALLOC_H
#define MINIOS_MM_VMALLOC_H

#include <compiler.h>
#include <types.h>

/*
 * A freed range is reused by an allocation of the same size once
 * VMALLOC_QUARANTINE newer ones were freed, and up to VMALLOC_REUSE_SLOTS
 * are kept (beyond that, the oldest range is abandoned). See vmalloc.c.
 */
#define VMALLOC_QUARANTINE  16
#define VMALLOC_REUSE_SLOTS 512

/* Allocates `size` bytes (rounded up to pages), zeroed. NULL on failure. */
__must_check void *vmalloc(usize size);

/* Frees a vmalloc() allocation. Panics on a pointer vmalloc did not return. */
void vfree(void *ptr);

/*
 * Maps physical [phys, phys + size) into the vmalloc region, behind a guard
 * page, and returns the virtual address of `phys` (which need not be page
 * aligned). For what the direct map deliberately leaves out: device
 * registers (VM_NOCACHE), framebuffers (VM_WC) and firmware tables in
 * reserved memory. `flags` may contain only VM_WRITE, VM_NOCACHE and
 * VM_WC; mappings are never executable.
 *
 * Returns NULL on a bad range, when out of address space, or for an
 * uncached or write-combining request covering RAM the direct map
 * already caches.
 * Mappings are permanent: nothing needs to unmap one yet.
 */
__must_check void *iomap(u64 phys, usize size, unsigned flags);

#endif /* MINIOS_MM_VMALLOC_H */
