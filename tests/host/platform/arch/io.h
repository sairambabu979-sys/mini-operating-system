/* SPDX-License-Identifier: MIT */
#ifndef MINIOS_HOST_IO_H
#define MINIOS_HOST_IO_H
#include <types.h>
u8 test_uart_inb(u16 port);
void test_uart_outb(u16 port, u8 value);
#define inb test_uart_inb
#define outb test_uart_outb
#endif
