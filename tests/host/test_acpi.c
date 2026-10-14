// SPDX-License-Identifier: MIT
/*
 * ACPI table parsing against hand-built tables, firmware-style mistakes
 * and random corruption (run under ASan: every read must stay in bounds).
 */
#include "test.h"

#include <acpi/tables.h>
#include <lib/errno.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MADT_FUZZ_START 36u /* the fuzzer leaves the header alone: checksums are not the point */

/* A table under construction: header, then appended bytes. */
struct builder {
    uint8_t buf[512];
    size_t len;
};

static void put(struct builder *b, const void *p, size_t n)
{
    memcpy(b->buf + b->len, p, n);
    b->len += n;
}

static void put8(struct builder *b, uint8_t v)
{
    put(b, &v, 1);
}

static void put16(struct builder *b, uint16_t v)
{
    put(b, &v, 2);
}

static void put32(struct builder *b, uint32_t v)
{
    put(b, &v, 4);
}

static void put64(struct builder *b, uint64_t v)
{
    put(b, &v, 8);
}

static void begin(struct builder *b, const char *sig)
{
    memset(b, 0, sizeof(*b));
    memcpy(b->buf, sig, 4);
    memcpy(b->buf + 10, "MINIOS", 6);
    b->len = ACPI_SDT_HEADER_SIZE;
}

/* Fills in the length and fixes the checksum. */
static void finish(struct builder *b)
{
    uint32_t len = (uint32_t)b->len;
    uint8_t sum = 0;

    memcpy(b->buf + 4, &len, 4);
    b->buf[9] = 0;
    for (size_t i = 0; i < b->len; i++)
        sum = (uint8_t)(sum + b->buf[i]);
    b->buf[9] = (uint8_t)-sum;
}

static void madt_begin(struct builder *b, uint32_t lapic, uint32_t flags)
{
    begin(b, "APIC");
    put32(b, lapic);
    put32(b, flags);
}

static void madt_lapic(struct builder *b, uint8_t uid, uint8_t apic_id, uint32_t flags)
{
    put8(b, 0);
    put8(b, 8);
    put8(b, uid);
    put8(b, apic_id);
    put32(b, flags);
}

static void madt_ioapic(struct builder *b, uint8_t id, uint32_t addr, uint32_t gsi_base)
{
    put8(b, 1);
    put8(b, 12);
    put8(b, id);
    put8(b, 0);
    put32(b, addr);
    put32(b, gsi_base);
}

static void madt_override(struct builder *b, uint8_t source, uint32_t gsi, uint16_t flags)
{
    put8(b, 2);
    put8(b, 10);
    put8(b, 0); /* ISA */
    put8(b, source);
    put32(b, gsi);
    put16(b, flags);
}

static void madt_lapic_nmi(struct builder *b, uint8_t uid, uint16_t flags, uint8_t lint)
{
    put8(b, 4);
    put8(b, 6);
    put8(b, uid);
    put16(b, flags);
    put8(b, lint);
}

/* What QEMU's i440FX machine provides: one CPU, one IOAPIC, the usual overrides. */
static void qemu_madt(struct builder *b)
{
    madt_begin(b, 0xFEE00000, 1);
    madt_lapic(b, 0, 0, 1);
    madt_ioapic(b, 0, 0xFEC00000, 0);
    madt_override(b, 0, 2, 0);   /* timer: IRQ 0 -> GSI 2 */
    madt_override(b, 5, 5, 0xD); /* level, active high */
    madt_override(b, 9, 9, 0xD); /* SCI */
    madt_override(b, 10, 10, 0xD);
    madt_override(b, 11, 11, 0xD);
    madt_lapic_nmi(b, 0xFF, 0, 1); /* all CPUs: LINT1 is NMI */
    finish(b);
}

