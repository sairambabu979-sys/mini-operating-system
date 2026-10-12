/* SPDX-License-Identifier: MIT */
/*
 * x86 port I/O.
 *
 * Every accessor is a compiler barrier ("memory" clobber): device access
 * often orders against plain memory writes (e.g. DMA descriptors).
 */
#ifndef MINIOS_ARCH_IO_H
#define MINIOS_ARCH_IO_H

#include <types.h>

static inline void outb(u16 port, u8 value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline u8 inb(u16 port)
{
    u8 value;

    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline void outw(u16 port, u16 value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline u16 inw(u16 port)
{
    u16 value;

    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline void outl(u16 port, u32 value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline u32 inl(u16 port)
{
    u32 value;

    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

/* ~1 µs delay: port 0x80 is the POST diagnostic port, safe to write. */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

#endif /* MINIOS_ARCH_IO_H */
