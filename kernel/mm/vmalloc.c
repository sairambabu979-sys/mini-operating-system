// SPDX-License-Identifier: MIT
/*
 * vmalloc. See <mm/vmalloc.h>.
 *
 * Layout of the vmalloc region:
 *
 *     [guard][area 0 ...][guard][area 1 ......][guard][area 2] ...
 *
 * Virtual addresses come from a bump pointer and are never reused: the
 * region is 512 GiB, enough for millions of kernel stacks, and never
 * reusing a VA means a stale TLB entry or dangling pointer into a freed
 * area always faults, never aliases a new allocation. (On SMP, reuse would
 * also require TLB shootdowns.) The extent of an area needs no metadata:
 * it ends at the next unmapped page, the following area's guard.
 */
#include <arch/paging.h>
#include <core/panic.h>
#include <core/spinlock.h>
#include <lib/string.h>
#include <mm/pmm.h>
#include <mm/vmalloc.h>
#include <mm/vmm.h>

static uptr next_va = VMALLOC_BASE;
/* Protects next_va only: once reserved, an area belongs to its caller. */
static struct spinlock va_lock = SPINLOCK_INIT("vmalloc");

static bool page_mapped(uptr va)
{
    u64 pa, pte;

    return vmm_translate(vmm_kernel_space(), va, &pa, &pte);
}

void *vmalloc(usize size)
{
    if (size == 0)
        return NULL;
    usize npages = ALIGN_UP(size, PAGE_SIZE_4K) / PAGE_SIZE_4K;
    usize span = (npages + 1) * PAGE_SIZE_4K; /* + guard */

    irqflags_t flags = spin_lock_irqsave(&va_lock);

    if (span > VMALLOC_BASE + VMALLOC_SIZE - next_va) {
        spin_unlock_irqrestore(&va_lock, flags);
        return NULL;
    }
    uptr base = next_va + PAGE_SIZE_4K; /* the first page stays unmapped */

    next_va += span;
    spin_unlock_irqrestore(&va_lock, flags);
    for (usize i = 0; i < npages; i++) {
        u64 frame = pmm_alloc_page();
        uptr va = base + i * PAGE_SIZE_4K;

        if (frame == PMM_ALLOC_FAILED ||
            vmm_map(vmm_kernel_space(), va, frame, PAGE_SIZE_4K, VM_WRITE | VM_GLOBAL) != 0) {
            if (frame != PMM_ALLOC_FAILED)
                pmm_free_page(frame);
            /* Roll back what was mapped; the VA range is simply abandoned. */
            for (usize j = 0; j < i; j++) {
                u64 pa;

                if (vmm_unmap_page(vmm_kernel_space(), base + j * PAGE_SIZE_4K, &pa) == 0)
                    pmm_free_page(pa);
            }
            return NULL;
        }
        memset((void *)va, 0, PAGE_SIZE_4K);
    }
    return (void *)base;
}

void vfree(void *ptr)
{
    uptr va = (uptr)ptr;

    if (ptr == NULL)
        return;
    if (va < VMALLOC_BASE + PAGE_SIZE_4K || va >= __atomic_load_n(&next_va, __ATOMIC_RELAXED) ||
        !IS_ALIGNED(va, PAGE_SIZE_4K) || page_mapped(va - PAGE_SIZE_4K) || !page_mapped(va))
        panic("vfree: %p is not the start of a live vmalloc area", ptr);

    while (page_mapped(va)) {
        u64 pa;

        BUG_ON(vmm_unmap_page(vmm_kernel_space(), va, &pa) != 0);
        pmm_free_page(pa);
        va += PAGE_SIZE_4K;
    }
}