static void make_rsdp(uint8_t out[36], uint8_t revision, uint32_t rsdt, uint64_t xsdt)
{
    memset(out, 0, 36);
    memcpy(out, "RSD PTR ", 8);
    memcpy(out + 9, "MINIOS", 6);
    out[15] = revision;
    memcpy(out + 16, &rsdt, 4);
    uint32_t len = 36;

    memcpy(out + 20, &len, 4);
    memcpy(out + 24, &xsdt, 8);
    uint8_t sum = 0;

    for (int i = 0; i < 20; i++)
        sum = (uint8_t)(sum + out[i]);
    out[8] = (uint8_t)-sum;
    sum = 0;
    for (int i = 0; i < 36; i++)
        sum = (uint8_t)(sum + out[i]);
    out[32] = (uint8_t)-sum;
}

TEST(acpi_rsdp_v1_uses_the_rsdt)
{
    uint8_t r[36];
    struct acpi_rsdp_info info;

    make_rsdp(r, 0, 0x7FE0000, 0);
    EXPECT_EQ(acpi_parse_rsdp(r, 20, &info), 0); /* v1 is only 20 bytes long */
    EXPECT_EQ(info.revision, 0);
    EXPECT_EQ(info.root_phys, 0x7FE0000);
    EXPECT_TRUE(!info.root_is_xsdt);
    EXPECT_STREQ(info.oem_id, "MINIOS");
}

TEST(acpi_rsdp_v2_prefers_the_xsdt)
{
    uint8_t r[36];
    struct acpi_rsdp_info info;

    make_rsdp(r, 2, 0x7FE0000, 0x17FE0000ull);
    EXPECT_EQ(acpi_parse_rsdp(r, sizeof(r), &info), 0);
    EXPECT_EQ(info.root_phys, 0x17FE0000ull);
    EXPECT_TRUE(info.root_is_xsdt);

    make_rsdp(r, 2, 0x7FE0000, 0); /* v2 without an XSDT: fall back */
    EXPECT_EQ(acpi_parse_rsdp(r, sizeof(r), &info), 0);
    EXPECT_EQ(info.root_phys, 0x7FE0000);
    EXPECT_TRUE(!info.root_is_xsdt);
}

TEST(acpi_rsdp_rejects_bad_checksums_and_truncation)
{
    uint8_t r[36];
    struct acpi_rsdp_info info;

    make_rsdp(r, 2, 0x1000, 0x2000);
    EXPECT_EQ(acpi_parse_rsdp(r, 35, &info), -EINVAL); /* extended part cut off */
    EXPECT_EQ(acpi_parse_rsdp(r, 19, &info), -EINVAL);

    r[16] ^= 1; /* breaks both checksums */
    EXPECT_EQ(acpi_parse_rsdp(r, sizeof(r), &info), -EINVAL);

    make_rsdp(r, 2, 0x1000, 0x2000);
    r[30] ^= 1; /* only the extended checksum covers the XSDT address */
    EXPECT_EQ(acpi_parse_rsdp(r, sizeof(r), &info), -EINVAL);

    make_rsdp(r, 2, 0x1000, 0x2000);
    uint32_t huge = 0x10000;

    memcpy(r + 20, &huge, 4); /* length beyond the buffer */
    EXPECT_EQ(acpi_parse_rsdp(r, sizeof(r), &info), -EINVAL);

    make_rsdp(r, 0, 0, 0); /* nowhere to go */
    EXPECT_EQ(acpi_parse_rsdp(r, sizeof(r), &info), -EINVAL);
}

TEST(acpi_rsdp_scan_finds_only_aligned_valid_pointers)
{
    static uint8_t area[4096];
    uint8_t r[36];

    memset(area, 0, sizeof(area));
    make_rsdp(r, 0, 0x1000, 0);
    memcpy(area + 0x108, r, 20); /* misaligned: must not be found */
    EXPECT_EQ(acpi_find_rsdp(area, sizeof(area)), -1);

    memcpy(area + 0x200, "RSD PTR ", 8); /* signature, bad checksum */
    memcpy(area + 0x7F0, r, 20);
    EXPECT_EQ(acpi_find_rsdp(area, sizeof(area)), 0x7F0);

    memcpy(area + 0xFF0, r, 16); /* straddles the end */
    EXPECT_EQ(acpi_find_rsdp(area + 0x800, 0x800), -1);
}

