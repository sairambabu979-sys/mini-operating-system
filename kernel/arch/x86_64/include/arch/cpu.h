/* SPDX-License-Identifier: MIT */
/*
 * Single-instruction CPU primitives. Anything larger lives in a .c/.asm file.
 */
#ifndef MINIOS_ARCH_CPU_H
#define MINIOS_ARCH_CPU_H

#include <compiler.h>
#include <types.h>

/* Virtual base of the kernel image window (physical 0 maps here). */
#define KERNEL_VMA 0xFFFFFFFF80000000ull

static __always_inline void cpu_halt(void)
{
    __asm__ volatile("hlt" ::: "memory");
}

static __always_inline void cpu_irq_disable(void)
{
    __asm__ volatile("cli" ::: "memory");
}

static __always_inline void cpu_irq_enable(void)
{
    __asm__ volatile("sti" ::: "memory");
}

static __always_inline void cpu_pause(void)
{
    __asm__ volatile("pause");
}

#define CR0_PE (1ull << 0)
#define CR0_WP (1ull << 16) /* honor read-only pages in ring 0 */
#define CR0_PG (1ull << 31)

#define CR4_PAE  (1ull << 5)
#define CR4_PGE  (1ull << 7)
#define CR4_SMEP (1ull << 20)
#define CR4_SMAP (1ull << 21)

#define RFLAGS_IF (1ull << 9)
#define RFLAGS_DF (1ull << 10)

#define DEFINE_READ_CR(n)                                                                          \
    static inline u64 read_cr##n(void)                                                             \
    {                                                                                              \
        u64 v;                                                                                     \
        __asm__ volatile("mov %%cr" #n ", %0" : "=r"(v));                                          \
        return v;                                                                                  \
    }
DEFINE_READ_CR(0)
DEFINE_READ_CR(2)
DEFINE_READ_CR(3)
DEFINE_READ_CR(4)
#undef DEFINE_READ_CR

static inline u64 read_rflags(void)
{
    u64 v;

    __asm__ volatile("pushfq; popq %0" : "=r"(v));
    return v;
}

static __always_inline uptr read_rsp(void)
{
    uptr v;

    __asm__ volatile("mov %%rsp, %0" : "=r"(v));
    return v;
}

static __always_inline uptr read_rbp(void)
{
    uptr v;

    __asm__ volatile("mov %%rbp, %0" : "=r"(v));
    return v;
}

/* Stops this CPU for good: interrupts off, then halt in a loop. */
static inline __noreturn void cpu_halt_forever(void)
{
    for (;;) {
        cpu_irq_disable();
        cpu_halt();
    }
}

/*
 * Physical-to-virtual through the kernel image window. Valid only for the
 * first 1 GiB of physical memory, which the bootloader maps at KERNEL_VMA.
 * Used before the kernel builds its own direct map.
 */
static inline void *phys_to_kimage(u64 phys)
{
    return (void *)(uptr)(phys + KERNEL_VMA);
}

#endif /* MINIOS_ARCH_CPU_H */
