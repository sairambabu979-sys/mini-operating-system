// SPDX-License-Identifier: MIT
#include "test.h"
#include <drivers/serial.h>
#include "platform/arch/io.h"
#include <string.h>

static struct {
    u8 lcr, mcr, ier, byte;
    unsigned delay, polls, received, errors, writes;
    bool pending, stalled, absent;
} uart;

void test_uart_outb(u16 port, u8 value)
{
    if (port == 0x3FB)
        uart.lcr = value;
    else if (port == 0x3FC)
        uart.mcr = value;
    else if (port == 0x3F9 && !(uart.lcr & 0x80))
        uart.ier = value;
    else if (port == 0x3F8 && !(uart.lcr & 0x80)) {
        uart.writes++;
        if (uart.mcr & 0x10) {
            uart.byte = value;
            uart.pending = true;
        }
    }
}

u8 test_uart_inb(u16 port)
{
    if (uart.absent)
        return 0xFF;
    if (port == 0x3FD) {
        uart.polls++;
        if (uart.stalled)
            return 0;
        if (uart.errors)
            return 0x69; /* data-ready, framing error, transmitter empty */
        return 0x60 | (uart.pending && uart.polls > uart.delay ? 1 : 0);
    }
    if (port == 0x3F8) {
        uart.received++;
        if (uart.errors) {
            uart.errors--;
            return 'x';
        }
        if (uart.pending && uart.polls > uart.delay) {
            uart.pending = false;
            return uart.byte;
        }
    }
    return 0;
}

TEST(serial_waits_for_loopback_and_restores_normal_mode)
{
    memset(&uart, 0, sizeof(uart));
    uart.delay = 100;
    EXPECT_TRUE(serial_init());
    EXPECT_TRUE(uart.polls > uart.delay);
    EXPECT_EQ(uart.mcr & 0x10, 0);
}

TEST(serial_missing_or_stalled_device_has_bounded_init)
{
    memset(&uart, 0, sizeof(uart));
    uart.stalled = true;
    EXPECT_TRUE(!serial_init());
    EXPECT_TRUE(uart.polls > 0 && uart.polls <= 100000);
    EXPECT_EQ(uart.mcr & 0x10, 0);
    uart.absent = true;
    EXPECT_TRUE(!serial_init());
    EXPECT_EQ(uart.mcr & 0x10, 0);
}

TEST(serial_stalled_transmit_disables_port_and_returns)
{
    memset(&uart, 0, sizeof(uart));
    EXPECT_TRUE(serial_init());
    serial_rx_enable();
    uart.stalled = true;
    uart.polls = 0;
    unsigned writes = uart.writes;

    serial_write("line\n", 5);
    EXPECT_TRUE(!serial_is_present());
    EXPECT_TRUE(uart.polls > 0 && uart.polls <= 100000);
    EXPECT_EQ(uart.writes, writes);
    EXPECT_EQ(uart.ier, 0);
}

TEST(serial_receive_budget_counts_discarded_bytes)
{
    memset(&uart, 0, sizeof(uart));
    EXPECT_TRUE(serial_init());
    uart.errors = 1024;
    unsigned before = uart.received;
    u8 buf[4];
    bool more, dead;

    EXPECT_EQ(serial_rx_drain(buf, sizeof(buf), &more, &dead), 0);
    EXPECT_EQ(uart.received - before, sizeof(buf));
    EXPECT_TRUE(more && !dead);
    EXPECT_EQ(serial_rx_drain(buf, 0, &more, &dead), 0);
    EXPECT_EQ(uart.received - before, sizeof(buf));
}
