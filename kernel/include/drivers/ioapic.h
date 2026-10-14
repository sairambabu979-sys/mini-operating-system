/* SPDX-License-Identifier: MIT */
/*
 * I/O APIC (82093AA datasheet): routes global system interrupts (GSIs)
 * to local APIC vectors.
 */
#ifndef MINIOS_DRIVERS_IOAPIC_H
#define MINIOS_DRIVERS_IOAPIC_H

#include <acpi/tables.h>
#include <compiler.h>
#include <types.h>

#define IOAPIC_REDIR_MASKED     (1ull << 16)
#define IOAPIC_REDIR_LEVEL      (1ull << 15)
#define IOAPIC_REDIR_ACTIVE_LOW (1ull << 13)

/*
 * Maps every I/O APIC the MADT lists and masks all of their pins. An I/O
 * APIC that does not answer, or whose GSI range overlaps one already
 * accepted, is skipped with a warning. False if none is usable.
 */
bool ioapic_init(const struct acpi_madt_info *madt);

/*
 * Points `gsi` at `vector` on the CPU with APIC ID `dest`, fixed delivery,
 * physical destination. The pin is left masked. -ENODEV if no I/O APIC
 * serves `gsi`.
 */
__must_check int ioapic_route(u32 gsi, u8 vector, u8 dest, bool active_low, bool level);

void ioapic_set_masked(u32 gsi, bool masked);

/* The raw 64-bit redirection entry, for tests. False if `gsi` is not served. */
bool ioapic_read_entry(u32 gsi, u64 *entry);

/* GSIs served in total, across every I/O APIC. */
u32 ioapic_pin_count(void);

#endif /* MINIOS_DRIVERS_IOAPIC_H */
