// SPDX-License-Identifier: MIT
/*
 * 16550 UART driver, polled. Interrupt-driven receive arrives with the
 * interrupt layer; output stays polled so panic() can always print.
 */
#include <arch/io.h>
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

#define LCR_DLAB      0x80
#define LCR_8N1       0x03
#define FCR_ENABLE    0xC7 /* enable, clear RX/TX, 14-byte threshold */
#define MCR_DTR_RTS   0x03
#define MCR_OUT2      0x08 /* gates the IRQ line on PC hardware */
#define MCR_LOOPBACK  0x10
#define LSR_THR_EMPTY 0x20
#define BAUD_DIVISOR  1 /* 115200 / 115200 */

static bool serial_present;

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
