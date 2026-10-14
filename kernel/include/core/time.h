/* SPDX-License-Identifier: MIT */
/*
 * Timekeeping: the PIT drives a 1 kHz tick; the TSC is calibrated against
 * it for fine-grained measurements.
 */
#ifndef MINIOS_CORE_TIME_H
#define MINIOS_CORE_TIME_H

#include <types.h>

#define TIMER_HZ 1000

void time_init(void);

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

/* The divisor programmed into PIT channel 0, and a latched read of its counter. */
u16 pit_divisor(void);
u16 pit_read_counter(void);

/* TSC frequency measured at boot, in kHz. */
u64 tsc_khz(void);

/* Spins (interrupts state untouched) for at least `us` microseconds. */
void tsc_delay_us(u64 us);

/* Registered per-tick callback (the scheduler). Runs in IRQ context. */
void time_set_tick_hook(void (*hook)(void));

#endif /* MINIOS_CORE_TIME_H */
