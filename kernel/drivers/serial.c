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

/*
 * Reading the line status clears its receive-error bits, which describe
 * the byte at the head of the receive FIFO. Transmit polling reads it
 * too, so it keeps what it saw here for the drain to apply to that byte.
 * Both sides run with interrupts off, on the one CPU.
 */
static u8 lsr_rx_seen;

static u8 read_lsr(void)
{
    u8 lsr = inb(COM1 + UART_LSR);

    if (lsr != LSR_ABSENT)
        lsr_rx_seen |= lsr & (LSR_OVERRUN | LSR_ERRORS);
    return lsr;
}

bool serial_init(void)
{
    bool ok = false;

    serial_present = false;
    outb(COM1 + UART_IER, 0x00);
    outb(COM1 + UART_LCR, LCR_DLAB);
    outb(COM1 + UART_DATA, BAUD_DIVISOR & 0xFF);
    outb(COM1 + UART_IER, BAUD_DIVISOR >> 8);
    outb(COM1 + UART_LCR, LCR_8N1);
    outb(COM1 + UART_FCR, FCR_ENABLE);

    /*
     * Loopback self-test: a byte written must read back unchanged, once the
     * line status says it has arrived (a bounded wait: there is no clock
     * yet). Normal mode again whatever happened: a port left in loopback
     * would swallow everything sent to it.
     */
    outb(COM1 + UART_MCR, MCR_LOOPBACK | MCR_OUT2 | MCR_DTR_RTS);
    outb(COM1 + UART_DATA, 0xAE);
    for (int polls = 0; polls < SERIAL_POLL_MAX; polls++) {
        u8 lsr = inb(COM1 + UART_LSR);

        if (lsr == LSR_ABSENT)
            break;
        if (lsr & LSR_DATA_READY) {
            ok = inb(COM1 + UART_DATA) == 0xAE;
            break;
        }
    }
    outb(COM1 + UART_MCR, MCR_OUT2 | MCR_DTR_RTS);
    lsr_rx_seen = 0; /* the self-test's status reads are no received byte's */
    serial_present = ok;
    return ok;
}

/*
 * One byte, once the transmitter has room. A transmitter that never has
 * room (stuck, or gone) is given up on for good after SERIAL_POLL_MAX
 * polls rather than hang every later write, panic() included: it reads as
 * absent from then on, and its receive interrupt is turned off. Nothing
 * is logged, since the console may be holding its own lock.
 */
static void serial_putc_raw(char c)
{
    for (int polls = 0; polls < SERIAL_POLL_MAX; polls++) {
        u8 lsr = read_lsr();

        if (lsr == LSR_ABSENT)
            break;
        if (lsr & LSR_THR_EMPTY) {
            outb(COM1 + UART_DATA, (u8)c);
            return;
        }
    }
    serial_present = false;
    serial_rx_disable();
}

void serial_putc(char c)
{
    if (!serial_present)
        return;
    if (c == '\n') {
        serial_putc_raw('\r');
        if (!serial_present)
            return; /* given up on: one wait, not two */
    }
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
    usize n = 0, taken = 0;

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
        if (taken == cap) { /* dropped bytes count: a stream of them must not hold the CPU */
            *more = true;
            break;
        }
        u8 byte = inb(COM1 + UART_DATA);

        taken++;
        lsr |= lsr_rx_seen; /* what transmit polling saw of this byte */
        lsr_rx_seen = 0;
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

/* Spins until the transmitter has sent everything; false if it never does. */
static bool wait_tx_idle(void)
{
    for (int spins = TX_IDLE_SPINS; spins; spins--)
        if (read_lsr() & LSR_TX_IDLE)
            return true;
    return false;
}

void serial_test_framing_error_seen(void)
{
    irqflags_t flags = arch_irq_save();

    lsr_rx_seen |= 0x08; /* framing error */
    arch_irq_restore(flags);
}

bool serial_test_loopback_inject(const u8 *bytes, usize n)
{
    irqflags_t flags = arch_irq_save();
    bool ok = n <= 14 && wait_tx_idle(); /* what the console sent last must go out, not loop back */

    if (ok)
        outb(COM1 + UART_MCR, MCR_LOOPBACK | MCR_OUT2 | MCR_DTR_RTS);
    for (usize i = 0; ok && i < n; i++) {
        outb(COM1 + UART_DATA, bytes[i]);
        ok = wait_tx_idle();
    }
    outb(COM1 + UART_MCR, MCR_OUT2 | MCR_DTR_RTS);
    arch_irq_restore(flags);
    return ok;
}
