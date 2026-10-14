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
#include <arch/cpuid.h>
#include <arch/irqflags.h>
#include <arch/lapic.h>
#include <arch/trap.h>
#include <core/boot.h>
#include <core/irq.h>
#include <core/ktest.h>
#include <core/printk.h>
#include <core/time.h>
#include <drivers/hpet.h>
#include <drivers/ioapic.h>
#include <drivers/pit.h>
#include <lib/cmdline.h>
#include <lib/errno.h>
#include <lib/string.h>

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

/*
 * The 8259 exactly when the command line says noapic or the CPU has no
 * local APIC (QEMU's -cpu max,-apic). The other reasons, no MADT and no
 * usable I/O APIC, already fail acpi_tables_are_found_and_valid.
 */
KTEST(controller_matches_the_command_line)
{
    bool pic = cmdline_has_flag(boot_info_get()->cmdline, "noapic") || !cpu_features()->apic;

    KEXPECT_EQ(irq_mode(), pic ? IRQ_MODE_PIC : IRQ_MODE_APIC);
    KEXPECT_EQ(time_tick_source(), pic ? TICK_PIT : TICK_LAPIC);
    KEXPECT_EQ(irq_pic_reason() != NULL, pic);
}

KTEST(reference_clock_matches_the_platform)
{
    bool nohpet = cmdline_has_flag(boot_info_get()->cmdline, "nohpet");

    KEXPECT_EQ(time_ref_clock(), acpi_hpet() && !nohpet ? REF_HPET : REF_PIT);
    KEXPECT(tsc_khz() > 100000); /* any x86-64 CPU runs its TSC above 100 MHz */
}

