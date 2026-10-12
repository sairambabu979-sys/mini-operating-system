/* SPDX-License-Identifier: MIT */
/*
 * High Precision Event Timer (IA-PC HPET specification 1.0a), used only as
 * a free-running reference clock: its main counter calibrates the TSC and
 * the local APIC timer. No HPET comparator ever raises an interrupt.
 */
#ifndef MINIOS_DRIVERS_HPET_H
#define MINIOS_DRIVERS_HPET_H

#include <acpi/tables.h>
#include <types.h>

/*
 * Maps the timer block, checks its period, disables every comparator
 * interrupt and starts the main counter. False (with a warning) if the
 * block is unusable.
 */
bool hpet_init(const struct acpi_hpet_info *info);

bool hpet_available(void);

/* The main counter. Only bits in hpet_counter_mask() count (32 or 64). */
u64 hpet_counter(void);
u64 hpet_counter_mask(void);

/* Counter period in femtoseconds (at most 100 ns by specification). */
u64 hpet_period_fs(void);

#endif /* MINIOS_DRIVERS_HPET_H */
