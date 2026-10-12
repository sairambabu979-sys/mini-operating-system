// SPDX-License-Identifier: MIT
/*
 * ACPI discovery, local APIC, I/O APIC and HPET tests.
 *
 * QEMU always provides ACPI with a MADT and (unless -machine hpet=off) an
 * HPET, so their absence is a failure, not a skip: a broken RSDP search
 * must not quietly turn the APIC tests into no-ops. What depends on the
 * boot configuration ("noapic", hpet=off) is checked against that
 * configuration instead.
 */
#include <acpi/acpi.h>
#include <arch/irqflags.h>
#include <arch/lapic.h>
#include <arch/trap.h>
#include <core/boot.h>
#include <core/irq.h>
#include <core/ktest.h>
#include <core/time.h>
#include <drivers/hpet.h>
#include <drivers/ioapic.h>
#include <drivers/pit.h>
#include <lib/cmdline.h>
#include <lib/errno.h>

static bool apic_mode(void)
{
    return irq_mode() == IRQ_MODE_APIC;
}

KTEST(acpi_tables_are_found_and_valid)
{
    const struct acpi_madt_info *madt = acpi_madt();
    usize len = 0;

    KEXPECT(acpi_rsdp() != NULL);
    KEXPECT(madt != NULL);
    if (!madt)
        return;
    KEXPECT(madt->ncpus >= 1);
    KEXPECT(madt->nioapics >= 1);
    KEXPECT(acpi_find_table("FACP", &len) != NULL); /* every ACPI system has a FADT */
    KEXPECT(len >= ACPI_SDT_HEADER_SIZE);
    KEXPECT(acpi_find_table("NONE", &len) == NULL);
}

KTEST(controller_matches_the_command_line)
{
    bool noapic = cmdline_has_flag(boot_info_get()->cmdline, "noapic");

    KEXPECT_EQ(irq_mode(), noapic ? IRQ_MODE_PIC : IRQ_MODE_APIC);
    KEXPECT_EQ(time_tick_source(), noapic ? TICK_PIT : TICK_LAPIC);
}

KTEST(reference_clock_matches_the_platform)
{
    bool nohpet = cmdline_has_flag(boot_info_get()->cmdline, "nohpet");

    KEXPECT_EQ(time_ref_clock(), acpi_hpet() && !nohpet ? REF_HPET : REF_PIT);
    KEXPECT(tsc_khz() > 100000); /* any x86-64 CPU runs its TSC above 100 MHz */
}

KTEST(lapic_is_configured)
{
    if (!apic_mode())
        KTEST_SKIP("8259 mode");

    const struct acpi_madt_info *madt = acpi_madt();
    bool listed = false;

    for (u32 i = 0; i < madt->ncpus && i < ACPI_MAX_CPUS; i++)
        listed |= madt->cpu_apic_ids[i] == lapic_id();
    KEXPECT(listed); /* this CPU is one the firmware describes */

    KEXPECT_EQ(lapic_read(LAPIC_SVR) & 0x1FF, LAPIC_SVR_ENABLE | VEC_APIC_SPURIOUS);
    KEXPECT_EQ(lapic_read(LAPIC_TPR) & 0xFF, 0);
    KEXPECT(lapic_read(LAPIC_LVT_LINT0) & LAPIC_LVT_MASKED); /* the 8259 is not wired in */
    KEXPECT_EQ(lapic_read(LAPIC_LVT_LINT1) & (LAPIC_LVT_MASKED | 0x700), LAPIC_LVT_NMI);
    KEXPECT_EQ(lapic_read(LAPIC_LVT_ERROR) & (LAPIC_LVT_MASKED | 0xFF), VEC_APIC_ERROR);
    KEXPECT_EQ(lapic_read_esr(), 0);
    KEXPECT_EQ(irq_apic_error_count(), 0);
}

KTEST(lapic_timer_is_periodic_at_one_millisecond)
{
    if (!apic_mode())
        KTEST_SKIP("8259 mode");

    u32 lvt = lapic_read(LAPIC_LVT_TIMER);
    u32 count = lapic_read(LAPIC_TIMER_INIT);
    u64 expected = (lapic_timer_hz() + TIMER_HZ / 2) / TIMER_HZ;

    KEXPECT_EQ(lvt & (LAPIC_LVT_MASKED | LAPIC_LVT_PERIODIC | 0xFF),
               LAPIC_LVT_PERIODIC | VEC_APIC_TIMER);
    KEXPECT_EQ(count, expected);
    KEXPECT(lapic_timer_current() <= count);
    /* Rounding to a whole count is the only error: well under 0.1%. */
    KEXPECT(time_ns_per_tick() > 999000 && time_ns_per_tick() < 1001000);
}

/* Waits for `n` ticks, with a TSC deadline so a dead timer fails instead of hanging. */
static bool wait_ticks(u64 n)
{
    u64 target = time_ticks() + n;
    u64 deadline = rdtsc() + tsc_khz() * 2000;

    while (time_ticks() < target) {
        if (rdtsc() > deadline)
            return false;
        cpu_halt();
    }
    return true;
}

/*
 * The tick against the HPET, end to end. The programming is checked
 * exactly above; this catches a calibration off by a factor (a divider or
 * unit mistake), so the bounds are deliberately wide. Under emulation,
 * ticks that come due while the host has descheduled the vCPU are merged,
 * never queued: 100 of them can take far longer than 100 ms of HPET time
 * (a +30% bound failed 1 run in 24 on a loaded host), but never less
 * than the 99 periods between the first and the last.
 */
