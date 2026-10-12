/* SPDX-License-Identifier: MIT */
/*
 * Kernel console output.
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

#endif /* MINIOS_CORE_PRINTK_H */
