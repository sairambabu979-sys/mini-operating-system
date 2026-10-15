// SPDX-License-Identifier: MIT
/*
 * Scan code set 1 to bytes, US layout. See <lib/kbdmap.h>.
 */
#include <lib/kbdmap.h>

#define SC_ESC       0x01
#define SC_BACKSPACE 0x0E
#define SC_TAB       0x0F
#define SC_ENTER     0x1C
#define SC_LCTRL     0x1D
#define SC_LSHIFT    0x2A
#define SC_RSHIFT    0x36
#define SC_KP_SLASH  0x35 /* after E0; plain 0x35 is '/' */
#define SC_LAST      0x58
#define SC_BREAK     0x80

#define BYTE_E0 0xE0
#define BYTE_E1 0xE1

/* Printable characters by make code, 0 for keys that type none; keypad digits included. */
static const char plain[SC_LAST + 1] = {
    [0x02] = '1',  [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',  [0x07] = '6',
    [0x08] = '7',  [0x09] = '8', [0x0A] = '9', [0x0B] = '0', [0x0C] = '-',  [0x0D] = '=',
    [0x10] = 'q',  [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',  [0x15] = 'y',
    [0x16] = 'u',  [0x17] = 'i', [0x18] = 'o', [0x19] = 'p', [0x1A] = '[',  [0x1B] = ']',
    [0x1E] = 'a',  [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',  [0x23] = 'h',
    [0x24] = 'j',  [0x25] = 'k', [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',  [0x30] = 'b',
    [0x31] = 'n',  [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',  [0x37] = '*',
    [0x39] = ' ',  [0x47] = '7', [0x48] = '8', [0x49] = '9', [0x4A] = '-',  [0x4B] = '4',
    [0x4C] = '5',  [0x4D] = '6', [0x4E] = '+', [0x4F] = '1', [0x50] = '2',  [0x51] = '3',
    [0x52] = '0',  [0x53] = '.',
};

/* The same keys with Shift held; the keypad is unaffected. */
static const char shifted[SC_LAST + 1] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%', [0x07] = '^',
    [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T', [0x15] = 'Y',
    [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G', [0x23] = 'H',
    [0x24] = 'J', [0x25] = 'K', [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|', [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M', [0x33] = '<', [0x34] = '>', [0x35] = '?', [0x37] = '*',
    [0x39] = ' ', [0x47] = '7', [0x48] = '8', [0x49] = '9', [0x4A] = '-', [0x4B] = '4',
    [0x4C] = '5', [0x4D] = '6', [0x4E] = '+', [0x4F] = '1', [0x50] = '2', [0x51] = '3',
    [0x52] = '0', [0x53] = '.',
};

static bool emit(u8 byte, u8 *out)
{
    *out = byte;
    return true;
}

/* A byte after E0: keypad Enter and '/', and the right Ctrl; every other E0 key types nothing. */
static bool feed_extended(struct kbdmap *k, u8 sc, u8 *out)
{
    bool ctrl = k->mods & (KBDMAP_LCTRL | KBDMAP_RCTRL);

    switch (sc) {
    case SC_LCTRL:
        k->mods |= KBDMAP_RCTRL;
        return false;
    case SC_LCTRL | SC_BREAK:
        k->mods &= (u8)~KBDMAP_RCTRL;
        return false;
    case SC_ENTER:
        return !ctrl && emit('\n', out);
    case SC_KP_SLASH:
        return !ctrl && emit('/', out);
    default: /* arrows, Home, Delete, the fake shifts E0 2A/AA/36/B6, ... */
        return false;
    }
}

static bool feed_break(struct kbdmap *k, u8 code)
{
    if (code == SC_LSHIFT)
        k->mods &= (u8)~KBDMAP_LSHIFT;
    else if (code == SC_RSHIFT)
        k->mods &= (u8)~KBDMAP_RSHIFT;
    else if (code == SC_LCTRL)
        k->mods &= (u8)~KBDMAP_LCTRL;
    return false;
}

static bool feed_make(struct kbdmap *k, u8 sc, u8 *out)
{
    bool shift = k->mods & (KBDMAP_LSHIFT | KBDMAP_RSHIFT);
    bool ctrl = k->mods & (KBDMAP_LCTRL | KBDMAP_RCTRL);

    switch (sc) {
    case SC_LSHIFT:
        k->mods |= KBDMAP_LSHIFT;
        return false;
    case SC_RSHIFT:
        k->mods |= KBDMAP_RSHIFT;
        return false;
    case SC_LCTRL:
        k->mods |= KBDMAP_LCTRL;
        return false;
    case SC_ESC:
        return !ctrl && emit(0x1B, out);
    case SC_BACKSPACE:
        return !ctrl && emit(0x7F, out);
    case SC_TAB:
        return !ctrl && emit('\t', out);
    case SC_ENTER:
        return !ctrl && emit('\n', out);
    default:
        break;
    }
    if (sc > SC_LAST || !plain[sc])
        return false;
    char c = plain[sc];

    if (ctrl)
        return c >= 'a' && c <= 'z' && emit((u8)(c & 0x1F), out);
    return emit((u8)(shift ? shifted[sc] : c), out);
}

bool kbdmap_feed(struct kbdmap *k, u8 sc, u8 *out)
{
    switch (sc) {
    case 0x00: /* key detection error or overrun */
    case 0xFF:
        k->prefix = 0;
        return false;
    case 0xFA: /* ACK */
    case 0xFE: /* resend */
    case 0xEE: /* echo */
        return false;
    default:
        break;
    }
    if (k->prefix == 1 || k->prefix == 2) { /* Pause: E1 1D 45 E1 9D C5 */
        k->prefix--;
        return false;
    }
    if (sc == BYTE_E1) {
        k->prefix = 2;
        return false;
    }
    if (sc == BYTE_E0) {
        k->prefix = BYTE_E0;
        return false;
    }
    if (k->prefix == BYTE_E0) {
        k->prefix = 0;
        return feed_extended(k, sc, out);
    }
    if (sc & SC_BREAK)
        return feed_break(k, sc & (u8)~SC_BREAK);
    return feed_make(k, sc, out);
}
