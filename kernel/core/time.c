// SPDX-License-Identifier: MIT
/*
 * The tick and the TSC. See <core/time.h>.
 *
 * Boot measures the TSC (and, in APIC mode, the local APIC timer) against
 * a reference clock, polled with interrupts off: the HPET main counter
 * when ACPI describes one, else a one-shot on PIT channel 2. The tick then
 * comes from the local APIC timer, programmed from that measurement, or
 * from PIT channel 0 when the kernel runs on the 8259.
 *
 * PIT tick: divisor 1193 gives 1000.15 Hz, a tick of 999,847 ns. LAPIC
 * tick: the nearest whole count to 1 ms; the period is what that count
 * measured, so time_ns() inherits the calibration error (well under
 * 0.1% against the HPET; see the tests).
 */
#define pr_fmt(fmt) "time: " fmt

#include <acpi/acpi.h>
#include <arch/irqflags.h>
#include <arch/lapic.h>
#include <core/boot.h>
#include <core/irq.h>
#include <core/klog.h>
#include <core/panic.h>
#include <core/time.h>
#include <drivers/hpet.h>
#include <drivers/pit.h>
#include <lib/cmdline.h>

#define PIT_TICK_DIVISOR    ((PIT_INPUT_HZ + TIMER_HZ / 2) / TIMER_HZ)
#define PIT_NS_PER_TICK     ((u64)PIT_TICK_DIVISOR * 1000000000ull / PIT_INPUT_HZ)
#define CALIBRATION_MS      50u /* PIT channel 2 can time at most 54.9 ms */
#define PIT_CALIBRATION_CNT (PIT_INPUT_HZ * CALIBRATION_MS / 1000u)

static volatile u64 ticks;
static u64 ns_per_tick;
static u64 tsc_freq_khz;
static u64 lapic_freq_hz;
static enum tick_source tick_source;
static enum ref_clock ref_clock;
static void (*tick_hook)(void);

static void tick_interrupt(struct trap_frame *frame, void *ctx)
{
    (void)frame;
    (void)ctx;
    ticks++;
    if (tick_hook)
        tick_hook();
}

#define REF_TIMEOUT_CYCLES (1ull << 32) /* 1-4 s at any plausible TSC rate */
#define TSC_MIN_KHZ        100000ull    /* 100 MHz */
#define TSC_MAX_KHZ        20000000ull  /* 20 GHz */

struct calibration {
    bool ok;   /* the reference completed the window and the rate is plausible */
    bool dead; /* it never completed the window: retrying cannot help */
    u64 elapsed_ns;
    u64 tsc;   /* TSC cycles in that time */
    u64 lapic; /* local APIC timer counts (divider 16), if it was running */
    u64 skew;  /* TSC cycles the endpoints are uncertain by */
};

/*
 * One reading of the reference (and the LAPIC timer) between two TSC
 * reads. Its time is the midpoint; its uncertainty, the gap. A gap much
 * wider than a few register reads means something ran in between: host
 * scheduling under emulation, an SMI on real hardware.
 */
struct sample {
    u64 tsc;
    u64 width;
    u64 ref;
    u32 lapic;
};

static struct sample take_sample(bool with_lapic)
{
    struct sample s;
    u64 a = rdtsc();

    s.ref = ref_clock == REF_HPET ? hpet_counter() : pit_oneshot_done();
    s.lapic = with_lapic ? lapic_timer_current() : 0;
    u64 b = rdtsc();

    s.tsc = a + (b - a) / 2;
    s.width = b - a;
    return s;
}

/* Waits for the HPET to count `target` ticks past start.ref. False if it never does. */
static bool hpet_window(const struct sample *start, u64 target)
{
    u64 mask = hpet_counter_mask();

    while (((hpet_counter() - start->ref) & mask) < target) {
        if (rdtsc() - start->tsc > REF_TIMEOUT_CYCLES)
            return false; /* the counter is stopped or crawling */
        cpu_pause();
    }
    return true;
}

