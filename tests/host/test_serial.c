// SPDX-License-Identifier: MIT
/*
 * The COM1 driver (kernel/drivers/serial.c) against a model 16550: its
 * port I/O comes here (tests/host/platform/arch/io.h). The model can be
 * absent, have a transmitter that never empties, a loopback that never
 * delivers, and a receive FIFO of bytes with errors, so every wait the
 * driver makes can be shown to end.
 */
#include "test.h"

#include <drivers/serial.h>

#include <string.h>

#define COM1 0x3F8

static struct {
    bool absent;         /* every read is 0xFF */
    bool tx_stuck;       /* the transmitter never has room */
    bool loop_broken;    /* loopback never delivers */
    u8 loop_corrupt;     /* XORed into the looped-back byte */
    unsigned loop_delay; /* line-status polls before a looped byte arrives */
    u8 ier, lcr, mcr;
    u8 rx[2048], rx_err[2048];
    unsigned rx_head, rx_tail;
    char tx[64];
    unsigned ntx, lsr_polls, data_reads, loop_ready_at;
} uart;

static void reset(void)
{
    memset(&uart, 0, sizeof(uart));
}

static void rx_push(u8 byte, u8 err)
{
    uart.rx[uart.rx_tail] = byte;
    uart.rx_err[uart.rx_tail++] = err;
}

u8 host_inb(u16 port)
{
    if (uart.absent)
        return 0xFF;
    switch (port - COM1) {
    case 0: /* receive buffer */
        uart.data_reads++;
        return uart.rx_head < uart.rx_tail ? uart.rx[uart.rx_head++] : 0;
    case 1:
        return uart.ier;
    case 5: { /* line status */
        bool have = uart.rx_head < uart.rx_tail;
        u8 lsr = uart.tx_stuck ? 0 : 0x60;

        uart.lsr_polls++;
        if (have && uart.lsr_polls >= uart.loop_ready_at)
            lsr |= 0x01 | uart.rx_err[uart.rx_head];
        return lsr;
    }
    default:
        return 0;
    }
}

void host_outb(u16 port, u8 value)
{
    if (uart.absent)
        return;
    switch (port - COM1) {
    case 0:
        if (uart.lcr & 0x80)
            return;            /* divisor latch */
        if (uart.mcr & 0x10) { /* loopback */
            if (!uart.loop_broken) {
                rx_push(value ^ uart.loop_corrupt, 0);
                uart.loop_ready_at = uart.lsr_polls + uart.loop_delay;
            }
        } else if (uart.ntx < sizeof(uart.tx)) {
            uart.tx[uart.ntx++] = (char)value;
        }
        return;
    case 1:
        if (!(uart.lcr & 0x80))
            uart.ier = value;
        return;
    case 3:
        uart.lcr = value;
        return;
    case 4:
        uart.mcr = value;
        return;
    default:
        return;
    }
}

TEST(serial_init_waits_for_the_loopback_byte_and_leaves_loopback)
{
    reset();
    uart.loop_delay = 50; /* a slow UART: the byte is not there at once */
    EXPECT_TRUE(serial_init());
    EXPECT_TRUE(serial_is_present());
    EXPECT_TRUE(uart.lsr_polls >= 50);
    EXPECT_EQ(uart.mcr & 0x10, 0);
    serial_write("a\n", 2);
    EXPECT_EQ(uart.ntx, 3);
    EXPECT_TRUE(memcmp(uart.tx, "a\r\n", 3) == 0);
}

TEST(serial_init_gives_up_on_a_uart_that_does_not_answer)
{
    reset();
    uart.loop_broken = true;
    EXPECT_TRUE(!serial_init());
    EXPECT_TRUE(!serial_is_present());
    EXPECT_EQ(uart.lsr_polls, SERIAL_POLL_MAX);
    EXPECT_EQ(uart.mcr & 0x10, 0); /* out of loopback even so */

    reset();
    uart.loop_corrupt = 0x01;
    EXPECT_TRUE(!serial_init());
    EXPECT_EQ(uart.mcr & 0x10, 0);

    reset();
    uart.absent = true;
    EXPECT_TRUE(!serial_init());
    serial_write("x", 1); /* nothing to wait for */
    EXPECT_EQ(uart.ntx, 0);
}

TEST(serial_gives_up_on_a_transmitter_that_never_empties)
{
    reset();
    EXPECT_TRUE(serial_init());
    serial_rx_enable();
    EXPECT_EQ(uart.ier, 1);
    uart.tx_stuck = true;
    uart.lsr_polls = 0;
    serial_write("line\n", 5);
    EXPECT_TRUE(!serial_is_present());
    EXPECT_EQ(uart.ntx, 0);
    EXPECT_EQ(uart.lsr_polls, SERIAL_POLL_MAX); /* one wait, not one per byte */
    EXPECT_EQ(uart.ier, 0);                     /* receive interrupt off */
    uart.lsr_polls = 0;
    serial_putc('\n');
    EXPECT_EQ(uart.lsr_polls, 0); /* and no waits after */
}

/* A newline is "\r\n": with a stuck transmitter, the "\r" alone uses up the wait. */
TEST(serial_newline_on_a_stuck_transmitter_waits_once)
{
    reset();
    EXPECT_TRUE(serial_init());
    uart.tx_stuck = true;
    uart.lsr_polls = 0;
    serial_putc('\n');
    EXPECT_EQ(uart.lsr_polls, SERIAL_POLL_MAX);
}

TEST(serial_drain_counts_dropped_bytes_against_its_limit)
{
    struct serial_rx_stats before, after;
    u8 buf[4];
    bool more, dead;

    reset();
    EXPECT_TRUE(serial_init());
    serial_rx_get_stats(&before);
    for (int i = 0; i < 1000; i++) /* a line that only delivers framing errors */
        rx_push('?', 0x08);
    uart.data_reads = 0;
    EXPECT_EQ(serial_rx_drain(buf, sizeof(buf), &more, &dead), 0);
    EXPECT_EQ(uart.data_reads, 4); /* took no more than the limit */
    EXPECT_TRUE(more && !dead);
    serial_rx_get_stats(&after);
    EXPECT_EQ(after.errors - before.errors, 4);

    reset();
    EXPECT_TRUE(serial_init());
    rx_push('a', 0);
    rx_push('?', 0x08);
    rx_push('b', 0);
    rx_push('c', 0);
    EXPECT_EQ(serial_rx_drain(buf, 3, &more, &dead), 2); /* a, (dropped), b: three taken */
    EXPECT_TRUE(buf[0] == 'a' && buf[1] == 'b' && more);
    EXPECT_EQ(serial_rx_drain(buf, 3, &more, &dead), 1);
    EXPECT_TRUE(buf[0] == 'c' && !more && !dead);

    uart.absent = true;
    EXPECT_EQ(serial_rx_drain(buf, 3, &more, &dead), 0);
    EXPECT_TRUE(dead);
}
