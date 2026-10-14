// SPDX-License-Identifier: MIT
/*
 * Interrupt controller, timer and lock tests.
 */
#include <arch/irqflags.h>
#include <arch/lapic.h>
#include <arch/setjmp.h>
#include <core/irq.h>
#include <core/ktest.h>
#include <core/panic.h>
#include <core/preempt.h>
#include <core/spinlock.h>
#include <core/time.h>
#include <drivers/pit.h>

/* Waits (spinning, with a TSC deadline) until `n` more ticks have passed, or gives up. */
static bool wait_ticks(u64 n)
{
    u64 target = time_ticks() + n;
    u64 deadline = rdtsc() + tsc_khz() * 1000 * 2; /* 2 s of TSC time */

    while (time_ticks() < target) {
        if (rdtsc() > deadline)
            return false;
        cpu_pause(); /* not hlt: with no live interrupt source it would never return */
    }
    return true;
}

/* Interrupts taken from whichever source drives the tick. */
static u64 tick_interrupts(void)
{
    return time_tick_source() == TICK_LAPIC ? irq_local_count(VEC_APIC_TIMER) : irq_count(0);
}

KTEST(timer_interrupts_are_delivered)
{
    u64 irqs = tick_interrupts();

    KEXPECT(arch_irqs_enabled());
    KEXPECT(wait_ticks(20));
    KEXPECT(tick_interrupts() >= irqs + 20);
}

/*
 * The tick rate is a property of the PIT programming, which is checked
 * exactly: in mode 2 the counter runs from the divisor down to 1. The TSC
 * comparison is only a sanity bound (it catches a wrong divisor, which is
 * off by integer factors), so it allows a factor of 2 fast or 4 slow.
 * It cannot be tight: under emulation, timer interrupts delayed by host
 * scheduling are delivered back to back or merged; 100 ticks have been
 * measured from 91.6 ms of TSC time to well over 130 ms on a loaded host.
 */
KTEST(pit_is_programmed_for_the_tick_rate)
{
    u16 lo = 0xFFFF, hi = 0;

    if (time_tick_source() != TICK_PIT)
        KTEST_SKIP("the local APIC timer drives the tick");

    KEXPECT_EQ(pit_divisor(), 1193); /* 1193182 Hz / 1000 */

    /*
     * Sample until the readings span most of a period, with a deadline.
     * Neither a sample count (on a fast CI runner, 2000 reads fit in half a
     * period) nor a number of ticks (a host that deschedules the vCPU
     * leaves only a handful of reads in two ticks) is a reliable proxy.
     */
    u64 deadline = rdtsc() + tsc_khz() * 1000; /* 1 s: never hang */

    while (hi - lo <= pit_divisor() / 2 && rdtsc() < deadline) {
        u16 c = pit_read_counter();

        lo = c < lo ? c : lo;
        hi = c > hi ? c : hi;
    }
    KEXPECT(lo >= 1 && hi <= pit_divisor());
    KEXPECT(hi - lo > pit_divisor() / 2); /* it is actually counting down the full range */

    KEXPECT(wait_ticks(1));
    u64 t0 = rdtsc(), s = time_ticks();

    KEXPECT(wait_ticks(100));
    u64 elapsed_us = (rdtsc() - t0) * 1000 / tsc_khz();
    u64 ticks = time_ticks() - s;

    KEXPECT(elapsed_us * 2 >= ticks * 1000);
    KEXPECT(elapsed_us <= ticks * 1000 * 4);
}

KTEST(time_ns_tracks_ticks)
{
    u64 a = time_ns();

    KEXPECT(wait_ticks(5));
    u64 b = time_ns();

    KEXPECT(b > a);
    KEXPECT(b - a >= 4 * 999000ull); /* at least ~4 full ticks */
}

static void mask_tick(bool masked)
{
    if (time_tick_source() == TICK_LAPIC)
        lapic_timer_set_masked(masked);
    else if (masked)
        irq_mask(0);
    else
        irq_unmask(0);
}

KTEST(masked_tick_delivers_nothing)
{
    mask_tick(true);
    u64 before = time_ticks();

    tsc_delay_us(20000); /* 20 ms with the timer masked: interrupts stay on */
    KEXPECT_EQ(time_ticks(), before);
    mask_tick(false);
    KEXPECT(wait_ticks(2)); /* and it comes back */
}

KTEST(spinlock_restores_interrupt_state)
{
    struct spinlock lock = SPINLOCK_INIT("test");
    u32 preempt = preempt_count;

    irqflags_t f = spin_lock_irqsave(&lock);

    KEXPECT(!arch_irqs_enabled());
    KEXPECT_EQ(preempt_count, preempt + 1);
    KEXPECT(spin_is_locked(&lock));
    spin_unlock_irqrestore(&lock, f);
    KEXPECT(arch_irqs_enabled());
    KEXPECT_EQ(preempt_count, preempt);

    /* Nested, starting with interrupts off: they must stay off. */
    struct spinlock inner = SPINLOCK_INIT("inner");
    irqflags_t outer = arch_irq_save();
    irqflags_t g = spin_lock_irqsave(&inner);

    spin_unlock_irqrestore(&inner, g);
    KEXPECT(!arch_irqs_enabled());
    arch_irq_restore(outer);
}

KTEST(spinlock_recursion_is_detected)
{
    struct spinlock lock = SPINLOCK_INIT("recursive");
    struct kjmp_buf jb;
    struct panic_expectation e;
    u32 preempt = preempt_count;
    irqflags_t outer = arch_irq_save();
    irqflags_t f = spin_lock_irqsave(&lock);

    panic_expect(&e, &jb);
    if (ksetjmp(&jb) == 0) {
        irqflags_t g = spin_lock_irqsave(&lock); /* would deadlock */

        (void)g;
    }
    panic_expect_clear();
    KEXPECT(e.hit);
    /* Undo the half-taken second acquisition, then release normally. */
    preempt_count = preempt + 1;
    spin_unlock_irqrestore(&lock, f);
    arch_irq_restore(outer);
}
