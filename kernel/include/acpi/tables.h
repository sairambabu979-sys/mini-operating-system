/* SPDX-License-Identifier: MIT */
/*
 * ACPI table validation and parsing (ACPI 6.5, chapter 5).
 *
 * Pure code: no mapping, no allocation, no hardware. Every function takes
 * a buffer and its length and treats the contents as hostile, because
 * firmware tables are routinely wrong. The kernel side (acpi/acpi.c)
 * finds and maps the tables; this file only ever reads within the bounds
 * it is given, which the host tests check under ASan with fuzzed tables.
 */
#ifndef MINIOS_ACPI_TABLES_H
#define MINIOS_ACPI_TABLES_H

#include <compiler.h>
#include <types.h>

#define ACPI_SDT_HEADER_SIZE 36u
#define ACPI_RSDP_V1_SIZE    20u
#define ACPI_RSDP_V2_SIZE    36u

#define ACPI_ISA_IRQS       16u
#define ACPI_MAX_CPUS       64u
#define ACPI_MAX_IOAPICS    8u
#define ACPI_MAX_LAPIC_NMIS 8u
#define ACPI_GSI_NONE       0xFFFFFFFFu
#define ACPI_UID_ALL_CPUS   0xFFFFFFFFu

/* Common header of every system description table except the RSDP. */
struct acpi_sdt_header {
    char signature[4];
    u32 length; /* of the whole table, header included */
    u8 revision;
    u8 checksum; /* all `length` bytes sum to 0 (mod 256) */
    char oem_id[6];
    char oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
} __packed;

static_assert(sizeof(struct acpi_sdt_header) == ACPI_SDT_HEADER_SIZE, "ACPI 5.2.6");

/* What the RSDP says about where the root table is. */
struct acpi_rsdp_info {
    u8 revision; /* 0: ACPI 1.0 (RSDT only); 2+: XSDT available */
    u64 root_phys;
    bool root_is_xsdt; /* entries are 8 bytes instead of 4 */
    char oem_id[7];
};

/*
 * Validates a Root System Description Pointer: signature, the ACPI 1.0
 * checksum over 20 bytes and, from revision 2, the extended checksum over
 * `length` bytes. Prefers the XSDT when it is present. -EINVAL otherwise.
 */
__must_check int acpi_parse_rsdp(const void *p, usize avail, struct acpi_rsdp_info *out);

/*
 * Searches [area, area + len) on 16-byte boundaries for a valid RSDP, the
 * way the BIOS-era specification requires (EBDA, then 0xE0000-0xFFFFF).
 * Returns the offset of the first one, or -1.
 */
long acpi_find_rsdp(const void *area, usize len);

/*
 * True if `p` holds a complete, checksummed table: at least a header,
 * `length` within `avail`, and, if `signature` is not NULL, a match.
 */
bool acpi_table_valid(const void *p, usize avail, const char *signature);

/*
 * Root table (RSDT or XSDT, already validated) entries: the number of
 * table addresses, and the i-th address. Entries are unaligned in the
 * XSDT (8-byte addresses after a 36-byte header), so they are copied out.
 */
usize acpi_root_count(const void *root, bool xsdt);
u64 acpi_root_entry(const void *root, bool xsdt, usize i);

/* An ISA IRQ as the interrupt source overrides describe it. */
struct acpi_isa_irq {
    u32 gsi; /* ACPI_GSI_NONE: this ISA line is not wired (its GSI was taken) */
    bool active_low;
    bool level_triggered;
    bool overridden; /* an override entry named this IRQ */
};

struct acpi_ioapic {
    u8 id;
    u64 phys;
    u32 gsi_base;
};

struct acpi_lapic_nmi {
    u32 uid; /* processor UID, or ACPI_UID_ALL_CPUS */
    u8 lint; /* LINT0 or LINT1 */
    bool active_low;
    bool level_triggered;
};

/* The MADT, reduced to what an interrupt-controller driver needs. */
struct acpi_madt_info {
    u64 lapic_phys;                  /* after any 64-bit address override */
    bool pcat_compat;                /* a dual 8259 is present and must be masked */
    u32 ncpus;                       /* enabled processors, all of them */
    u32 cpu_apic_ids[ACPI_MAX_CPUS]; /* the first ACPI_MAX_CPUS of them */
    u32 cpu_uids[ACPI_MAX_CPUS];
    u32 nioapics;
    struct acpi_ioapic ioapics[ACPI_MAX_IOAPICS];
    struct acpi_isa_irq isa[ACPI_ISA_IRQS];
    u32 nlapic_nmis;
    struct acpi_lapic_nmi lapic_nmis[ACPI_MAX_LAPIC_NMIS];
    u32 ignored; /* well-formed entries dropped: duplicates, overflow, non-ISA buses */
};

/*
 * Parses a validated MADT. Every entry must have a length of at least 2
 * that stays inside the table, and at least the size its type requires;
 * otherwise the whole table is rejected with -EINVAL (a short entry would
 * desynchronize everything after it). Unknown entry types are skipped.
 *
 * ISA IRQs start out identity-mapped, edge-triggered, active high. An
 * override moves one; an ISA line whose identity GSI another override
 * claimed (typically IRQ 2, after IRQ 0 -> GSI 2) becomes ACPI_GSI_NONE.
 */
__must_check int acpi_parse_madt(const void *madt, usize len, struct acpi_madt_info *out);

struct acpi_hpet_info {
    u64 phys;
    u8 number;
    u16 min_tick;
};

/* Parses a validated HPET table; the block must be in system memory. */
__must_check int acpi_parse_hpet(const void *hpet, usize len, struct acpi_hpet_info *out);

#endif /* MINIOS_ACPI_TABLES_H */
