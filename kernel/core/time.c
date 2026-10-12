// SPDX-License-Identifier: MIT
/*
 * PIT tick and TSC calibration.
 *
 * The PIT input clock is 1.193182 MHz; divisor 1193 gives 1000.15 Hz, a
 * tick of 999,847 ns. time_ns() multiplies by that integer, drifting
 * about 0.4 ppm, far below the PIT crystal's own tolerance.
 */
#define pr_fmt(fmt) "time: " fmt

#include <arch/io.h>
#include <arch/irqflags.h>
#include <core/irq.h>
#include <core/klog.h>
#include <core/panic.h>
#include <core/time.h>

#define PIT_INPUT_HZ  1193182u
#define PIT_CH0_DATA  0x40
#define PIT_CMD       0x43
#define PIT_CH0_MODE2 0x34 /* channel 0, lobyte/hibyte, rate generator, binary */
#define PIT_DIVISOR   ((PIT_INPUT_HZ + TIMER_HZ / 2) / TIMER_HZ)
#define NS_PER_TICK   ((u64)PIT_DIVISOR * 1000000000ull / PIT_INPUT_HZ)

static volatile u64 ticks;
static u64 tsc_freq_khz;
static void (*tick_hook)(void);

static void pit_irq(struct trap_frame *frame, void *ctx)
{
    (void)frame;
    (void)ctx;
    ticks++;
    if (tick_hook)
        tick_hook();
}

/* Counts TSC cycles across 50 PIT ticks. Needs interrupts enabled. */
static void calibrate_tsc(void)
{
    irqflags_t flags = arch_irq_save();

    cpu_irq_enable();
    u64 start_tick = ticks;

    while (ticks == start_tick)
        cpu_halt(); /* align to a tick edge */
    u64 t0 = rdtsc(), s = ticks;

    while (ticks < s + 50)
        cpu_halt();
    u64 t1 = rdtsc();

    if (!(flags & RFLAGS_IF))
        cpu_irq_disable(); /* back to the caller's state */
    tsc_freq_khz = (t1 - t0) * 1000000 / (50 * NS_PER_TICK);
}

void time_init(void)
{
    outb(PIT_CMD, PIT_CH0_MODE2);
    outb(PIT_CH0_DATA, PIT_DIVISOR & 0xFF);
    outb(PIT_CH0_DATA, PIT_DIVISOR >> 8);
    BUG_ON(irq_register(0, pit_irq, NULL) != 0);
    calibrate_tsc();
    pr_info("PIT at %u Hz (%llu ns/tick), TSC %llu.%03llu MHz\n", TIMER_HZ,
            (unsigned long long)NS_PER_TICK, (unsigned long long)(tsc_freq_khz / 1000),
            (unsigned long long)(tsc_freq_khz % 1000));
}

u16 pit_divisor(void)
{
    return PIT_DIVISOR;
}

u16 pit_read_counter(void)
{
    irqflags_t flags = arch_irq_save(); /* lo/hi reads must not be split */

    outb(PIT_CMD, 0x00); /* latch channel 0 */
    u8 lo = inb(PIT_CH0_DATA);
    u8 hi = inb(PIT_CH0_DATA);

    arch_irq_restore(flags);
    return (u16)(hi << 8 | lo);
}

u64 time_ticks(void)
{
    return ticks;
}

u64 time_ns(void)
{
    return ticks * NS_PER_TICK;
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
