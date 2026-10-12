/* SPDX-License-Identifier: MIT */
/*
 * Kernel console output.
 */
#ifndef MINIOS_CORE_PRINTK_H
#define MINIOS_CORE_PRINTK_H

#include <compiler.h>
#include <stdarg.h>

int kprintf(const char *fmt, ...) __printf(1, 2);
int kvprintf(const char *fmt, va_list ap);

#endif /* MINIOS_CORE_PRINTK_H */
