// SPDX-License-Identifier: MIT
/*
 * 8254 PIT. See <drivers/pit.h>.
 */
#include <arch/io.h>
#include <arch/irqflags.h>
#include <drivers/pit.h>

#define PIT_CH0_DATA 0x40
#define PIT_CH2_DATA 0x42
#define PIT_CMD      0x43
#define PORT_B       0x61 /* bit 0: channel 2 gate, bit 1: speaker, bit 5: channel 2 output */

#define CMD_CH0_MODE2  0x34 /* channel 0, lobyte/hibyte, rate generator, binary */
#define CMD_CH0_MODE0  0x30 /* channel 0, lobyte/hibyte, interrupt on terminal count */
#define CMD_CH2_MODE0  0xB0 /* channel 2, lobyte/hibyte, interrupt on terminal count */
#define CMD_LATCH_CH0  0x00
#define PORT_B_GATE2   0x01
#define PORT_B_SPEAKER 0x02
#define PORT_B_OUT2    0x20

static u16 divisor;

static void load_ch0(u8 cmd, u16 count)
{
    irqflags_t flags = arch_irq_save(); /* the two data bytes must not be split */

    outb(PIT_CMD, cmd);
    outb(PIT_CH0_DATA, count & 0xFF);
    outb(PIT_CH0_DATA, count >> 8);
    arch_irq_restore(flags);
}

void pit_start_periodic(u16 d)
{
    divisor = d;
    load_ch0(CMD_CH0_MODE2, d);
}

void pit_stop(void)
{
    load_ch0(CMD_CH0_MODE0, 1);
}

u16 pit_divisor(void)
{
    return divisor;
}

u16 pit_read_counter(void)
{
    irqflags_t flags = arch_irq_save(); /* lo/hi reads must not be split */

    outb(PIT_CMD, CMD_LATCH_CH0);
    u8 lo = inb(PIT_CH0_DATA);
    u8 hi = inb(PIT_CH0_DATA);

    arch_irq_restore(flags);
    return (u16)(hi << 8 | lo);
}

void pit_oneshot_start(u16 count)
{
    /* Gate on, speaker off; mode 0 drives the output low until the count runs out. */
    outb(PORT_B, (inb(PORT_B) & ~PORT_B_SPEAKER) | PORT_B_GATE2);
    outb(PIT_CMD, CMD_CH2_MODE0);
    outb(PIT_CH2_DATA, count & 0xFF);
    outb(PIT_CH2_DATA, count >> 8);
}

bool pit_oneshot_done(void)
{
    return inb(PORT_B) & PORT_B_OUT2;
}

bool pit_present(void)
{
    pit_oneshot_start(0xFFFF); /* 55 ms: far longer than the check */
    return !pit_oneshot_done();
}
