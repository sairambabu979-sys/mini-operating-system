/* SPDX-License-Identifier: MIT */
/*
 * GDT selectors and TSS layout.
 *
 * The descriptor order is dictated by syscall/sysret (STAR MSR semantics):
 * SYSCALL loads CS = STAR[47:32], SS = CS + 8; 64-bit SYSRET loads
 * CS = STAR[63:48] + 16, SS = STAR[63:48] + 8. With STAR[63:48] = 0x18 that
 * forces user data at 0x20 and user code at 0x28.
 */
#ifndef MINIOS_ARCH_SEGMENTS_H
#define MINIOS_ARCH_SEGMENTS_H

#include <compiler.h>
#include <types.h>

#define GDT_NULL        0x00
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE32 0x18 /* placeholder required by the sysret layout */
#define GDT_USER_DATA   0x20
#define GDT_USER_CODE   0x28
#define GDT_TSS         0x30 /* 16-byte system descriptor: two slots */
#define GDT_ENTRIES     8

#define RPL_USER 3

/* Interrupt Stack Table slots (1-based, as encoded in the IDT gate). */
enum ist_slot {
    IST_NONE = 0,
    IST_DOUBLE_FAULT = 1,
    IST_NMI = 2,
    IST_MACHINE_CHECK = 3,
    IST_COUNT = 3,
};

#define IST_STACK_SIZE 16384

/* 64-bit Task State Segment (Intel SDM Vol. 3A, 8.7). */
struct tss {
    u32 reserved0;
    u64 rsp[3]; /* stack pointers loaded on privilege change to ring 0-2 */
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base; /* offset past the TSS limit: no I/O permission bitmap */
} __packed;

static_assert(sizeof(struct tss) == 104, "64-bit TSS is 104 bytes");

void gdt_init(void);

/* Kernel stack used on interrupts/syscalls arriving from ring 3. */
void tss_set_kernel_stack(u64 rsp0);

/* Top of an IST stack, for diagnostics ("am I on the #DF stack?"). */
uptr ist_stack_top(enum ist_slot slot);

#endif /* MINIOS_ARCH_SEGMENTS_H */
