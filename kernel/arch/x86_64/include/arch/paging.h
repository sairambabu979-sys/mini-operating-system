/* SPDX-License-Identifier: MIT */
/*
 * x86_64 4-level paging: virtual layout, entry bits, TLB control.
 *
 * Virtual address map (PML4 index in brackets):
 *
 *   0x0000000000000000 - 0x00007FFFFFFFFFFF  [0-255]  user space (per process)
 *   0xFFFF800000000000 - +512 GiB            [256]    direct map of physical RAM
 *   0xFFFFC00000000000 - +512 GiB            [384]    vmalloc: guarded kernel stacks etc.
 *   0xFFFFFFFF80000000 - 0xFFFFFFFFFFFFFFFF  [511]    kernel image (-mcmodel=kernel)
 */
#ifndef MINIOS_ARCH_PAGING_H
#define MINIOS_ARCH_PAGING_H

#include <compiler.h>
#include <types.h>

#define HHDM_BASE    0xFFFF800000000000ull
#define HHDM_SIZE    (512ull << 30)
#define VMALLOC_BASE 0xFFFFC00000000000ull
#define VMALLOC_SIZE (512ull << 30)
#define KERNEL_HALF  0xFFFF800000000000ull /* first kernel-half address */

#define PAGE_SIZE_4K 0x1000ull
#define PAGE_SIZE_2M 0x200000ull
#define PAGE_SIZE_1G 0x40000000ull

#define PT_ENTRIES 512

/* Page-table entry bits (SDM Vol. 3A, 4.5). */
#define PTE_PRESENT  (1ull << 0)
#define PTE_WRITE    (1ull << 1)
#define PTE_USER     (1ull << 2)
#define PTE_PWT      (1ull << 3)
#define PTE_PCD      (1ull << 4)
#define PTE_ACCESSED (1ull << 5)
#define PTE_DIRTY    (1ull << 6)
#define PTE_HUGE     (1ull << 7) /* PS: 2 MiB in a PD, 1 GiB in a PDPT */
#define PTE_GLOBAL   (1ull << 8)
#define PTE_NX       (1ull << 63)
#define PTE_ADDR     0x000FFFFFFFFFF000ull

static inline unsigned pml4_index(uptr va)
{
    return (va >> 39) & 0x1FF;
}

static inline unsigned pdpt_index(uptr va)
{
    return (va >> 30) & 0x1FF;
}

static inline unsigned pd_index(uptr va)
{
    return (va >> 21) & 0x1FF;
}

static inline unsigned pt_index(uptr va)
{
    return (va >> 12) & 0x1FF;
}

static inline bool va_is_canonical(uptr va)
{
    return (uptr)((s64)(va << 16) >> 16) == va;
}

/* Direct-map translation. Valid once the VMM has switched to its own tables. */
static inline void *phys_to_virt(u64 phys)
{
    return (void *)(uptr)(phys + HHDM_BASE);
}

static inline u64 virt_to_phys_hhdm(const void *va)
{
    return (u64)(uptr)va - HHDM_BASE;
}

static inline void invlpg(uptr va)
{
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

static inline void write_cr3(u64 pml4_phys)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

static inline void write_cr0(u64 v)
{
    __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}

static inline void write_cr4(u64 v)
{
    __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}

/* SMAP: permit / forbid supervisor access to user pages (AC flag). */
static __always_inline void stac(void)
{
    __asm__ volatile("stac" ::: "memory");
}

static __always_inline void clac(void)
{
    __asm__ volatile("clac" ::: "memory");
}

#endif /* MINIOS_ARCH_PAGING_H */
