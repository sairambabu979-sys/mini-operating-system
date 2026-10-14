/* SPDX-License-Identifier: MIT */
/*
 * Kernel console output: COM1, a log ring, and a screen once one exists.
 */
#ifndef MINIOS_CORE_PRINTK_H
#define MINIOS_CORE_PRINTK_H

#include <compiler.h>
#include <stdarg.h>
#include <types.h>

int kprintf(const char *fmt, ...) __printf(1, 2);
int kvprintf(const char *fmt, va_list ap);

/* Writes raw bytes as one message (used by the write syscall). */
void console_write(const char *s, usize len);

/*
 * A screen console: framebuffer or VGA text. `putc` takes one byte of the
 * log and should only update memory; `flush` (optional) then draws. Both
 * run in one context at a time, with interrupts on once the system is
 * live, and from panic() with nothing guaranteed: they must not allocate,
 * sleep or take locks.
 */
struct console_screen {
    const char *name;
    void (*putc)(char c);
    void (*flush)(void);
};

/* Replays the log ring onto `s`, then sends it all further output. */
void console_register_screen(const struct console_screen *s);
const struct console_screen *console_screen(void);

/*
 * Called once interrupts run (kmain). From then on, output produced with
 * interrupts off is drawn later: by the next printer that has them on, or
 * by console_drain(), which the idle loop calls.
 */
void console_go_live(void);
bool console_is_live(void);
void console_drain(void);

/* Starts the thread that drains every 50 ms (if there is a screen). Needs the scheduler. */
void console_start_thread(void);

/* Copies the newest min(len, logged) bytes of console output into buf. */
usize console_log_read(char *buf, usize len);

#endif /* MINIOS_CORE_PRINTK_H */