TEST(acpi_table_checksum_length_and_signature)
{
    struct builder b;

    begin(&b, "XSDT");
    put64(&b, 0x1000);
    put64(&b, 0x2000);
    finish(&b);
    EXPECT_TRUE(acpi_table_valid(b.buf, b.len, "XSDT"));
    EXPECT_TRUE(acpi_table_valid(b.buf, b.len, NULL));
    EXPECT_TRUE(!acpi_table_valid(b.buf, b.len, "RSDT"));
    EXPECT_TRUE(!acpi_table_valid(b.buf, b.len - 1, "XSDT")); /* length beyond avail */
    EXPECT_TRUE(!acpi_table_valid(b.buf, 35, NULL));

    b.buf[40] ^= 0x80;
    EXPECT_TRUE(!acpi_table_valid(b.buf, b.len, "XSDT"));
    b.buf[40] ^= 0x80;

    uint32_t short_len = 20; /* shorter than its own header */

    memcpy(b.buf + 4, &short_len, 4);
    EXPECT_TRUE(!acpi_table_valid(b.buf, b.len, NULL));
}

TEST(acpi_root_entries_are_read_unaligned)
{
    struct builder b;

    begin(&b, "XSDT");
    put64(&b, 0x123456789ull);
    put64(&b, 0xFEDCBA987ull);
    finish(&b);
    EXPECT_EQ(acpi_root_count(b.buf, true), 2);
    EXPECT_EQ(acpi_root_entry(b.buf, true, 0), 0x123456789ull);
    EXPECT_EQ(acpi_root_entry(b.buf, true, 1), 0xFEDCBA987ull);

    begin(&b, "RSDT");
    put32(&b, 0x7FE1000);
    put32(&b, 0x7FE2000);
    put32(&b, 0x7FE3000);
    finish(&b);
    EXPECT_EQ(acpi_root_count(b.buf, false), 3);
    EXPECT_EQ(acpi_root_entry(b.buf, false, 2), 0x7FE3000);
}

TEST(acpi_madt_qemu_layout)
{
    struct builder b;
    struct acpi_madt_info m;

    qemu_madt(&b);
    EXPECT_TRUE(acpi_table_valid(b.buf, b.len, "APIC"));
    EXPECT_EQ(acpi_parse_madt(b.buf, b.len, &m), 0);
    EXPECT_EQ(m.lapic_phys, 0xFEE00000);
    EXPECT_TRUE(m.pcat_compat);
    EXPECT_EQ(m.ncpus, 1);
    EXPECT_EQ(m.cpu_apic_ids[0], 0);
    EXPECT_EQ(m.nioapics, 1);
    EXPECT_EQ(m.ioapics[0].phys, 0xFEC00000);
    EXPECT_EQ(m.ioapics[0].gsi_base, 0);

    EXPECT_EQ(m.isa[0].gsi, 2); /* the timer override */
    EXPECT_TRUE(!m.isa[0].level_triggered && !m.isa[0].active_low);
    EXPECT_EQ(m.isa[2].gsi, ACPI_GSI_NONE); /* GSI 2 belongs to IRQ 0 now */
    EXPECT_EQ(m.isa[1].gsi, 1);             /* untouched: identity, edge, high */
    EXPECT_TRUE(!m.isa[1].overridden && !m.isa[1].level_triggered);
    EXPECT_EQ(m.isa[9].gsi, 9);
    EXPECT_TRUE(m.isa[9].level_triggered && !m.isa[9].active_low);

    EXPECT_EQ(m.nlapic_nmis, 1);
    EXPECT_EQ(m.lapic_nmis[0].uid, ACPI_UID_ALL_CPUS);
    EXPECT_EQ(m.lapic_nmis[0].lint, 1);
    EXPECT_EQ(m.ignored, 0);
}

