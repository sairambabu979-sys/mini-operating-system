/* SPDX-License-Identifier: MIT */
/*
 * Dual 8259A programmable interrupt controller (master + cascaded slave).
 */
#ifndef MINIOS_DRIVERS_PIC8259_H
#define MINIOS_DRIVERS_PIC8259_H

#include <types.h>

#define PIC_IRQ_LINES 16
#define PIC_CASCADE   2 /* the slave is wired to master input 2 */

/* Remaps IRQ 0-15 to vectors base..base+15 and masks every line. */
void pic_init(u8 vector_base);
void pic_mask(unsigned line);
void pic_unmask(unsigned line);
bool pic_is_masked(unsigned line);
void pic_eoi(unsigned line);

/*
 * Lines 7 and 15 can fire spuriously (the request vanished before the CPU
 * acknowledged it). Returns true if so; the caller must then skip the
 * handler and this function has already sent any EOI the cascade needs.
 */
bool pic_handle_spurious(unsigned line);

#endif /* MINIOS_DRIVERS_PIC8259_H */
