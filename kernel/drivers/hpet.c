// SPDX-License-Identifier: MIT
/*
 * HPET as a reference clock. See <drivers/hpet.h>.
 */
#define pr_fmt(fmt) "hpet: " fmt

#include <core/klog.h>
#include <drivers/hpet.h>
#include <mm/vmalloc.h>
#include <mm/vmm.h>

#define HPET_BLOCK_SIZE  0x400u
#define REG_CAPS         0x000u
#define REG_CONFIG       0x010u
#define REG_COUNTER      0x0F0u
#define REG_TIMER_CFG(n) (0x100u + 0x20u * (n))

#define CAPS_COUNT_64    (1ull << 13)
#define CONFIG_ENABLE    (1ull << 0)
#define CONFIG_LEGACY    (1ull << 1)
#define TIMER_INT_ENABLE (1ull << 2)
#define MAX_PERIOD_FS    100000000ull /* 100 ns: a 10 MHz minimum */

static volatile u64 *regs;
static u64 period_fs;
static u64 counter_mask;

static u64 reg_read(u32 reg)
{
    return regs[reg / 8];
}

static void reg_write(u32 reg, u64 value)
{
    regs[reg / 8] = value;
}

bool hpet_init(const struct acpi_hpet_info *info)
{
    volatile u64 *block = iomap(info->phys, HPET_BLOCK_SIZE, VM_WRITE | VM_NOCACHE);

    if (!block) {
        pr_warn("cannot map the timer block at %#llx\n", (unsigned long long)info->phys);
        return false;
    }
    regs = block;
    u64 caps = reg_read(REG_CAPS);
    u64 period = caps >> 32;
    u32 timers = ((caps >> 8) & 0x1F) + 1;

    if (period == 0 || period > MAX_PERIOD_FS) {
        pr_warn("invalid counter period %llu fs; not using the HPET\n", (unsigned long long)period);
        regs = NULL;
        return false;
    }

    /* Stop, take back legacy routing and every comparator interrupt, restart. */
    reg_write(REG_CONFIG, reg_read(REG_CONFIG) & ~(CONFIG_ENABLE | CONFIG_LEGACY));
    for (u32 n = 0; n < timers; n++)
        reg_write(REG_TIMER_CFG(n), reg_read(REG_TIMER_CFG(n)) & ~TIMER_INT_ENABLE);
    reg_write(REG_CONFIG, reg_read(REG_CONFIG) | CONFIG_ENABLE);

    period_fs = period;
    counter_mask = caps & CAPS_COUNT_64 ? ~0ull : 0xFFFFFFFFull;
    pr_info("%llu.%06llu MHz, %u-bit counter, %u comparators, at %#llx\n",
            (unsigned long long)(1000000000ull / period),
            (unsigned long long)(1000000000000000ull / period % 1000000),
            counter_mask == ~0ull ? 64 : 32, timers, (unsigned long long)info->phys);
    return true;
}

bool hpet_available(void)
{
    return regs != NULL;
}

u64 hpet_counter(void)
{
    return reg_read(REG_COUNTER) & counter_mask;
}

u64 hpet_counter_mask(void)
{
    return counter_mask;
}

u64 hpet_period_fs(void)
{
    return period_fs;
}
