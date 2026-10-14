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

/* Allocates `size` bytes (rounded up to pages), zeroed. NULL on failure. */
__must_check void *vmalloc(usize size);

/* Frees a vmalloc() allocation. Panics on a pointer vmalloc did not return. */
void vfree(void *ptr);

#endif /* MINIOS_MM_VMALLOC_H */
