// SPDX-License-Identifier: MIT
/*
 * ACPI table validation and parsing. See <acpi/tables.h>.
 *
 * Multi-byte fields are copied out with memcpy: tables are byte-packed and
 * nothing guarantees their alignment in memory.
 */
#include <acpi/tables.h>
#include <lib/errno.h>
#include <lib/string.h>

/* MADT layout (ACPI 6.5, 5.2.12). */
#define MADT_LAPIC_ADDR   36u
#define MADT_FLAGS        40u
#define MADT_ENTRIES      44u
#define MADT_PCAT_COMPAT  (1u << 0)
#define MADT_CPU_ENABLED  (1u << 0)
#define MADT_UID_ALL_8BIT 0xFFu

enum madt_type {
    MADT_LAPIC = 0,
    MADT_IOAPIC = 1,
    MADT_INT_OVERRIDE = 2,
    MADT_NMI_SOURCE = 3,
    MADT_LAPIC_NMI = 4,
    MADT_LAPIC_ADDR_OVERRIDE = 5,
    MADT_X2APIC = 9,
    MADT_X2APIC_NMI = 10,
};

/* Minimum length of each entry type the parser reads. */
static const u8 madt_min_len[] = {
    [MADT_LAPIC] = 8,      [MADT_IOAPIC] = 12,     [MADT_INT_OVERRIDE] = 10,
    [MADT_NMI_SOURCE] = 8, [MADT_LAPIC_NMI] = 6,   [MADT_LAPIC_ADDR_OVERRIDE] = 12,
    [MADT_X2APIC] = 16,    [MADT_X2APIC_NMI] = 12,
};

/* MPS INTI flags, shared by overrides and NMI entries. */
#define INTI_POLARITY(f) ((f) & 3u)
#define INTI_TRIGGER(f)  (((f) >> 2) & 3u)
enum { INTI_CONFORMS = 0, INTI_HIGH_OR_EDGE = 1, INTI_RESERVED = 2, INTI_LOW_OR_LEVEL = 3 };

/* HPET table layout (IA-PC HPET specification 1.0a, 3.2.4). */
#define HPET_GAS_SPACE    40u
#define HPET_GAS_ADDR     44u
#define HPET_NUMBER       52u
#define HPET_MIN_TICK     53u
#define HPET_TABLE_SIZE   56u
#define GAS_SYSTEM_MEMORY 0u

static u16 get16(const u8 *p)
{
    u16 v;

    memcpy(&v, p, sizeof(v));
    return v;
}

static u32 get32(const u8 *p)
{
    u32 v;

    memcpy(&v, p, sizeof(v));
    return v;
}

static u64 get64(const u8 *p)
{
    u64 v;

    memcpy(&v, p, sizeof(v));
    return v;
}

static u8 checksum(const u8 *p, usize len)
{
    u8 sum = 0;

    for (usize i = 0; i < len; i++)
        sum = (u8)(sum + p[i]);
    return sum;
}

int acpi_parse_rsdp(const void *p, usize avail, struct acpi_rsdp_info *out)
{
    const u8 *b = p;

    if (avail < ACPI_RSDP_V1_SIZE || memcmp(b, "RSD PTR ", 8) != 0 ||
        checksum(b, ACPI_RSDP_V1_SIZE) != 0)
        return -EINVAL;

    *out = (struct acpi_rsdp_info){.revision = b[15]};
    memcpy(out->oem_id, b + 9, 6);
    out->oem_id[6] = '\0';
    out->root_phys = get32(b + 16);

    if (out->revision >= 2) {
        if (avail < ACPI_RSDP_V2_SIZE)
            return -EINVAL;
        u32 length = get32(b + 20);

        if (length < ACPI_RSDP_V2_SIZE || length > avail || checksum(b, length) != 0)
            return -EINVAL;
        u64 xsdt = get64(b + 24);

        if (xsdt != 0) {
            out->root_phys = xsdt;
            out->root_is_xsdt = true;
        }
    }
    return out->root_phys != 0 ? 0 : -EINVAL;
}

long acpi_find_rsdp(const void *area, usize len)
{
    struct acpi_rsdp_info info;

    for (usize off = 0; off + ACPI_RSDP_V1_SIZE <= len; off += 16)
        if (acpi_parse_rsdp((const u8 *)area + off, len - off, &info) == 0)
            return (long)off;
    return -1;
}

bool acpi_table_valid(const void *p, usize avail, const char *signature)
{
    const u8 *b = p;

    if (avail < ACPI_SDT_HEADER_SIZE)
        return false;
    u32 length = get32(b + 4);

    if (length < ACPI_SDT_HEADER_SIZE || length > avail || checksum(b, length) != 0)
        return false;
    return signature == NULL || memcmp(b, signature, 4) == 0;
}

usize acpi_root_count(const void *root, bool xsdt)
{
    u32 length = get32((const u8 *)root + 4);

    return (length - ACPI_SDT_HEADER_SIZE) / (xsdt ? 8 : 4);
}

u64 acpi_root_entry(const void *root, bool xsdt, usize i)
{
    const u8 *entries = (const u8 *)root + ACPI_SDT_HEADER_SIZE;

    return xsdt ? get64(entries + i * 8) : get32(entries + i * 4);
}