TEST(acpi_madt_cpus_x2apic_and_address_override)
{
    struct builder b;
    struct acpi_madt_info m;

    madt_begin(&b, 0xFEE00000, 0);
    madt_lapic(&b, 0, 0, 1);
    madt_lapic(&b, 1, 1, 0); /* disabled: not counted */
    madt_lapic(&b, 2, 4, 1);
    put8(&b, 9); /* x2APIC entry, id 300 */
    put8(&b, 16);
    put16(&b, 0);
    put32(&b, 300);
    put32(&b, 1);
    put32(&b, 7);
    put8(&b, 5); /* 64-bit LAPIC address */
    put8(&b, 12);
    put16(&b, 0);
    put64(&b, 0x1FEE00000ull);
    put8(&b, 0x7F); /* unknown type: skipped by its length */
    put8(&b, 3);
    put8(&b, 0xAA);
    finish(&b);

    EXPECT_EQ(acpi_parse_madt(b.buf, b.len, &m), 0);
    EXPECT_TRUE(!m.pcat_compat);
    EXPECT_EQ(m.ncpus, 3);
    EXPECT_EQ(m.cpu_apic_ids[1], 4);
    EXPECT_EQ(m.cpu_apic_ids[2], 300);
    EXPECT_EQ(m.cpu_uids[2], 7);
    EXPECT_EQ(m.lapic_phys, 0x1FEE00000ull);
}

TEST(acpi_madt_override_conflicts_and_bad_flags)
{
    struct builder b;
    struct acpi_madt_info m;

    madt_begin(&b, 0xFEE00000, 1);
    madt_override(&b, 0, 2, 0);
    madt_override(&b, 0, 20, 0);  /* duplicate source: ignored */
    madt_override(&b, 4, 2, 0);   /* GSI 2 already taken: ignored */
    madt_override(&b, 7, 7, 0x2); /* reserved polarity: ignored */
    madt_override(&b, 16, 16, 0); /* not an ISA IRQ: ignored */
    madt_override(&b, 8, 8, 0xF); /* RTC: level, active low */
    madt_lapic_nmi(&b, 0, 0, 2);  /* no LINT2: ignored */
    finish(&b);

    EXPECT_EQ(acpi_parse_madt(b.buf, b.len, &m), 0);
    EXPECT_EQ(m.ignored, 5);
    EXPECT_EQ(m.isa[0].gsi, 2);
    EXPECT_EQ(m.isa[4].gsi, 4);
    EXPECT_EQ(m.isa[7].gsi, 7);
    EXPECT_TRUE(!m.isa[7].active_low);
    EXPECT_TRUE(m.isa[8].active_low && m.isa[8].level_triggered);
    EXPECT_EQ(m.nlapic_nmis, 0);
}

TEST(acpi_madt_rejects_malformed_entries)
{
    struct builder b;
    struct acpi_madt_info m;

    madt_begin(&b, 0xFEE00000, 1);
    put8(&b, 0x7F); /* zero-length entry: would loop forever */
    put8(&b, 0);
    finish(&b);
    EXPECT_EQ(acpi_parse_madt(b.buf, b.len, &m), -EINVAL);

    madt_begin(&b, 0xFEE00000, 1);
    madt_lapic(&b, 0, 0, 1);
    b.buf[b.len - 7] = 20; /* claims to run past the end of the table */
    finish(&b);
    EXPECT_EQ(acpi_parse_madt(b.buf, b.len, &m), -EINVAL);

    madt_begin(&b, 0xFEE00000, 1);
    put8(&b, 1); /* IOAPIC entry shorter than its type requires */
    put8(&b, 8);
    put32(&b, 0);
    put16(&b, 0);
    finish(&b);
    EXPECT_EQ(acpi_parse_madt(b.buf, b.len, &m), -EINVAL);

    madt_begin(&b, 0xFEE00000, 1);
    put8(&b, 0); /* a lone type byte at the very end */
    finish(&b);
    EXPECT_EQ(acpi_parse_madt(b.buf, b.len, &m), -EINVAL);

    EXPECT_EQ(acpi_parse_madt(b.buf, 43, &m), -EINVAL); /* no room for the fixed fields */
}

