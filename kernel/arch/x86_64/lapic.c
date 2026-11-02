// SPDX-License-Identifier: MIT
/*
 * Local APIC. See <arch/lapic.h>.
 *
 * Driven in the mode the firmware left: xAPIC through the MMIO page, or
 * x2APIC through MSRs (0x800 + offset / 16). An x2APIC hand-off is never
 * reverted. The SDM allows going back to xAPIC through the disabled
 * state, but firmware may lock x2APIC on (Intel parts with SGX, after the
 * AEPIC Leak mitigation), and then that transition raises #GP. Linux
 * stopped reverting for this reason.
 *
 * The "x2apic" command-line option switches to x2APIC mode when the CPU
 * has it, through xAPIC (the transition the SDM allows), so the suite can
 * run the MSR path too: firmware on recent Intel machines hands over in
 * it. The register layout is the same apart from three differences,
 * handled below: the ID is a full 32 bits, the ICR is a single 64-bit MSR,
 * and there is no delivery-status bit to poll.
 */
#define pr_fmt(fmt) "lapic: " fmt

#include <arch/cpu.h>
#include <arch/cpuid.h>
#include <arch/lapic.h>
#include <arch/msr.h>
#include <arch/paging.h>
#include <arch/trap.h>
#include <core/boot.h>
#include <core/klog.h>
#include <core/panic.h>
#include <lib/cmdline.h>
#include <mm/vmalloc.h>
#include <mm/vmm.h>

#define MSR_APIC_BASE    0x1Bu
#define APIC_BASE_BSP    (1ull << 8)
#define APIC_BASE_X2APIC (1ull << 10)
#define APIC_BASE_ENABLE (1ull << 11)

#define MSR_X2APIC_BASE    0x800u
#define X2APIC_ICR         0x830u
#define ICR_SELF           (1u << 18)
#define ICR_ASSERT         (1u << 14)
#define ICR_SEND_PENDING   (1u << 12)
#define IPI_SEND_SPINS     1000000
#define TIMER_DIVIDE_BY_16 0x3u

static volatile u32 *regs; /* xAPIC */
static bool x2apic;
static bool active;

static void lapic_write(u32 reg, u32 value)
{
    if (x2apic)
        wrmsr(MSR_X2APIC_BASE + reg / 16, value);
    else
        regs[reg / 4] = value;
}

u32 lapic_read(u32 reg)
{
    return x2apic ? (u32)rdmsr(MSR_X2APIC_BASE + reg / 16) : regs[reg / 4];
}

bool lapic_active(void)
{
    return active;
}

bool lapic_x2apic_mode(void)
{
    return x2apic;
}

u32 lapic_id(void)
{
    return x2apic ? lapic_read(LAPIC_ID) : lapic_read(LAPIC_ID) >> 24;
}

u32 lapic_boot_id(void)
{
    if (rdmsr(MSR_APIC_BASE) & APIC_BASE_X2APIC)
        return (u32)rdmsr(MSR_X2APIC_BASE + LAPIC_ID / 16);
    return cpuid(1, 0).ebx >> 24;
}

/*
 * An IPI to `dest`; in xAPIC mode, waits until the APIC has sent it. An
 * APIC sends within microseconds, so one still busy after a million
 * pauses is broken: a panic names it rather than the machine hanging.
 */
static void send_ipi(u32 dest, u32 low)
{
    if (x2apic) {
        wrmsr(X2APIC_ICR, (u64)dest << 32 | low); /* one write; no busy bit */
        return;
    }
    lapic_write(LAPIC_ICR_HI, dest << 24);
    lapic_write(LAPIC_ICR_LO, low);
    for (int spins = 0; lapic_read(LAPIC_ICR_LO) & ICR_SEND_PENDING; spins++) {
        if (spins == IPI_SEND_SPINS)
            panic("lapic: IPI %#x to APIC %u still pending after %d spins", low, dest, spins);
        cpu_pause();
    }
}

void lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

bool lapic_in_service(u8 vector)
{
    return lapic_read(LAPIC_ISR + (vector / 32u) * 0x10) & (1u << (vector % 32u));
}

u32 lapic_read_esr(void)
{
    lapic_write(LAPIC_ESR, 0); /* a write (of 0: x2APIC #GPs on others) latches the errors */
    u32 esr = lapic_read(LAPIC_ESR);

    lapic_write(LAPIC_ESR, 0);
    return esr;
}

