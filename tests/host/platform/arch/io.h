/* SPDX-License-Identifier: MIT */
/*
 * Port I/O for kernel code compiled into the host tests: calls into the
 * test's device model (test_serial.c) instead of executing in/out. Found
 * before kernel/arch through -Itests/host/platform, for the drivers that
 * need it only (Makefile).
 */
#ifndef MINIOS_ARCH_IO_H
#define MINIOS_ARCH_IO_H

#include <types.h>

u8 host_inb(u16 port);
void host_outb(u16 port, u8 value);

static inline u8 inb(u16 port)
{
    return host_inb(port);
}

static inline void outb(u16 port, u8 value)
{
    host_outb(port, value);
}

#endif /* MINIOS_ARCH_IO_H */
