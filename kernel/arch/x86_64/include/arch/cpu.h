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
