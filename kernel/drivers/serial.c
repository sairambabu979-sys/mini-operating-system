// SPDX-License-Identifier: MIT
/*
 * 16550 UART driver. See <drivers/serial.h>: output polled, receive on
 * IRQ 4 through core/input.c.
 */
#include <arch/io.h>
#include <arch/irqflags.h>
#include <drivers/serial.h>

#define COM1 0x3F8

/* Register offsets from the base port. */
enum {
    UART_DATA = 0, /* THR/RBR; divisor low when DLAB=1 */
    UART_IER = 1,  /* interrupt enable; divisor high when DLAB=1 */
    UART_FCR = 2,  /* FIFO control (write) */
    UART_LCR = 3,  /* line control */
    UART_MCR = 4,  /* modem control */
    UART_LSR = 5,  /* line status */
};

#define LCR_DLAB       0x80
#define LCR_8N1        0x03
#define FCR_ENABLE     0xC7 /* enable, clear RX/TX, 14-byte threshold */
#define MCR_DTR_RTS    0x03
#define MCR_OUT2       0x08 /* gates the IRQ line on PC hardware */
#define MCR_LOOPBACK   0x10
#define LSR_DATA_READY 0x01
#define LSR_OVERRUN    0x02
#define LSR_ERRORS     0x1C /* parity, framing, break */
#define LSR_THR_EMPTY  0x20
#define LSR_TX_IDLE    0x40 /* shift register empty too */
#define LSR_ABSENT     0xFF
#define IER_RDA        0x01 /* received data available */
#define TX_IDLE_SPINS  1000000
#define BAUD_DIVISOR   1 /* 115200 / 115200 */

static bool serial_present;
static struct serial_rx_stats rx_stats;

bool serial_init(void)
{
    outb(COM1 + UART_IER, 0x00);
    outb(COM1 + UART_LCR, LCR_DLAB);
    outb(COM1 + UART_DATA, BAUD_DIVISOR & 0xFF);
    outb(COM1 + UART_IER, BAUD_DIVISOR >> 8);
    outb(COM1 + UART_LCR, LCR_8N1);
    outb(COM1 + UART_FCR, FCR_ENABLE);

    /* Loopback self-test: a byte written must read back unchanged. */
    outb(COM1 + UART_MCR, MCR_LOOPBACK | MCR_OUT2 | MCR_DTR_RTS);
    outb(COM1 + UART_DATA, 0xAE);
    if (inb(COM1 + UART_DATA) != 0xAE) {
        serial_present = false;
        return false;
    }

    outb(COM1 + UART_MCR, MCR_OUT2 | MCR_DTR_RTS);
    serial_present = true;
    return true;
}

static void serial_putc_raw(char c)
{
    while (!(inb(COM1 + UART_LSR) & LSR_THR_EMPTY))
        ;
    outb(COM1 + UART_DATA, (u8)c);
}

void serial_putc(char c)
{
    if (!serial_present)
        return;
    if (c == '\n')
        serial_putc_raw('\r');
    serial_putc_raw(c);
}

void serial_write(const char *s, usize len)
{
    for (usize i = 0; i < len; i++)
        serial_putc(s[i]);
}

bool serial_is_present(void)
{
    return serial_present;
}

void serial_rx_enable(void)
{
    outb(COM1 + UART_IER, IER_RDA);
}

void serial_rx_disable(void)
{
    outb(COM1 + UART_IER, 0x00);
}

void serial_rx_kick(void)
{
    outb(COM1 + UART_IER, 0x00);
    outb(COM1 + UART_IER, IER_RDA);
}

usize serial_rx_drain(u8 *buf, usize cap, bool *more, bool *dead)
{
    usize n = 0;

    *more = false;
    *dead = false;
    for (;;) {
        u8 lsr = inb(COM1 + UART_LSR);

        if (lsr == LSR_ABSENT) {
            *dead = true;
            break;
        }
        if (!(lsr & LSR_DATA_READY))
            break;
        if (n == cap) {
            *more = true;
            break;
        }
        u8 byte = inb(COM1 + UART_DATA);

        if (lsr & LSR_OVERRUN)
            rx_stats.overruns++;
        if (lsr & LSR_ERRORS) {
            rx_stats.errors++;
            continue;
        }
        buf[n++] = byte;
    }
    return n;
}

void serial_rx_get_stats(struct serial_rx_stats *out)
{
    irqflags_t flags = arch_irq_save();

    *out = rx_stats;
    arch_irq_restore(flags);
}

bool serial_test_loopback_inject(const u8 *bytes, usize n)
{
    irqflags_t flags = arch_irq_save();
    bool ok = n <= 14;

    outb(COM1 + UART_MCR, MCR_LOOPBACK | MCR_OUT2 | MCR_DTR_RTS);
    for (usize i = 0; ok && i < n; i++) {
        int spins = TX_IDLE_SPINS;

        outb(COM1 + UART_DATA, bytes[i]);
        while (!(inb(COM1 + UART_LSR) & LSR_TX_IDLE) && --spins)
            ;
        ok = spins != 0;
    }
    outb(COM1 + UART_MCR, MCR_OUT2 | MCR_DTR_RTS);
    arch_irq_restore(flags);
    return ok;
}
