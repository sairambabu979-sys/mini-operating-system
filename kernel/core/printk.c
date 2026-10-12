// SPDX-License-Identifier: MIT
#include <core/panic.h>
#include <core/printk.h>
#include <core/spinlock.h>
#include <drivers/serial.h>
#include <lib/printf.h>

/*
 * Serializes whole messages so lines from IRQ context never interleave
 * mid-line. Bypassed once a panic starts: the panicking context may be the
 * one holding it.
 */
static struct spinlock console_lock = SPINLOCK_INIT("console");

static void console_putc(char c, void *ctx)
{
    (void)ctx;
    serial_putc(c);
}

int kvprintf(const char *fmt, va_list ap)
{
    if (panic_in_progress())
        return k_vformat(console_putc, NULL, fmt, ap);

    irqflags_t flags = spin_lock_irqsave(&console_lock);
    int n = k_vformat(console_putc, NULL, fmt, ap);

    spin_unlock_irqrestore(&console_lock, flags);
    return n;
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}
