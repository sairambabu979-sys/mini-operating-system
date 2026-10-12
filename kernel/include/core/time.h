/* SPDX-License-Identifier: MIT */
/*
 * Timekeeping: a 1 kHz tick from the local APIC timer (or the PIT on the
 * legacy path), and the TSC for fine-grained measurements. Both rates are
 * measured at boot against the HPET, or against the PIT without one.
 */
#ifndef MINIOS_CORE_TIME_H
#define MINIOS_CORE_TIME_H

#include <types.h>

#define TIMER_HZ 1000

enum tick_source {
    TICK_PIT,   /* PIT channel 0 on ISA IRQ 0 */
    TICK_LAPIC, /* local APIC timer, periodic */
};

enum ref_clock {
    REF_HPET, /* HPET main counter */
    REF_PIT,  /* PIT channel 2 one-shot */
};

/* Calibrates and starts the tick. Needs irq_init(); "nohpet" ignores the HPET. */
void time_init(void);

enum tick_source time_tick_source(void);
enum ref_clock time_ref_clock(void);
u64 time_ns_per_tick(void);

/* Local APIC timer input rate after its divider, in Hz (0 in PIT mode). */
u64 lapic_timer_hz(void);

/* Ticks since time_init(). Monotonic. */
u64 time_ticks(void);

/* Nanoseconds since time_init(), at tick resolution. */
u64 time_ns(void);

static inline u64 time_ms_to_ticks(u64 ms)
{
    return (ms * TIMER_HZ + 999) / 1000; /* round up: never sleep short */
}

static inline u64 rdtsc(void)
{
    u32 lo, hi;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* TSC frequency measured at boot, in kHz. */
u64 tsc_khz(void);

/* Spins (interrupts state untouched) for at least `us` microseconds. */
void tsc_delay_us(u64 us);

/* Registered per-tick callback (the scheduler). Runs in IRQ context. */
void time_set_tick_hook(void (*hook)(void));

#endif /* MINIOS_CORE_TIME_H */