TEST(acpi_madt_overflowing_lists_are_counted_not_overrun)
{
    struct builder b;
    struct acpi_madt_info m;

    madt_begin(&b, 0xFEE00000, 1);
    for (int i = 0; i < 10; i++)
        madt_ioapic(&b, (uint8_t)i, 0xFEC00000u + (uint32_t)i * 0x1000, (uint32_t)i * 24);
    for (int i = 0; i < 12; i++)
        madt_lapic_nmi(&b, (uint8_t)i, 0, 1);
    finish(&b);

    EXPECT_EQ(acpi_parse_madt(b.buf, b.len, &m), 0);
    EXPECT_EQ(m.nioapics, ACPI_MAX_IOAPICS);
    EXPECT_EQ(m.nlapic_nmis, ACPI_MAX_LAPIC_NMIS);
    EXPECT_EQ(m.ignored, (10 - ACPI_MAX_IOAPICS) + (12 - ACPI_MAX_LAPIC_NMIS));
}

TEST(acpi_hpet_table)
{
    struct builder b;
    struct acpi_hpet_info h;

    begin(&b, "HPET");
    put32(&b, 0x8086A201); /* event timer block id */
    put8(&b, 0);           /* GAS: system memory */
    put8(&b, 64);
    put8(&b, 0);
    put8(&b, 0);
    put64(&b, 0xFED00000);
    put8(&b, 0);
    put16(&b, 128);
    put8(&b, 0);
    finish(&b);
    EXPECT_EQ(acpi_parse_hpet(b.buf, b.len, &h), 0);
    EXPECT_EQ(h.phys, 0xFED00000);
    EXPECT_EQ(h.min_tick, 128);

    EXPECT_EQ(acpi_parse_hpet(b.buf, b.len - 1, &h), -EINVAL);
    b.buf[40] = 1; /* I/O space: not a memory-mapped block */
    EXPECT_EQ(acpi_parse_hpet(b.buf, b.len, &h), -EINVAL);
}

/*
 * Random corruption of a valid MADT, parsed at every length. The checks
 * are ASan's (no read outside the buffer) plus the invariants below.
 */
TEST(acpi_madt_fuzz)
{
    struct builder b;
    struct acpi_madt_info m;

    srand(0x41435049);
    for (int iter = 0; iter < 20000; iter++) {
        qemu_madt(&b);
        int flips = 1 + rand() % 6;

        for (int i = 0; i < flips; i++)
            b.buf[MADT_FUZZ_START + (size_t)rand() % (b.len - MADT_FUZZ_START)] = (uint8_t)rand();

        size_t len = (size_t)rand() % (b.len + 1);
        uint8_t *copy = malloc(len ? len : 1); /* exact size, so ASan sees overreads */

        memcpy(copy, b.buf, len);
        if (acpi_parse_madt(copy, len, &m) == 0) {
            EXPECT_TRUE(m.nioapics <= ACPI_MAX_IOAPICS);
            EXPECT_TRUE(m.nlapic_nmis <= ACPI_MAX_LAPIC_NMIS);
            for (unsigned i = 0; i < ACPI_ISA_IRQS; i++)
                for (unsigned j = i + 1; j < ACPI_ISA_IRQS; j++)
                    if (m.isa[i].gsi != ACPI_GSI_NONE)
                        EXPECT_TRUE(m.isa[i].gsi != m.isa[j].gsi); /* no GSI shared */
        }
        free(copy);
    }
}
