/* SPDX-License-Identifier: MIT */
#ifndef MINIOS_HOST_IRQFLAGS_H
#define MINIOS_HOST_IRQFLAGS_H
typedef unsigned long irqflags_t;
static inline irqflags_t arch_irq_save(void)
{
    return 0;
}
static inline void arch_irq_restore(irqflags_t flags)
{
    (void)flags;
}
#endif
