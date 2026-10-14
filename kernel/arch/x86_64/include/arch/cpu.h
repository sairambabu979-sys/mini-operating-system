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
#define CR0_MP (1ull << 1)  /* monitor coprocessor */
#define CR0_EM (1ull << 2)  /* x87 emulation: must be clear for SSE */
#define CR0_TS (1ull << 3)  /* task switched: lazy-FPU trap, unused */
#define CR0_NE (1ull << 5)  /* x87 errors raise #MF, not the legacy FERR#/IRQ 13 */
#define CR0_WP (1ull << 16) /* honor read-only pages in ring 0 */
#define CR0_PG (1ull << 31)

#define CR4_PAE        (1ull << 5)
#define CR4_MCE        (1ull << 6) /* machine checks raise #MC instead of a shutdown */
#define CR4_PGE        (1ull << 7)
#define CR4_OSFXSR     (1ull << 9)  /* FXSAVE/FXRSTOR and SSE enabled */
#define CR4_OSXMMEXCPT (1ull << 10) /* unmasked SSE exceptions raise #XM */
#define CR4_SMEP       (1ull << 20)
#define CR4_SMAP       (1ull << 21)

#define RFLAGS_CF       (1ull << 0)
#define RFLAGS_PF       (1ull << 2)
#define RFLAGS_AF       (1ull << 4)
#define RFLAGS_ZF       (1ull << 6)
#define RFLAGS_SF       (1ull << 7)
#define RFLAGS_TF       (1ull << 8)
#define RFLAGS_IF       (1ull << 9)
#define RFLAGS_AC       (1ull << 18)
#define RFLAGS_RESERVED (1ull << 1) /* always reads as 1 */
#define RFLAGS_DF       (1ull << 10)
#define RFLAGS_OF       (1ull << 11)
#define RFLAGS_IOPL     (3ull << 12)
#define RFLAGS_NT       (1ull << 14) /* an iretq with NT set raises #GP in 64-bit mode */
#define RFLAGS_RF       (1ull << 16)
#define RFLAGS_ID       (1ull << 21)

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
 * Physical-to-virtual through the bootloader's kernel window, which maps
 * the first BOOT_WINDOW_SIZE of physical memory at KERNEL_VMA. Valid only
 * until vmm_init() switches page tables; use phys_to_virt() after that.
 */
#define BOOT_WINDOW_SIZE (1ull << 30)

static inline void *phys_to_kimage(u64 phys)
{
    return (void *)(uptr)(phys + KERNEL_VMA);
}

#endif /* MINIOS_ARCH_CPU_H */