/*
 * Times one CALIBRATION_MS window on the reference clock. Interrupts are
 * off throughout: one arriving between two reads would stretch only one
 * side of the measurement. A reference that never completes the window,
 * or yields an implausible TSC rate, makes the result not ok: an
 * unbounded wait here would hang the boot with nothing on the screen.
 */
static struct calibration measure_once(bool with_lapic)
{
    struct calibration c = {0};
    struct sample start, end;
    irqflags_t flags = arch_irq_save();

    if (with_lapic)
        lapic_timer_start_oneshot(0xFFFFFFFFu);

    if (ref_clock == REF_HPET) {
        start = take_sample(with_lapic);
        if (!hpet_window(&start, CALIBRATION_MS * 1000000000000ull / hpet_period_fs())) {
            arch_irq_restore(flags);
            c.dead = true;
            return c;
        }
        end = take_sample(with_lapic);
        c.elapsed_ns = ((end.ref - start.ref) & hpet_counter_mask()) * hpet_period_fs() / 1000000;
    } else {
        /* The count starts during the programming: bracket that instead. */
        u64 a = rdtsc();

        pit_oneshot_start(PIT_CALIBRATION_CNT);
        start = (struct sample){.lapic = with_lapic ? lapic_timer_current() : 0};
        start.width = rdtsc() - a;
        start.tsc = a + start.width / 2;

        /*
         * The output rose between the last two polls: the end is their
         * span. Mode 0 drives it low as soon as it is programmed, so high
         * at the first poll means no counting PIT (a missing one reads as
         * 0xFF) or a stall longer than the window: either way, no result.
         */
        struct sample prev = take_sample(with_lapic);

        if (prev.ref) {
            arch_irq_restore(flags);
            c.dead = true;
            return c;
        }
        while (!(end = take_sample(with_lapic)).ref) {
            if (end.tsc - start.tsc > REF_TIMEOUT_CYCLES) {
                arch_irq_restore(flags);
                c.dead = true;
                return c; /* channel 2 never ran out: clock-gated */
            }
            prev = end;
        }
        end.width = end.tsc + end.width / 2 - (prev.tsc - prev.width / 2);
        end.tsc = (prev.tsc + end.tsc) / 2;
        c.elapsed_ns = (u64)PIT_CALIBRATION_CNT * 1000000000ull / PIT_INPUT_HZ;
    }
    arch_irq_restore(flags);

    c.tsc = end.tsc - start.tsc;
    c.lapic = with_lapic ? start.lapic - end.lapic : 0;
    c.skew = start.width + end.width;

    u64 khz = c.elapsed_ns ? c.tsc * 1000000 / c.elapsed_ns : 0;

    c.ok = khz >= TSC_MIN_KHZ && khz <= TSC_MAX_KHZ;
    return c;
}

/*
 * Up to five windows, keeping the first whose endpoints are pinned to
 * within 0.1% of its length (else the tightest good one seen). Without
 * this, a single preemption at an endpoint skews the TSC rate for the
 * whole run. Not ok only if no window succeeded at all. A reference that
 * timed out is not tried again: each timeout is 2^32 TSC cycles with
 * interrupts off and nothing printed, and five of them looked like the
 * very hang the timeout exists to prevent.
 */
static struct calibration measure(bool with_lapic)
{
    struct calibration best = {0};

    for (int attempt = 0; attempt < 5; attempt++) {
        struct calibration c = measure_once(with_lapic);

        if (c.dead)
            break;
        if (!c.ok)
            continue;
        if (!best.ok || c.skew < best.skew)
            best = c;
        if (c.skew * 1000 <= c.tsc)
            return c;
    }
    if (best.ok)
        pr_warn("calibration endpoints uncertain by %llu of %llu TSC cycles\n",
                (unsigned long long)best.skew, (unsigned long long)best.tsc);
    return best;
}

