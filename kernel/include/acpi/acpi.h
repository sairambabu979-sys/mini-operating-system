/* SPDX-License-Identifier: MIT */
/*
 * ACPI table discovery: finds the RSDP, walks the root table, and keeps
 * the parsed MADT and HPET tables for the interrupt and timer drivers.
 */
#ifndef MINIOS_ACPI_ACPI_H
#define MINIOS_ACPI_ACPI_H

#include <acpi/tables.h>
#include <boot/boot_info.h>

/*
 * Uses the loader's RSDP address, or scans the BIOS areas for one. Never
 * fails: without ACPI the kernel runs on the legacy 8259 PIC and the PIT.
 * Needs the memory manager (tables are mapped through iomap()).
 */
void acpi_init(const struct boot_info *bi);

/* NULL if ACPI, or that table, is absent or invalid. */
const struct acpi_rsdp_info *acpi_rsdp(void);
const struct acpi_madt_info *acpi_madt(void);
const struct acpi_hpet_info *acpi_hpet(void);

/* Maps and validates the first table with `signature`; NULL if there is none. */
const void *acpi_find_table(const char *signature, usize *len);

#endif /* MINIOS_ACPI_ACPI_H */