/* Decodes INTI flags against the bus defaults; false for a reserved encoding. */
static bool decode_inti(u16 flags, bool *active_low, bool *level)
{
    if (INTI_POLARITY(flags) == INTI_RESERVED || INTI_TRIGGER(flags) == INTI_RESERVED)
        return false;
    *active_low = INTI_POLARITY(flags) == INTI_LOW_OR_LEVEL;
    *level = INTI_TRIGGER(flags) == INTI_LOW_OR_LEVEL;
    return true;
}

static void add_cpu(struct acpi_madt_info *m, u32 apic_id, u32 uid, u32 flags)
{
    if (!(flags & MADT_CPU_ENABLED))
        return;
    if (m->ncpus < ACPI_MAX_CPUS) {
        m->cpu_apic_ids[m->ncpus] = apic_id;
        m->cpu_uids[m->ncpus] = uid;
    }
    m->ncpus++;
}

static void add_override(struct acpi_madt_info *m, const u8 *e)
{
    u8 bus = e[2], source = e[3];
    u32 gsi = get32(e + 4);
    struct acpi_isa_irq irq = {.gsi = gsi, .overridden = true};

    if (bus != 0 || source >= ACPI_ISA_IRQS || gsi == ACPI_GSI_NONE ||
        !decode_inti(get16(e + 8), &irq.active_low, &irq.level_triggered) ||
        m->isa[source].overridden) {
        m->ignored++;
        return;
    }
    for (u32 i = 0; i < ACPI_ISA_IRQS; i++) {
        if (m->isa[i].overridden && m->isa[i].gsi == gsi) {
            m->ignored++; /* two sources on one GSI: keep the first */
            return;
        }
    }
    m->isa[source] = irq;
}

static void add_lapic_nmi(struct acpi_madt_info *m, u32 uid, u16 flags, u8 lint)
{
    struct acpi_lapic_nmi nmi = {.uid = uid, .lint = lint};

    if (lint > 1 || m->nlapic_nmis >= ACPI_MAX_LAPIC_NMIS ||
        !decode_inti(flags, &nmi.active_low, &nmi.level_triggered)) {
        m->ignored++;
        return;
    }
    m->lapic_nmis[m->nlapic_nmis++] = nmi;
}

int acpi_parse_madt(const void *madt, usize len, struct acpi_madt_info *out)
{
    const u8 *b = madt;

    if (len < MADT_ENTRIES)
        return -EINVAL;

    *out = (struct acpi_madt_info){
        .lapic_phys = get32(b + MADT_LAPIC_ADDR),
        .pcat_compat = get32(b + MADT_FLAGS) & MADT_PCAT_COMPAT,
    };
    for (u32 i = 0; i < ACPI_ISA_IRQS; i++)
        out->isa[i] = (struct acpi_isa_irq){.gsi = i};

    for (usize off = MADT_ENTRIES; off < len;) {
        if (len - off < 2)
            return -EINVAL;
        const u8 *e = b + off;
        u8 type = e[0], elen = e[1];

        if (elen < 2 || elen > len - off ||
            (type < ARRAY_SIZE(madt_min_len) && elen < madt_min_len[type]))
            return -EINVAL;

        switch (type) {
        case MADT_LAPIC:
            add_cpu(out, e[3], e[2] == MADT_UID_ALL_8BIT ? ACPI_UID_ALL_CPUS : e[2], get32(e + 4));
            break;
        case MADT_X2APIC:
            add_cpu(out, get32(e + 4), get32(e + 12), get32(e + 8));
            break;
        case MADT_IOAPIC:
            if (out->nioapics < ACPI_MAX_IOAPICS)
                out->ioapics[out->nioapics++] = (struct acpi_ioapic){
                    .id = e[2], .phys = get32(e + 4), .gsi_base = get32(e + 8)};
            else
                out->ignored++;
            break;
        case MADT_INT_OVERRIDE:
            add_override(out, e);
            break;
        case MADT_LAPIC_NMI:
            add_lapic_nmi(out, e[2] == MADT_UID_ALL_8BIT ? ACPI_UID_ALL_CPUS : e[2], get16(e + 3),
                          e[5]);
            break;
        case MADT_X2APIC_NMI:
            add_lapic_nmi(out, get32(e + 4), get16(e + 2), e[8]);
            break;
        case MADT_LAPIC_ADDR_OVERRIDE:
            out->lapic_phys = get64(e + 4);
            break;
        default:
            break; /* NMI sources, GIC entries, future types: not ours */
        }
        off += elen;
    }

    /* An ISA line whose identity GSI was given to another source is not wired. */
    for (u32 i = 0; i < ACPI_ISA_IRQS; i++) {
        if (out->isa[i].overridden)
            continue;
        for (u32 j = 0; j < ACPI_ISA_IRQS; j++)
            if (out->isa[j].overridden && out->isa[j].gsi == i)
                out->isa[i].gsi = ACPI_GSI_NONE;
    }
    return 0;
}

int acpi_parse_hpet(const void *hpet, usize len, struct acpi_hpet_info *out)
{
    const u8 *b = hpet;

    if (len < HPET_TABLE_SIZE || b[HPET_GAS_SPACE] != GAS_SYSTEM_MEMORY)
        return -EINVAL;
    *out = (struct acpi_hpet_info){
        .phys = get64(b + HPET_GAS_ADDR),
        .number = b[HPET_NUMBER],
        .min_tick = get16(b + HPET_MIN_TICK),
    };
    return out->phys != 0 && IS_ALIGNED(out->phys, 8) ? 0 : -EINVAL;
}
