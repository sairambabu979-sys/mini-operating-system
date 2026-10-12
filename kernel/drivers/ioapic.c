// SPDX-License-Identifier: MIT
/*
 * I/O APIC. See <drivers/ioapic.h>.
 *
 * Registers are reached through an index/data pair (IOREGSEL at +0x00,
 * IOWIN at +0x10), so every access is a two-step sequence that must not
 * be interleaved with another: one lock covers all I/O APICs.
 */
#define pr_fmt(fmt) "ioapic: " fmt

#include <arch/paging.h>
#include <core/klog.h>
#include <core/spinlock.h>
#include <drivers/ioapic.h>
#include <lib/errno.h>
#include <mm/vmalloc.h>
#include <mm/vmm.h>

#define IOREGSEL       0x00u
#define IOWIN          0x10u
#define REG_ID         0x00u
#define REG_VERSION    0x01u
#define REG_REDIR(pin) (0x10u + 2u * (pin))

struct ioapic {
    volatile u32 *regs;
    u8 id;
    u32 gsi_base;
    u32 pins;
};

static struct ioapic ioapics[ACPI_MAX_IOAPICS];
static u32 nioapics;
static struct spinlock lock = SPINLOCK_INIT("ioapic");

static u32 reg_read(const struct ioapic *io, u32 reg)
{
    io->regs[IOREGSEL / 4] = reg;
    return io->regs[IOWIN / 4];
}

static void reg_write(const struct ioapic *io, u32 reg, u32 value)
{
    io->regs[IOREGSEL / 4] = reg;
    io->regs[IOWIN / 4] = value;
}

static struct ioapic *find(u32 gsi, u32 *pin)
{
    for (u32 i = 0; i < nioapics; i++) {
        if (gsi >= ioapics[i].gsi_base && gsi - ioapics[i].gsi_base < ioapics[i].pins) {
            *pin = gsi - ioapics[i].gsi_base;
            return &ioapics[i];
        }
    }
    return NULL;
}

/* The high half (destination) first: the entry is never live half-written. */
static void write_entry(const struct ioapic *io, u32 pin, u64 entry)
{
    reg_write(io, REG_REDIR(pin), (u32)entry | (u32)IOAPIC_REDIR_MASKED);
    reg_write(io, REG_REDIR(pin) + 1, (u32)(entry >> 32));
    reg_write(io, REG_REDIR(pin), (u32)entry);
}

static u64 read_entry(const struct ioapic *io, u32 pin)
{
    return reg_read(io, REG_REDIR(pin)) | (u64)reg_read(io, REG_REDIR(pin) + 1) << 32;
}

static bool overlaps(u32 base, u32 pins)
{
    for (u32 i = 0; i < nioapics; i++)
        if (base < ioapics[i].gsi_base + ioapics[i].pins && ioapics[i].gsi_base < base + pins)
            return true;
    return false;
}

bool ioapic_init(const struct acpi_madt_info *madt)
{
    for (u32 i = 0; i < madt->nioapics; i++) {
        const struct acpi_ioapic *a = &madt->ioapics[i];
        struct ioapic io = {.id = a->id, .gsi_base = a->gsi_base};

        io.regs = iomap(a->phys, PAGE_SIZE_4K, VM_WRITE | VM_NOCACHE);
        if (!io.regs) {
            pr_warn("cannot map I/O APIC %u at %#llx\n", a->id, (unsigned long long)a->phys);
            continue;
        }
        u32 version = reg_read(&io, REG_VERSION);

        if (version == 0xFFFFFFFFu) { /* nothing decodes the address */
            pr_warn("I/O APIC %u at %#llx does not respond\n", a->id, (unsigned long long)a->phys);
            continue;
        }
        io.pins = ((version >> 16) & 0xFF) + 1;
        if (io.gsi_base + io.pins < io.gsi_base || overlaps(io.gsi_base, io.pins)) {
            pr_warn("I/O APIC %u: GSIs %u-%u overlap another; skipped\n", a->id, io.gsi_base,
                    io.gsi_base + io.pins - 1);
            continue;
        }
        for (u32 pin = 0; pin < io.pins; pin++)
            write_entry(&io, pin, IOAPIC_REDIR_MASKED);
        ioapics[nioapics++] = io;
        pr_info("I/O APIC %u at %#llx: version %#x, GSIs %u-%u\n", io.id,
                (unsigned long long)a->phys, version & 0xFF, io.gsi_base,
                io.gsi_base + io.pins - 1);
    }
    return nioapics > 0;
}

int ioapic_route(u32 gsi, u8 vector, u8 dest, bool active_low, bool level)
{
    u32 pin;
    struct ioapic *io = find(gsi, &pin);

    if (!io)
        return -ENODEV;
    u64 entry = vector | IOAPIC_REDIR_MASKED | (active_low ? IOAPIC_REDIR_ACTIVE_LOW : 0) |
                (level ? IOAPIC_REDIR_LEVEL : 0) | (u64)dest << 56;
    irqflags_t flags = spin_lock_irqsave(&lock);

    write_entry(io, pin, entry);
    spin_unlock_irqrestore(&lock, flags);
    return 0;
}

void ioapic_set_masked(u32 gsi, bool masked)
{
    u32 pin;
    struct ioapic *io = find(gsi, &pin);

    if (!io)
        return;
    irqflags_t flags = spin_lock_irqsave(&lock);
    u32 lo = reg_read(io, REG_REDIR(pin));

    reg_write(io, REG_REDIR(pin),
              masked ? lo | (u32)IOAPIC_REDIR_MASKED : lo & ~(u32)IOAPIC_REDIR_MASKED);
    spin_unlock_irqrestore(&lock, flags);
}

bool ioapic_read_entry(u32 gsi, u64 *entry)
{
    u32 pin;
    struct ioapic *io = find(gsi, &pin);

    if (!io)
        return false;
    irqflags_t flags = spin_lock_irqsave(&lock);

    *entry = read_entry(io, pin);
    spin_unlock_irqrestore(&lock, flags);
    return true;
}

u32 ioapic_pin_count(void)
{
    u32 n = 0;

    for (u32 i = 0; i < nioapics; i++)
        n += ioapics[i].pins;
    return n;
}
