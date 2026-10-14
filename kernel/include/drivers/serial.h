/* SPDX-License-Identifier: MIT */
/*
 * 16550-compatible UART on COM1: the primary kernel console.
 */
#ifndef MINIOS_DRIVERS_SERIAL_H
#define MINIOS_DRIVERS_SERIAL_H

#include <types.h>

/* Programs COM1 for 115200 8N1. Returns false if no UART responds. */
bool serial_init(void);

/* Blocking output. "\n" is translated to "\r\n". No-op without a UART. */
void serial_putc(char c);
void serial_write(const char *s, usize len);

#endif /* MINIOS_DRIVERS_SERIAL_H */
