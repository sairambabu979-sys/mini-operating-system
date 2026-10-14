/* SPDX-License-Identifier: MIT */
/*
 * Hardware interrupts.
 *
 * Two kinds of source:
 *   - ISA IRQ lines 0-15, on vectors VEC_FIRST_IRQ + line, through the
 *     I/O APIC (routed per the MADT's overrides) or, without one, through
 *     the 8259 PIC. Drivers name the ISA line either way.
 *   - Local APIC vectors (VEC_FIRST_LOCAL and up): its timer, its error
 *     and spurious interrupts, and inter-processor interrupts.
 *
 * Handlers run with interrupts disabled, on the interrupted context's
 * kernel stack, and must not sleep. The EOI is sent after the handler.
 */
#ifndef MINIOS_CORE_IRQ_H
#define MINIOS_CORE_IRQ_H

#include <arch/trap.h>
#include <compiler.h>
#include <types.h>

#define IRQ_LINES 16

typedef void (*irq_handler_t)(struct trap_frame *frame, void *ctx);

enum irq_mode {
    IRQ_MODE_PIC,  /* 8259 pair; no local APIC in use */
    IRQ_MODE_APIC, /* local APIC + I/O APIC; the 8259s fully masked */
};

/*
 * Picks the controller: the APICs when the CPU has a local APIC, the MADT
 * lists an I/O APIC that answers, and "noapic" is not on the command line;
 * the 8259 otherwise. Needs acpi_init().
 */
void irq_init(void);
enum irq_mode irq_mode(void);
const char *irq_controller_name(void);
/* Why irq_init() chose the 8259 ("noapic on the command line", "no MADT", ...); NULL on the APIC path. */
const char *irq_pic_reason(void);

/* Installs a handler and unmasks the line. -EBUSY if one is installed, -ENODEV if unwired. */
__must_check int irq_register(unsigned line, irq_handler_t handler, void *ctx);
/* Masks the line and removes its handler. */
void irq_unregister(unsigned line);
void irq_mask(unsigned line);
void irq_unmask(unsigned line);

/* The same for a local APIC vector (APIC mode only; -ENODEV otherwise). */
__must_check int irq_register_local(u8 vector, irq_handler_t handler, void *ctx);
void irq_unregister_local(u8 vector);

/* Called by trap_dispatch for vectors at or above VEC_FIRST_IRQ. */
void irq_dispatch(struct trap_frame *frame);

/* True while an IRQ handler is running. */
bool in_irq(void);

u64 irq_count(unsigned line);
u64 irq_local_count(u8 vector);
u64 irq_spurious_count(void);
u64 irq_apic_error_count(void);

/* Hook run on every interrupt return to a preemptible context (scheduler). */
void irq_exit_hook(void);

#endif /* MINIOS_CORE_IRQ_H */
