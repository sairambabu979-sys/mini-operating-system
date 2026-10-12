/* SPDX-License-Identifier: MIT */
/*
 * Hardware interrupt lines (IRQ 0-15 on the 8259 PIC).
 *
 * Handlers run with interrupts disabled, on the interrupted context's
 * kernel stack, and must not sleep. The EOI is sent after the handler.
 */
#ifndef MINIOS_CORE_IRQ_H
#define MINIOS_CORE_IRQ_H

#include <arch/trap.h>
#include <compiler.h>
#include <types.h>

typedef void (*irq_handler_t)(struct trap_frame *frame, void *ctx);

void irq_init(void);

/* Installs a handler and unmasks the line. -EBUSY if one is installed. */
__must_check int irq_register(unsigned line, irq_handler_t handler, void *ctx);

/* Called by trap_dispatch for vectors in the IRQ range. */
void irq_dispatch(struct trap_frame *frame);

/* True while an IRQ handler is running. */
bool in_irq(void);

u64 irq_count(unsigned line);
u64 irq_spurious_count(void);

/* Hook run on every interrupt return to a preemptible context (scheduler). */
void irq_exit_hook(void);

#endif /* MINIOS_CORE_IRQ_H */
