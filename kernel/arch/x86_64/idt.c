// SPDX-License-Identifier: MIT
/*
 * Interrupt Descriptor Table.
 *
 * All 256 vectors use interrupt gates (IF cleared on entry), so handlers
 * start with interrupts off and decide for themselves when to re-enable.
 * Exceptions that must survive a corrupt kernel stack (#DF, NMI, #MC) run
 * on dedicated IST stacks.
 */
#include <arch/segments.h>
#include <arch/trap.h>

#define GATE_INTERRUPT 0xE
#define GATE_PRESENT   (1u << 7)
#define GATE_DPL(n)    ((u32)(n) << 5)

struct idt_gate {
    u16 offset_lo;
    u16 selector;
    u8 ist;
    u8 type_attr;
    u16 offset_mid;
    u32 offset_hi;
    u32 reserved;
} __packed;

static_assert(sizeof(struct idt_gate) == 16, "long-mode IDT gates are 16 bytes");

struct idt_ptr {
    u16 limit;
    u64 base;
} __packed;

extern const uptr isr_stub_table[VEC_COUNT];

static struct idt_gate idt[VEC_COUNT] __aligned(16);

static void set_gate(unsigned vector, uptr handler, enum ist_slot ist, unsigned dpl)
{
    idt[vector] = (struct idt_gate){
        .offset_lo = (u16)handler,
        .selector = GDT_KERNEL_CODE,
        .ist = (u8)ist,
        .type_attr = (u8)(GATE_PRESENT | GATE_DPL(dpl) | GATE_INTERRUPT),
        .offset_mid = (u16)(handler >> 16),
        .offset_hi = (u32)(handler >> 32),
        .reserved = 0,
    };
}

static enum ist_slot ist_for_vector(unsigned vector)
{
    switch (vector) {
    case VEC_DOUBLE_FAULT:
        return IST_DOUBLE_FAULT;
    case VEC_NMI:
        return IST_NMI;
    case VEC_MACHINE_CHECK:
        return IST_MACHINE_CHECK;
    default:
        return IST_NONE;
    }
}

void idt_init(void)
{
    for (unsigned v = 0; v < VEC_COUNT; v++)
        set_gate(v, isr_stub_table[v], ist_for_vector(v), 0);

    struct idt_ptr idtr = {.limit = sizeof(idt) - 1, .base = (u64)(uptr)idt};

    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
}