KTEST(tick_rate_agrees_with_the_hpet)
{
    if (time_ref_clock() != REF_HPET)
        KTEST_SKIP("no HPET");

    KEXPECT(wait_ticks(1));
    u64 h0 = hpet_counter();

    KEXPECT(wait_ticks(100));
    u64 ns = ((hpet_counter() - h0) & hpet_counter_mask()) * hpet_period_fs() / 1000000;

    KEXPECT(ns >= 99ull * 1000000 * 95 / 100);
    KEXPECT(ns <= 100ull * 1000000 * 4);
}

/*
 * An HPET read pinned between two TSC reads at most 20 µs apart, retried
 * otherwise: a preemption between the two clocks' reads (host scheduling
 * here) would otherwise be measured as disagreement between them.
 */
static bool hpet_tsc_pair(u64 *hpet, u64 *tsc)
{
    for (int i = 0; i < 1000; i++) {
        u64 a = rdtsc();
        u64 h = hpet_counter();
        u64 b = rdtsc();

        if (b - a <= tsc_khz() / 50) {
            *hpet = h;
            *tsc = a + (b - a) / 2;
            return true;
        }
    }
    return false;
}

KTEST(hpet_counter_runs_at_its_period)
{
    if (time_ref_clock() != REF_HPET)
        KTEST_SKIP("no HPET");

    u64 h0, t0, h1, t1;

    KEXPECT(hpet_period_fs() > 0 && hpet_period_fs() <= 100000000);
    KEXPECT(hpet_tsc_pair(&h0, &t0));
    tsc_delay_us(20000);
    KEXPECT(hpet_tsc_pair(&h1, &t1));
    u64 ns = ((h1 - h0) & hpet_counter_mask()) * hpet_period_fs() / 1000000;
    u64 tsc_ns = (t1 - t0) * 1000000 / tsc_khz();

    /* The TSC was calibrated against this counter: they must agree closely. */
    KEXPECT(ns * 100 >= tsc_ns * 98);
    KEXPECT(ns * 100 <= tsc_ns * 102);
}

/*
 * Waits up to a second for a counter bumped by an interrupt handler. A
 * deadline, not a fixed delay: delivery is asynchronous, and under
 * emulation the vCPU may not run at all for a while.
 */
static bool wait_for_count(const volatile u64 *counter, u64 value)
{
    u64 deadline = rdtsc() + tsc_khz() * 1000;

    while (*counter < value && rdtsc() < deadline)
        cpu_pause();
    return *counter >= value;
}

static void count_irq(struct trap_frame *frame, void *ctx)
{
    (void)frame;
    (*(volatile u64 *)ctx)++;
}

/*
 * The PIT is ISA IRQ 0, which QEMU's MADT (like most firmware) overrides
 * onto GSI 2. Driving it through irq_register() proves the override is
 * honored and the redirection entry is right; then masking must stop it.
 */
KTEST(ioapic_routes_isa_irq0_through_its_override)
{
    if (!apic_mode())
        KTEST_SKIP("8259 mode");

    const struct acpi_isa_irq *isa = &acpi_madt()->isa[0];
    volatile u64 hits = 0;
    u64 entry = 0;

    KEXPECT(isa->overridden && isa->gsi != 0);
    KEXPECT_EQ(irq_register(0, count_irq, (void *)&hits), 0);
    KEXPECT(ioapic_read_entry(isa->gsi, &entry));
    KEXPECT_EQ(entry & 0xFF, VEC_FIRST_IRQ);
    KEXPECT_EQ(entry & (IOAPIC_REDIR_MASKED | IOAPIC_REDIR_LEVEL | IOAPIC_REDIR_ACTIVE_LOW), 0);
    KEXPECT_EQ(entry >> 56, lapic_id());

    pit_start_periodic(1193); /* 1 kHz */
    KEXPECT(wait_for_count(&hits, 5));

    irq_mask(0);
    tsc_delay_us(2000); /* let an interrupt already in flight land */
    u64 before = hits;

    tsc_delay_us(20000);
    KEXPECT_EQ(hits, before);
    KEXPECT(ioapic_read_entry(isa->gsi, &entry));
    KEXPECT(entry & IOAPIC_REDIR_MASKED);

    irq_unregister(0);
    pit_stop();
    KEXPECT_EQ(irq_register(2, count_irq, (void *)&hits), -ENODEV); /* GSI 2 belongs to IRQ 0 */
}

KTEST(lapic_self_ipi_waits_for_interrupts_to_be_enabled)
{
    if (!apic_mode())
        KTEST_SKIP("8259 mode");

    volatile u64 hits = 0;

    KEXPECT_EQ(irq_register_local(VEC_APIC_TEST, count_irq, (void *)&hits), 0);
    KEXPECT_EQ(irq_register_local(VEC_APIC_TEST, count_irq, (void *)&hits), -EBUSY);
    KEXPECT_EQ(irq_register_local(VEC_APIC_SPURIOUS, count_irq, (void *)&hits), -EINVAL);

    irqflags_t flags = arch_irq_save();

    lapic_send_self_ipi(VEC_APIC_TEST);
    tsc_delay_us(1000);
    KEXPECT_EQ(hits, 0); /* pending in the IRR, not delivered */
    arch_irq_restore(flags);
    KEXPECT(wait_for_count(&hits, 1));         /* delivered once interrupts are on */
    KEXPECT_EQ(hits, 1);                       /* exactly once */
    KEXPECT(!lapic_in_service(VEC_APIC_TEST)); /* and acknowledged */

    lapic_send_self_ipi(VEC_APIC_TEST);
    KEXPECT(wait_for_count(&hits, 2));
    KEXPECT_EQ(irq_local_count(VEC_APIC_TEST), 2);
    irq_unregister_local(VEC_APIC_TEST);
}
