/* SPDX-License-Identifier: MIT */
/*
 * Interrupt-flag save/restore. The only sanctioned way to toggle IF outside
 * of entry code: callers save, do their work, and restore whatever state
 * they found, so nesting is always correct.
 */
#ifndef MINIOS_ARCH_IRQFLAGS_H
#define MINIOS_ARCH_IRQFLAGS_H

#include <arch/cpu.h>
#include <types.h>

typedef u64 irqflags_t;

static __always_inline irqflags_t arch_irq_save(void)
{
    irqflags_t flags;

    __asm__ volatile("pushfq\n\t"
                     "popq %0\n\t"
                     "cli"
                     : "=r"(flags)
                     :
                     : "memory");
    return flags;
}

static __always_inline void arch_irq_restore(irqflags_t flags)
{
    if (flags & RFLAGS_IF)
        cpu_irq_enable();
}

static __always_inline bool arch_irqs_enabled(void)
{
    return read_rflags() & RFLAGS_IF;
}

#endif /* MINIOS_ARCH_IRQFLAGS_H */
