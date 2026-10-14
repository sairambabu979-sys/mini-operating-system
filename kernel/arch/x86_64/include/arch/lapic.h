/* SPDX-License-Identifier: MIT */
/*
 * Local APIC (Intel SDM Vol. 3A, chapter 11), in whichever mode the
 * firmware left it: xAPIC (MMIO) or x2APIC (MSRs).
 */
#ifndef MINIOS_ARCH_LAPIC_H
#define MINIOS_ARCH_LAPIC_H

#include <acpi/tables.h>
#include <compiler.h>
#include <types.h>

/* Register offsets in the 4 KiB MMIO page. */
enum {
    LAPIC_ID = 0x020,
    LAPIC_VERSION = 0x030,
    LAPIC_TPR = 0x080,
    LAPIC_EOI = 0x0B0,
    LAPIC_SVR = 0x0F0,
    LAPIC_ISR = 0x100, /* 8 registers, 0x10 apart */
    LAPIC_ESR = 0x280,
    LAPIC_LVT_CMCI = 0x2F0,
    LAPIC_ICR_LO = 0x300,
    LAPIC_ICR_HI = 0x310,
    LAPIC_LVT_TIMER = 0x320,
    LAPIC_LVT_THERMAL = 0x330,
    LAPIC_LVT_PERF = 0x340,
    LAPIC_LVT_LINT0 = 0x350,
    LAPIC_LVT_LINT1 = 0x360,
    LAPIC_LVT_ERROR = 0x370,
    LAPIC_TIMER_INIT = 0x380,
    LAPIC_TIMER_CURRENT = 0x390,
    LAPIC_TIMER_DIVIDE = 0x3E0,
};

#define LAPIC_SVR_ENABLE     (1u << 8)
#define LAPIC_LVT_MASKED     (1u << 16)
#define LAPIC_LVT_PERIODIC   (1u << 17)
#define LAPIC_LVT_NMI        (4u << 8)
#define LAPIC_LVT_ACTIVE_LOW (1u << 13)
#define LAPIC_TIMER_DIVIDER  16u

/*
 * Enables the local APIC of the boot CPU (mapping it in xAPIC mode, keeping
 * x2APIC mode if the firmware chose it), drains stale in-service bits, then programs the
 * spurious, error and LINT vectors (LINT0 masked: the 8259 is not used;
 * the MADT's NMI entries, or LINT1 by default, deliver NMIs). The timer
 * stays masked until lapic_timer_start_periodic(). Panics if the APIC
 * cannot be mapped: the caller checked it exists.
 */
void lapic_init(const struct acpi_madt_info *madt);

bool lapic_active(void);
bool lapic_x2apic_mode(void);
u32 lapic_read(u32 reg);
u32 lapic_id(void);

/* This CPU's APIC ID, readable before lapic_init(). */
u32 lapic_boot_id(void);
void lapic_eoi(void);
bool lapic_in_service(u8 vector);

/* Error Status Register, latched and cleared. */
u32 lapic_read_esr(void);

/* A masked one-shot count from `count` (divider 16), for calibration. */
void lapic_timer_start_oneshot(u32 count);
u32 lapic_timer_current(void);

/* Periodic interrupts on `vector` every `count` timer ticks (divider 16). */
void lapic_timer_start_periodic(u8 vector, u32 count);
void lapic_timer_set_masked(bool masked);

/* Sends a fixed interrupt to this CPU and waits until the APIC accepted it. */
void lapic_send_self_ipi(u8 vector);

/* Sends an NMI to this CPU (physical destination: the self shorthand excludes NMI). */
void lapic_send_self_nmi(void);

#endif /* MINIOS_ARCH_LAPIC_H */
