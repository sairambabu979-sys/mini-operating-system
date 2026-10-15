/* SPDX-License-Identifier: MIT */
/*
 * Line editing for a program reading raw input bytes: the shell.
 *
 * Pure code (host-tested), no system calls. The kernel delivers raw
 * bytes and never echoes; le_feed() edits the line, says what to echo,
 * and reports when the line is done.
 *   - 0x20-0x7E is added (and echoed) while the line has room;
 *   - Backspace (0x7F or 0x08) erases one character, echoed "\b \b";
 *     Ctrl-U (0x15) erases the line;
 *   - CR, LF and CRLF each end exactly one line (serial terminals send
 *     CR, the keyboard LF); the line is echoed as ended with "\n";
 *   - Ctrl-C (0x03) cancels the line ("^C\n"); Ctrl-D (0x04) on an empty
 *     line is end of input, elsewhere ignored;
 *   - escape sequences (ESC [ ... or ESC O x, from a terminal's arrow or
 *     function keys, and the Linux console's ESC [ [ x) are swallowed
 *     whole; one that runs to 16 bytes without its final byte ends there;
 *   - Tab, other control bytes and bytes from 0x80 are ignored.
 * A line holds at most LE_LINE_MAX - 1 characters, so after a 4-column
 * prompt the cursor never passes column 35: the echo never wraps on an
 * 80-column screen, where "\b" could not cross back over the edge.
 */
#ifndef MINIOS_USER_LINEEDIT_H
#define MINIOS_USER_LINEEDIT_H

#include <types.h>

#define LE_LINE_MAX 32 /* bytes in buf: 31 characters and the NUL */
#define LE_ECHO_MAX 96 /* the longest echo of one byte: Ctrl-U on a full line */

enum le_event {
    LE_MORE,   /* keep feeding */
    LE_DONE,   /* buf holds the line, NUL-terminated, len characters */
    LE_CANCEL, /* Ctrl-C: the line was dropped */
    LE_EOF,    /* Ctrl-D on an empty line */
};

struct lineedit {
    char buf[LE_LINE_MAX];
    usize len;
    u8 esc;       /* 0, or 1 after ESC, or 2 inside an ESC [ / ESC O sequence */
    u8 esc_len;   /* bytes of the sequence so far */
    bool last_cr; /* the previous byte ended a line with CR: a following LF is its pair */
    bool ended;   /* the previous event ended a line: the next byte starts a new one */
};

void le_reset(struct lineedit *le);

/*
 * Feeds one input byte. Writes what to echo into `echo` (LE_ECHO_MAX
 * bytes), its length into *echo_len (possibly 0), and returns the event.
 * After LE_DONE the line stays in buf until the next call.
 */
enum le_event le_feed(struct lineedit *le, u8 c, char *echo, usize *echo_len);

#endif /* MINIOS_USER_LINEEDIT_H */