/* "x2apic" asks for x2APIC mode; the image that passes it must get it. */
KTEST(lapic_mode_matches_the_command_line)
{
    if (!apic_mode())
        KTEST_SKIP("8259 mode");

    bool wanted = cmdline_has_flag(boot_info_get()->cmdline, "x2apic");

    if (wanted)
        KEXPECT(cpu_features()->x2apic);     /* an emulator without it cannot run this image */
    KEXPECT_EQ(lapic_x2apic_mode(), wanted); /* QEMU's firmware never leaves x2APIC on */
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
        cpu_pause(); /* not hlt: with no live interrupt source it would never return */
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

/*
 * Calibration against a measurement that shares no code with time.c: this
 * test's own polled PIT channel 2 window, bracketed by TSC reads, the
 * tightest of up to eight tries. In the default boot the kernel used the
 * HPET, so two clocks are compared; with hpet=off, two implementations on
 * one. A calibration off by a factor (an injected 2x error passed every
 * other test) fails either way.
 *
 * The tolerance is 2% plus what the endpoints are uncertain by: under a
 * loaded host the vCPU can stop at any instruction, and a bracket wide
 * enough to contain such a stop is measured, not ignored.
 */
#define PIT_TEST_COUNT 47727u /* 40 ms */

struct pit_window {
    u64 tsc;   /* TSC cycles from the programming to the output edge */
    u64 slack; /* TSC cycles of uncertainty at the two ends */
};

static struct pit_window time_pit_window(void)
{
    struct pit_window w;
    irqflags_t flags = arch_irq_save();
    u64 a = rdtsc();

    pit_oneshot_start(PIT_TEST_COUNT);
    u64 b = rdtsc(), low = b, high;

    /*
     * The edge lies between the last poll that saw the output low and the
     * first that saw it high: bound it by a TSC read taken before the
     * former and one taken after the latter.
     */
    for (;;) {
        u64 t = rdtsc();

        if (pit_oneshot_done()) {
            high = rdtsc();
            break;
        }
        if (t - b > tsc_khz() * 1000) { /* a second: the PIT is not counting */
            high = t;
            low = b;
            break;
        }
        low = t;
    }
    arch_irq_restore(flags);
    w.tsc = (low + high) / 2 - (a + b) / 2;
    w.slack = (b - a) + (high - low);
    return w;
}

static struct pit_window best_pit_window(void)
{
    struct pit_window best = {.slack = ~0ull};

    for (int i = 0; i < 8; i++) {
        struct pit_window w = time_pit_window();

        if (w.slack < best.slack)
            best = w;
        if (best.slack * 1000 < best.tsc) /* endpoints pinned to 0.1% */
            break;
    }
    return best;
}

/*
 * |measured - expected| <= 2% of expected, plus `slack` parts per `total`.
 * A measurement uncertain by a quarter or more proves nothing (its bound
 * would let a 2x error through) and fails rather than passes.
 */
static bool agrees(u64 measured, u64 expected, u64 slack, u64 total)
{
    if (slack * 4 >= total)
        return false;

    u64 bound = expected / 50 + expected * slack / total + 1;

    return measured + bound >= expected && measured <= expected + bound;
}

KTEST(tsc_rate_matches_an_independent_measurement)
{
    if (!pit_present())
        KTEST_SKIP("no PIT to measure with");

    struct pit_window w = best_pit_window();
    u64 khz = w.tsc * PIT_INPUT_HZ / PIT_TEST_COUNT / 1000;

    KEXPECT(agrees(khz, tsc_khz(), w.slack, w.tsc));
}

/* A LAPIC timer reading pinned between two TSC reads at most 20 us apart. */
static bool lapic_tsc_pair(u32 *lapic, u64 *tsc, u64 *width)
{
    for (int i = 0; i < 1000; i++) {
        u64 a = rdtsc();
        u32 l = lapic_timer_current();
        u64 b = rdtsc();

        if (b - a <= tsc_khz() / 50) {
            *lapic = l;
            *tsc = a + (b - a) / 2;
            *width = b - a;
            return true;
        }
    }
    return false;
}

/*
 * The LAPIC timer against the TSC, and the TSC against this test's own PIT
 * window: neither ratio comes from time.c.
 */
KTEST(lapic_timer_rate_matches_an_independent_measurement)
{
    if (!apic_mode())
        KTEST_SKIP("8259 mode");
    if (!pit_present())
        KTEST_SKIP("no PIT to measure with");

    struct pit_window w = best_pit_window();
    u64 tsc_hz = w.tsc * PIT_INPUT_HZ / PIT_TEST_COUNT;
    u32 count = lapic_read(LAPIC_TIMER_INIT);
    u32 l0, l1;
    u64 t0, t1, w0, w1;

    /* The tick pauses for the measurement (40 ms). */
    lapic_timer_start_oneshot(0xFFFFFFFFu);
    KEXPECT(lapic_tsc_pair(&l0, &t0, &w0));
    tsc_delay_us(40000);
    KEXPECT(lapic_tsc_pair(&l1, &t1, &w1));
    lapic_timer_start_periodic(VEC_APIC_TIMER, count);

    u64 hz = (u64)(l0 - l1) * tsc_hz / (t1 - t0);

    KEXPECT(agrees(hz, lapic_timer_hz(), w.slack * (t1 - t0) / w.tsc + w0 + w1, t1 - t0));
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
    if (!pit_present())
        KTEST_SKIP("no PIT to drive IRQ 0");

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

/* The newest console output contains `needle`. */
static bool log_tail_contains(const char *needle)
{
    static char tail[1024];
    usize n = console_log_read(tail, sizeof(tail) - 1), len = strlen(needle);

    tail[n] = '\0';
    for (usize i = 0; i + len <= n; i++)
        if (memcmp(tail + i, needle, len) == 0)
            return true;
    return false;
}

/*
 * An NMI cannot be held off by cli, so its handler must not print: the
 * code it interrupted may own the console lock. It is counted at once and
 * reported by the next maskable interrupt.
 */
KTEST(nmi_is_counted_now_and_reported_later)
{
    if (!apic_mode())
        KTEST_SKIP("8259 mode");

    u64 before = trap_nmi_count();
    irqflags_t flags = arch_irq_save();

    kprintf("nmi-probe\n");
    lapic_send_self_nmi();
    u64 deadline = rdtsc() + tsc_khz() * 1000;

    while (trap_nmi_count() == before && rdtsc() < deadline)
        cpu_pause();
    KEXPECT_EQ(trap_nmi_count(), before + 1); /* delivered with interrupts off */
    char tail[10];

    /* Nothing at all was printed in NMI context: the log still ends with the probe. */
    KEXPECT_EQ(console_log_read(tail, sizeof(tail)), sizeof(tail));
    KEXPECT(memcmp(tail, "nmi-probe\n", sizeof(tail)) == 0);
    arch_irq_restore(flags);
    KEXPECT(wait_ticks(2));
    KEXPECT(log_tail_contains("1 NMI(s) received"));
}

/*
 * The other half of the override: polarity and trigger. QEMU's MADT, like
 * a PC's, makes the ACPI SCI (ISA 9) level-triggered, active high. The
 * line stays quiet: nothing enabled an ACPI event.
 */
KTEST(ioapic_programs_trigger_and_polarity_from_the_madt)
{
    if (!apic_mode())
        KTEST_SKIP("8259 mode");

    const struct acpi_isa_irq *isa = &acpi_madt()->isa[9];
    volatile u64 hits = 0;
    u64 entry = 0;

    KEXPECT(isa->overridden && isa->level_triggered && !isa->active_low);
    KEXPECT_EQ(irq_register(9, count_irq, (void *)&hits), 0);
    KEXPECT(ioapic_read_entry(isa->gsi, &entry));
    irq_unregister(9);
    KEXPECT_EQ(entry & (IOAPIC_REDIR_MASKED | IOAPIC_REDIR_LEVEL | IOAPIC_REDIR_ACTIVE_LOW),
               IOAPIC_REDIR_LEVEL);
    KEXPECT_EQ(entry & 0xFF, VEC_FIRST_IRQ + 9);
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
