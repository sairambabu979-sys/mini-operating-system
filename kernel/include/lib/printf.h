/* SPDX-License-Identifier: MIT */
/*
 * Freestanding printf engine.
 *
 * Supported: flags "-+ #0", width and precision (including '*'), length
 * modifiers hh h l ll z t j, and conversions d i u o x X c s p %.
 * No floating point: the kernel is built with -mgeneral-regs-only.
 *
 * Semantics follow C17 7.21.6.1, except %p, which always prints
 * "0x" followed by 16 hex digits.
 */
#ifndef MINIOS_LIB_PRINTF_H
#define MINIOS_LIB_PRINTF_H

#include <compiler.h>
#include <stdarg.h>
#include <types.h>

/* Character sink. Called once per output character. */
typedef void (*fmt_putc_fn)(char c, void *ctx);

/* Formats into `out`. Returns the number of characters emitted. */
int k_vformat(fmt_putc_fn out, void *ctx, const char *fmt, va_list ap);

/*
 * Bounded formatting into a buffer. Returns the length the full output would
 * have had (C99 semantics), so truncation is detected by `ret >= size`.
 */
int k_vsnprintf(char *buf, usize size, const char *fmt, va_list ap);
int k_snprintf(char *buf, usize size, const char *fmt, ...) __printf(3, 4);

#endif /* MINIOS_LIB_PRINTF_H */
