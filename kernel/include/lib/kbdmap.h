/* SPDX-License-Identifier: MIT */
/*
 * PS/2 scan code set 1 to bytes, US layout.
 *
 * Pure code (host-tested). The 8042 translates whatever set the keyboard
 * speaks into set 1, so set 1 is all the kernel decodes. One scan code
 * gives at most one byte:
 *   - printable ASCII, with either Shift; Ctrl plus a letter gives the
 *     letter & 0x1F (Ctrl-C is 0x03), Ctrl plus anything else nothing;
 *   - Enter and keypad Enter 0x0A, Backspace 0x7F, Tab 0x09, Esc 0x1B;
 *   - the keypad always types digits (there is no Num Lock state).
 * Alt, the lock keys, the F-keys, the arrows and the other E0 keys, and
 * Pause give nothing; break codes only update the modifiers. The filler
 * bytes the controller and keyboard send (ACK 0xFA, resend 0xFE, echo
 * 0xEE, errors 0x00 and 0xFF) give nothing; the errors also drop a
 * pending prefix.
 */
#ifndef MINIOS_LIB_KBDMAP_H
#define MINIOS_LIB_KBDMAP_H

#include <types.h>

enum {
    KBDMAP_LSHIFT = 1 << 0,
    KBDMAP_RSHIFT = 1 << 1,
    KBDMAP_LCTRL = 1 << 2,
    KBDMAP_RCTRL = 1 << 3,
};

struct kbdmap {
    u8 mods;   /* KBDMAP_* held down */
    u8 prefix; /* 0, 0xE0 after an E0 byte, or 1-2: E1 bytes still to swallow */
};

/* Feeds one byte from the controller. Returns true, with the byte in *out, if it typed one. */
bool kbdmap_feed(struct kbdmap *k, u8 sc, u8 *out);

#endif /* MINIOS_LIB_KBDMAP_H */