static void choose_reference(void)
{
    const struct acpi_hpet_info *hpet = acpi_hpet();

    ref_clock = REF_PIT;
    if (cmdline_has_flag(boot_info_get()->cmdline, "nohpet"))
        pr_info("HPET disabled on the command line\n");
    else if (hpet && hpet_init(hpet))
        ref_clock = REF_HPET;
}

static void start_lapic_tick(void)
{
    u32 count = (u32)((lapic_freq_hz + TIMER_HZ / 2) / TIMER_HZ);

    if (count == 0)
        panic("local APIC timer runs at %llu Hz: too slow for a %u Hz tick",
              (unsigned long long)lapic_freq_hz, TIMER_HZ);
    ns_per_tick = (u64)count * 1000000000ull / lapic_freq_hz;
    BUG_ON(irq_register_local(VEC_APIC_TIMER, tick_interrupt, NULL) != 0);
    lapic_timer_start_periodic(VEC_APIC_TIMER, count);
    tick_source = TICK_LAPIC;
}

static void start_pit_tick(void)
{
    ns_per_tick = PIT_NS_PER_TICK;
    pit_start_periodic(PIT_TICK_DIVISOR);
    BUG_ON(irq_register(0, tick_interrupt, NULL) != 0);
    tick_source = TICK_PIT;
}

void time_init(void)
{
    bool apic = irq_mode() == IRQ_MODE_APIC;

    choose_reference();
    bool hpet_tried = ref_clock == REF_HPET;
    struct calibration c = measure(apic);

    if (!c.ok && ref_clock == REF_HPET) {
        pr_warn("the HPET did not time a %u ms window; calibrating against the PIT\n",
                CALIBRATION_MS);
        ref_clock = REF_PIT;
        c = measure(apic);
    }
    if (!c.ok)
        panic("no working reference clock: the %s did not time a %u ms window, so the tick "
              "and the TSC cannot be calibrated",
              hpet_tried    ? "HPET and PIT channel 2 both"
              : acpi_hpet() ? "PIT (HPET not used)"
                            : "PIT (no HPET)",
              CALIBRATION_MS);
    tsc_freq_khz = c.tsc * 1000000 / c.elapsed_ns;
    if (apic) {
        lapic_freq_hz = c.lapic * 1000000000ull / c.elapsed_ns;
        start_lapic_tick();
    } else {
        if (!pit_present())
            panic("no PIT: the 8259 path (%s) has no other tick", irq_pic_reason());
        start_pit_tick();
    }

    pr_info("tick: %s at %u Hz (%llu ns/tick); TSC %llu.%03llu MHz; calibrated against the %s\n",
            tick_source == TICK_LAPIC ? "local APIC timer" : "PIT", TIMER_HZ,
            (unsigned long long)ns_per_tick, (unsigned long long)(tsc_freq_khz / 1000),
            (unsigned long long)(tsc_freq_khz % 1000), ref_clock == REF_HPET ? "HPET" : "PIT");
    if (apic)
        pr_info("local APIC timer: %llu Hz after the divide-by-%u\n",
                (unsigned long long)lapic_freq_hz, LAPIC_TIMER_DIVIDER);
}

enum tick_source time_tick_source(void)
{
    return tick_source;
}

enum ref_clock time_ref_clock(void)
{
    return ref_clock;
}

u64 time_ns_per_tick(void)
{
    return ns_per_tick;
}

u64 lapic_timer_hz(void)
{
    return lapic_freq_hz;
}

u64 time_ticks(void)
{
    return ticks;
}

u64 time_ns(void)
{
    return ticks * ns_per_tick;
}

u64 tsc_khz(void)
{
    return tsc_freq_khz;
}

void tsc_delay_us(u64 us)
{
    u64 end = rdtsc() + us * tsc_freq_khz / 1000;

    while (rdtsc() < end)
        cpu_pause();
}

void time_set_tick_hook(void (*hook)(void))
{
    tick_hook = hook;
}
