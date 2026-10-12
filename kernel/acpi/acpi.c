// SPDX-License-Identifier: MIT
/*
 * ACPI table discovery. See <acpi/acpi.h>.
 *
 * Tables are mapped read-only and cached through iomap(): the direct map
 * covers ACPI memory, but firmware is free to put tables in "reserved"
 * memory it does not cover, and one path for both is simpler.
 */
#define pr_fmt(fmt) "acpi: " fmt

#include <acpi/acpi.h>
#include <arch/paging.h>
#include <core/klog.h>
#include <lib/string.h>
#include <mm/vmalloc.h>

#define MAX_TABLES     32u
#define MAX_TABLE_SIZE (1u << 20) /* larger than any table this kernel reads */
#define BDA_EBDA_SEG   0x40Eu     /* BIOS data area: EBDA segment */
#define EBDA_SCAN_SIZE 1024u
#define BIOS_ROM_START 0xE0000u
#define BIOS_ROM_SIZE  0x20000u

struct table_ref {
    char signature[5];
    u64 phys;
    const void *mapped; /* once mapped and validated */
    usize len;
};

static struct acpi_rsdp_info rsdp;
static bool have_rsdp;
static struct table_ref tables[MAX_TABLES];
static u32 ntables;
static struct acpi_madt_info madt;
static bool have_madt;
static struct acpi_hpet_info hpet;
static bool have_hpet;

/* Maps a whole table: the header first, to learn its length. */
static const void *map_table(u64 phys, usize *len)
{
    const struct acpi_sdt_header *h = iomap(phys, ACPI_SDT_HEADER_SIZE, 0);

    if (!h)
        return NULL;
    u32 length = h->length;

    if (length < ACPI_SDT_HEADER_SIZE || length > MAX_TABLE_SIZE)
        return NULL;
    /* The header mapping already reaches the end of its last page. */
    usize mapped = ALIGN_UP(phys + ACPI_SDT_HEADER_SIZE, PAGE_SIZE_4K) - phys;
    const void *t = length <= mapped ? (const void *)h : iomap(phys, length, 0);

    if (!t || !acpi_table_valid(t, length, NULL))
        return NULL;
    *len = length;
    return t;
}

static bool scan_bios_areas(u64 *found)
{
    const u16 *bda = iomap(BDA_EBDA_SEG, sizeof(u16), 0);
    u64 ebda = bda ? (u64)*bda << 4 : 0;
    const struct {
        u64 base;
        usize size;
    } areas[] = {{ebda, EBDA_SCAN_SIZE}, {BIOS_ROM_START, BIOS_ROM_SIZE}};

    for (usize i = 0; i < ARRAY_SIZE(areas); i++) {
        if (i == 0 && (ebda < 0x80000 || ebda >= 0xA0000))
            continue; /* no plausible EBDA */
        const void *p = iomap(areas[i].base, areas[i].size, 0);
        long off = p ? acpi_find_rsdp(p, areas[i].size) : -1;

        if (off >= 0) {
            *found = areas[i].base + (u64)off;
            return true;
        }
    }
    return false;
}

static bool load_rsdp(const struct boot_info *bi)
{
    u64 phys = bi->acpi_rsdp;

    if (!phys && !scan_bios_areas(&phys)) {
        pr_warn("no RSDP found\n");
        return false;
    }
    const void *p = iomap(phys, ACPI_RSDP_V2_SIZE, 0);

    if (!p || acpi_parse_rsdp(p, ACPI_RSDP_V2_SIZE, &rsdp) != 0) {
        pr_warn("invalid RSDP at %#llx\n", (unsigned long long)phys);
        return false;
    }
    pr_info("RSDP at %#llx: revision %u, OEM \"%s\", %s at %#llx\n", (unsigned long long)phys,
            rsdp.revision, rsdp.oem_id, rsdp.root_is_xsdt ? "XSDT" : "RSDT",
            (unsigned long long)rsdp.root_phys);
    return true;
}

static void load_root(void)
{
    usize len;
    const void *root = map_table(rsdp.root_phys, &len);

    if (!root || !acpi_table_valid(root, len, rsdp.root_is_xsdt ? "XSDT" : "RSDT")) {
        pr_warn("invalid root table at %#llx\n", (unsigned long long)rsdp.root_phys);
        return;
    }
    usize count = acpi_root_count(root, rsdp.root_is_xsdt);
    char list[MAX_TABLES * 5 + 1] = "";
    char *p = list;

    for (usize i = 0; i < count && ntables < MAX_TABLES; i++) {
        struct table_ref *t = &tables[ntables];
        usize tlen;

        t->phys = acpi_root_entry(root, rsdp.root_is_xsdt, i);
        t->mapped = t->phys ? map_table(t->phys, &tlen) : NULL;
        if (!t->mapped) {
            pr_warn("root entry %zu: no valid table at %#llx\n", i, (unsigned long long)t->phys);
            continue;
        }
        t->len = tlen;
        memcpy(t->signature, t->mapped, 4);
        t->signature[4] = '\0';
        if (ntables++ > 0)
            *p++ = ' ';
        memcpy(p, t->signature, 4);
        p += 4;
    }
    *p = '\0';
    pr_info("%u tables: %s\n", ntables, list);
}

const void *acpi_find_table(const char *signature, usize *len)
{
    for (u32 i = 0; i < ntables; i++) {
        if (memcmp(tables[i].signature, signature, 4) == 0) {
            *len = tables[i].len;
            return tables[i].mapped;
        }
    }
    return NULL;
}

static void load_madt(void)
{
    usize len;
    const void *t = acpi_find_table("APIC", &len);

    if (!t)
        return;
    if (acpi_parse_madt(t, len, &madt) != 0) {
        pr_warn("malformed MADT ignored\n");
        return;
    }
    have_madt = true;
    pr_info("MADT: %u CPU(s), %u I/O APIC(s), local APIC at %#llx%s%s\n", madt.ncpus, madt.nioapics,
            (unsigned long long)madt.lapic_phys, madt.pcat_compat ? ", 8259 present" : "",
            madt.ignored ? ", entries ignored" : "");
    for (u32 irq = 0; irq < ACPI_ISA_IRQS; irq++) {
        const struct acpi_isa_irq *i = &madt.isa[irq];

        if (i->overridden)
            pr_info("  ISA IRQ %u -> GSI %u, %s, active %s\n", irq, i->gsi,
                    i->level_triggered ? "level" : "edge", i->active_low ? "low" : "high");
    }
}

static void load_hpet(void)
{
    usize len;
    const void *t = acpi_find_table("HPET", &len);

    if (t && acpi_parse_hpet(t, len, &hpet) == 0)
        have_hpet = true;
    else if (t)
        pr_warn("malformed HPET table ignored\n");
}

void acpi_init(const struct boot_info *bi)
{
    have_rsdp = load_rsdp(bi);
    if (!have_rsdp)
        return;
    load_root();
    load_madt();
    load_hpet();
}

const struct acpi_rsdp_info *acpi_rsdp(void)
{
    return have_rsdp ? &rsdp : NULL;
}

const struct acpi_madt_info *acpi_madt(void)
{
    return have_madt ? &madt : NULL;
}

const struct acpi_hpet_info *acpi_hpet(void)
{
    return have_hpet ? &hpet : NULL;
}
