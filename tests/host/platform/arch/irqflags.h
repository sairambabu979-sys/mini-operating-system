/* SPDX-License-Identifier: MIT */
/* Interrupt flags for kernel code compiled into the host tests: no interrupts, nothing to save. */
#ifndef MINIOS_ARCH_IRQFLAGS_H
#define MINIOS_ARCH_IRQFLAGS_H

#include <types.h>

typedef u64 irqflags_t;

static inline irqflags_t arch_irq_save(void)
{
    return 0;
}

static inline void arch_irq_restore(irqflags_t flags)
{
    (void)flags;
}

#endif /* MINIOS_ARCH_IRQFLAGS_H */
