// SPDX-License-Identifier: MIT
#include <core/printk.h>
#include <drivers/serial.h>
#include <lib/printf.h>

static void console_putc(char c, void *ctx)
{
    (void)ctx;
    serial_putc(c);
}

int kvprintf(const char *fmt, va_list ap)
{
    return k_vformat(console_putc, NULL, fmt, ap);
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
