// SPDX-License-Identifier: MIT
/*
 * Line editing on raw input bytes. See lineedit.h.
 */
#include "lineedit.h"

#define CTRL_C      0x03
#define CTRL_D      0x04
#define BACKSPACE   0x08
#define CTRL_U      0x15
#define ESC         0x1B
#define DEL         0x7F
#define ESC_SEQ_MAX 16 /* a longer "sequence" is noise: give up on it */

enum { ESC_NONE, ESC_SEEN, ESC_SEQ };

static void put(char *echo, usize *n, const char *s)
{
    while (*s)
        echo[(*n)++] = *s++;
}

void le_reset(struct lineedit *le)
{
    *le = (struct lineedit){0};
}

static enum le_event end_line(struct lineedit *le, char *echo, usize *n)
{
    le->buf[le->len] = '\0';
    le->ended = true;
    put(echo, n, "\n");
    return LE_DONE;
}

/* Inside ESC [ or ESC O: true if the byte was swallowed as part of the sequence. */
static bool in_sequence(struct lineedit *le, u8 c)
{
    if (c < 0x20) { /* a control byte cuts the sequence short and counts for itself */
        le->esc = ESC_NONE;
        return false;
    }
    if (c == '[' && le->esc_len == 0) { /* the Linux console's F1-F5: ESC [ [ A..E */
        le->esc_len++;
        return true;
    }
    if ((c >= 0x40 && c <= 0x7E) || ++le->esc_len >= ESC_SEQ_MAX)
        le->esc = ESC_NONE; /* the final byte, or too long to be one */
    return true;
}

enum le_event le_feed(struct lineedit *le, u8 c, char *echo, usize *echo_len)
{
    bool after_cr = le->last_cr;

    *echo_len = 0;
    le->last_cr = false;
    if (le->ended) {
        le->len = 0;
        le->ended = false;
    }

    if (le->esc == ESC_SEQ && in_sequence(le, c))
        return LE_MORE;
    if (le->esc == ESC_SEEN) {
        le->esc = ESC_NONE;
        if (c == '[' || c == 'O') {
            le->esc = ESC_SEQ;
            le->esc_len = 0;
            return LE_MORE;
        }
        /* a lone ESC: drop it and take this byte as it is */
    }

    switch (c) {
    case '\r':
        le->last_cr = true;
        return end_line(le, echo, echo_len);
    case '\n':
        if (after_cr)
            return LE_MORE; /* the LF of a CRLF */
        return end_line(le, echo, echo_len);
    case DEL:
    case BACKSPACE:
        if (le->len) {
            le->len--;
            put(echo, echo_len, "\b \b");
        }
        return LE_MORE;
    case CTRL_U:
        for (; le->len; le->len--)
            put(echo, echo_len, "\b \b");
        return LE_MORE;
    case CTRL_C:
        le->len = 0;
        le->ended = true;
        put(echo, echo_len, "^C\n");
        return LE_CANCEL;
    case CTRL_D:
        if (le->len)
            return LE_MORE;
        le->ended = true;
        return LE_EOF;
    case ESC:
        le->esc = ESC_SEEN;
        return LE_MORE;
    default:
        break;
    }
    if (c >= 0x20 && c <= 0x7E && le->len < LE_LINE_MAX - 1) {
        le->buf[le->len++] = (char)c;
        echo[(*echo_len)++] = (char)c;
    }
    return LE_MORE;
}