/* Programs the LVT entries the MADT names as NMI inputs for this CPU. */
static void program_nmi_lints(const struct acpi_madt_info *madt)
{
    u32 uid = ACPI_UID_ALL_CPUS;
    bool programmed = false;

    for (u32 i = 0; i < madt->ncpus && i < ACPI_MAX_CPUS; i++)
        if (madt->cpu_apic_ids[i] == lapic_id())
            uid = madt->cpu_uids[i];

    for (u32 i = 0; i < madt->nlapic_nmis; i++) {
        const struct acpi_lapic_nmi *n = &madt->lapic_nmis[i];

        if (n->uid != ACPI_UID_ALL_CPUS && n->uid != uid)
            continue;
        /* NMIs are always edge-triggered; only the polarity is configurable. */
        lapic_write(n->lint ? LAPIC_LVT_LINT1 : LAPIC_LVT_LINT0,
                    LAPIC_LVT_NMI | (n->active_low ? LAPIC_LVT_ACTIVE_LOW : 0));
        programmed = true;
    }
    if (!programmed)
        lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_NMI); /* the PC convention */
}

void lapic_init(const struct acpi_madt_info *madt)
{
    u64 msr = rdmsr(MSR_APIC_BASE);
    u64 base = msr & ~0xFFFull & ((1ull << cpu_features()->phys_addr_bits) - 1);

    const char *why = "left on by the firmware";

    if (!(msr & APIC_BASE_X2APIC) && cpu_features()->x2apic &&
        cmdline_has_flag(boot_info_get()->cmdline, "x2apic")) {
        /* Disabled -> x2APIC directly would #GP: enable xAPIC first. */
        msr |= APIC_BASE_ENABLE;
        wrmsr(MSR_APIC_BASE, msr);
        msr |= APIC_BASE_X2APIC;
        wrmsr(MSR_APIC_BASE, msr);
        why = "switched on by the x2apic option";
    }
    if (msr & APIC_BASE_X2APIC) {
        x2apic = true; /* stay: leaving may #GP (see the top of this file) */
    } else {
        wrmsr(MSR_APIC_BASE, msr | APIC_BASE_ENABLE);
        if (base != madt->lapic_phys)
            pr_warn("MADT says %#llx, the APIC base MSR %#llx; using the MSR\n",
                    (unsigned long long)madt->lapic_phys, (unsigned long long)base);
        regs = iomap(base, PAGE_SIZE_4K, VM_WRITE | VM_NOCACHE);
        if (!regs)
            panic("cannot map the local APIC at %#llx", (unsigned long long)base);
    }
    active = true;

    u32 version = lapic_read(LAPIC_VERSION);
    u32 max_lvt = (version >> 16) & 0xFF;

    /* Everything masked before the APIC is (re-)enabled. */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED | VEC_APIC_TIMER);
    lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED | VEC_APIC_ERROR);
    if (max_lvt >= 4)
        lapic_write(LAPIC_LVT_PERF, LAPIC_LVT_MASKED);
    if (max_lvt >= 5)
        lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    if (max_lvt >= 6)
        lapic_write(LAPIC_LVT_CMCI, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TPR, 0); /* accept every priority class */

    /*
     * In-service bits left behind by the firmware would block every
     * vector of equal or lower priority forever. Each EOI clears the
     * highest one; 256 EOIs clear them all.
     */
    for (u32 i = 0; i < 256; i++) {
        bool any = false;

        for (u32 r = 0; r < 8; r++)
            any |= lapic_read(LAPIC_ISR + r * 0x10) != 0;
        if (!any)
            break;
        lapic_eoi();
    }

    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | VEC_APIC_SPURIOUS);
    program_nmi_lints(madt);
    lapic_write(LAPIC_TIMER_DIVIDE, TIMER_DIVIDE_BY_16);
    (void)lapic_read_esr(); /* discard errors from before the reset */
    lapic_write(LAPIC_LVT_ERROR, VEC_APIC_ERROR);

    if (x2apic)
        pr_info("id %u, version %#x, %u LVT entries, x2APIC mode (%s)\n", lapic_id(),
                version & 0xFF, max_lvt + 1, why);
    else
        pr_info("id %u, version %#x, %u LVT entries, at %#llx\n", lapic_id(), version & 0xFF,
                max_lvt + 1, (unsigned long long)base);
}

void lapic_timer_start_oneshot(u32 count)
{
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED | VEC_APIC_TIMER);
    lapic_write(LAPIC_TIMER_INIT, count);
}

u32 lapic_timer_current(void)
{
    return lapic_read(LAPIC_TIMER_CURRENT);
}

void lapic_timer_start_periodic(u8 vector, u32 count)
{
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_PERIODIC | vector);
    lapic_write(LAPIC_TIMER_INIT, count); /* writing the count starts the timer */
}

void lapic_timer_set_masked(bool masked)
{
    u32 lvt = lapic_read(LAPIC_LVT_TIMER);

    lapic_write(LAPIC_LVT_TIMER, masked ? lvt | LAPIC_LVT_MASKED : lvt & ~LAPIC_LVT_MASKED);
}

void lapic_send_self_nmi(void)
{
    send_ipi(lapic_id(), ICR_ASSERT | LAPIC_LVT_NMI);
}

void lapic_send_self_ipi(u8 vector)
{
    send_ipi(0, ICR_SELF | ICR_ASSERT | vector);
}
