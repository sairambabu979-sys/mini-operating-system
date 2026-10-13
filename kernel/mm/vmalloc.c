// SPDX-License-Identifier: MIT
/*
 * vmalloc. See <mm/vmalloc.h>.
 *
 * Layout of the vmalloc region:
 *
 *     [guard][area 0 ...][guard][area 1 ......][guard][area 2] ...
 *
 * Virtual addresses come from a bump pointer. A freed area's range is
 * reused, but only by an allocation of the same size and only after
 * QUARANTINE newer frees: a dangling pointer into a freed area keeps
 * faulting for that long, rather than aliasing a new allocation. Reuse is
 * what bounds the page tables. vfree() never frees one (vmm_translate()
 * walks kernel tables without a lock), so with fresh VA for every kernel
 * stack, each task creation used to cost page-table memory for good: one
 * frame per ~100 tasks, from any user program that spawns in a loop.
 * (On SMP, reuse will also need a TLB shootdown.) The extent of an area
 * needs no metadata: it ends at the next unmapped page, the following
 * area's guard.
 */
#include <arch/cpuid.h>
#include <arch/paging.h>
#include <core/panic.h>
#include <core/spinlock.h>
#include <lib/string.h>
#include <mm/pmm.h>
#include <mm/vmalloc.h>
#include <mm/vmm.h>

#define FREED_MAX  VMALLOC_REUSE_SLOTS
#define QUARANTINE VMALLOC_QUARANTINE

struct freed_range {
    uptr base; /* first page after the guard */
    usize npages;
};

static uptr next_va = VMALLOC_BASE;
static struct freed_range freed[FREED_MAX]; /* FIFO ring, oldest at freed_head */
static unsigned freed_head, nfreed;
/* Protects next_va and the ring: once reserved, an area belongs to its caller. */
static struct spinlock va_lock = SPINLOCK_INIT("vmalloc");

/* The oldest freed range of exactly `npages` past the quarantine, removed; or 0. */
static uptr reuse_va_locked(usize npages)
{
    for (unsigned i = 0; i + QUARANTINE < nfreed; i++) {
        struct freed_range *r = &freed[(freed_head + i) % FREED_MAX];

        if (r->npages != npages)
            continue;
        uptr base = r->base;

        /* Close the gap: shift the older entries up by one. */
        for (unsigned j = i; j > 0; j--)
            freed[(freed_head + j) % FREED_MAX] = freed[(freed_head + j - 1) % FREED_MAX];
        freed_head = (freed_head + 1) % FREED_MAX;
        nfreed--;
        return base;
    }
    return 0;
}

static void release_va(uptr base, usize npages)
{
    irqflags_t flags = spin_lock_irqsave(&va_lock);

    if (nfreed == FREED_MAX) {
        freed_head = (freed_head + 1) % FREED_MAX;
        nfreed--;
    }
    freed[(freed_head + nfreed) % FREED_MAX] = (struct freed_range){base, npages};
    nfreed++;
    spin_unlock_irqrestore(&va_lock, flags);
}

static bool page_mapped(uptr va)
{
    u64 pa, pte;

    return vmm_translate(vmm_kernel_space(), va, &pa, &pte);
}

/* Reserves a guard page plus `npages`; returns the first page after the guard, or 0. */
static uptr reserve_va(usize npages)
{
    usize span = (npages + 1) * PAGE_SIZE_4K; /* + guard */
    irqflags_t flags = spin_lock_irqsave(&va_lock);

    if (npages == 0 || span / PAGE_SIZE_4K != npages + 1) {
        spin_unlock_irqrestore(&va_lock, flags);
        return 0;
    }
    uptr base = reuse_va_locked(npages);

    if (base || span > VMALLOC_BASE + VMALLOC_SIZE - next_va) {
        spin_unlock_irqrestore(&va_lock, flags);
        return base;
    }
    base = next_va + PAGE_SIZE_4K; /* the first page stays unmapped */

    next_va += span;
    spin_unlock_irqrestore(&va_lock, flags);
    return base;
}

void *vmalloc(usize size)
{
    if (size == 0)
        return NULL;
    usize npages = ALIGN_UP(size, PAGE_SIZE_4K) / PAGE_SIZE_4K;
    uptr base = reserve_va(npages);

    if (!base)
        return NULL;
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

    u64 frame, pte;

    if (vmm_translate(vmm_kernel_space(), va, &frame, &pte) && (pte & PTE_SW_IOMAP))
        panic("vfree: %p is an iomap() mapping; its frames are not the PMM's", ptr);

    uptr base = va;

    while (page_mapped(va)) {
        u64 pa;

        BUG_ON(vmm_unmap_page(vmm_kernel_space(), va, &pa) != 0);
        pmm_free_page(pa);
        va += PAGE_SIZE_4K;
    }
    release_va(base, (va - base) / PAGE_SIZE_4K);
}

void *iomap(u64 phys, usize size, unsigned flags)
{
    u64 first = ALIGN_DOWN(phys, PAGE_SIZE_4K);

    if (size == 0 || (flags & ~(VM_WRITE | VM_NOCACHE | VM_WC)) || phys + size < phys ||
        phys + size - 1 >= 1ull << cpu_features()->phys_addr_bits)
        return NULL;
    usize npages = (ALIGN_UP(phys + size, PAGE_SIZE_4K) - first) / PAGE_SIZE_4K;

    /*
     * Two mappings of one frame with different memory types are undefined
     * (SDM Vol. 3A, 12.12.4): device memory must not also be cached RAM.
     */
    if (flags & (VM_NOCACHE | VM_WC))
        for (usize i = 0; i < npages; i++)
            if (first + i * PAGE_SIZE_4K < HHDM_SIZE &&
                page_mapped((uptr)phys_to_virt(first + i * PAGE_SIZE_4K)))
                return NULL;

    uptr base = reserve_va(npages);

    if (!base || vmm_map(vmm_kernel_space(), base, first, npages * PAGE_SIZE_4K,
                         flags | VM_GLOBAL | VM_IOMAP) != 0)
        return NULL; /* a failed map leaves the VA range abandoned, as in vmalloc */
    return (void *)(base + (phys - first));
}
