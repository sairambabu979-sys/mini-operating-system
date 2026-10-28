/* SPDX-License-Identifier: MIT */
/*
 * The 8042 PS/2 controller, keyboard port only: port I/O, no state of
 * its own. The keyboard's IRQ 1 handler lives in core/input.c, which
 * feeds the bytes to the set 1 decoder.
 *
 * Nothing here can hang: every wait for the controller has a deadline,
 * and a status byte of 0xFF means there is no controller (an absent
 * port floats high). The output port (commands 0xD0 and 0xD1, which own
 * the A20 gate and the reset line) is never touched. The mouse port is
 * disabled and its clock stopped; a byte from it is dropped and counted.
 */
#ifndef MINIOS_DRIVERS_I8042_H
#define MINIOS_DRIVERS_I8042_H

#include <compiler.h>
#include <types.h>

/* Bits of the controller configuration byte. */
#define I8042_CFG_KBD_IRQ   0x01
#define I8042_CFG_AUX_IRQ   0x02
#define I8042_CFG_SYSTEM    0x04 /* POST passed: kept as the firmware set it */
#define I8042_CFG_KBD_OFF   0x10 /* keyboard clock disabled */
#define I8042_CFG_AUX_OFF   0x20 /* mouse clock disabled */
#define I8042_CFG_TRANSLATE 0x40 /* keyboard bytes translated to scan code set 1 */

/*
 * Brings the keyboard port up: both ports off, the output buffer flushed,
 * the configuration byte rewritten to translation on and the mouse off
 * (read back to check it took), the keyboard port and scanning enabled,
 * and only then the keyboard IRQ. Writes the configuration byte found and
 * the one left. False, with a warning, if there is no controller or it
 * stops answering; the caller then has no keyboard.
 */
bool i8042_init(u8 *old_config, u8 *new_config);

enum {
    I8042_EMPTY = -1, /* nothing waiting, or no controller */
    I8042_AUX = -2,   /* a mouse byte, read and dropped */
};

/* One byte from the keyboard, without waiting; I8042_EMPTY or I8042_AUX otherwise. */
int i8042_read_kbd(void);

/* Mouse bytes dropped by i8042_read_kbd(). */
u64 i8042_aux_dropped(void);

/*
 * Reads the configuration byte, interrupts off around the command and
 * its reply. For tests: a keystroke arriving at that moment would be
 * read as the reply.
 */
__must_check bool i8042_read_config(u8 *config);

/* True if a byte is waiting in the output buffer. For tests. */
bool i8042_output_full(void);

/*
 * For tests: has the controller deliver `byte` as if the keyboard had
 * sent it (command 0xD2), raising IRQ 1. The byte skips translation, so
 * it must already be set 1. A second call before the first byte is read
 * overwrites it.
 */
__must_check bool i8042_test_put(u8 byte);

#endif /* MINIOS_DRIVERS_I8042_H */
