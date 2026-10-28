/* SPDX-License-Identifier: MIT */
/*
 * 16550-compatible UART on COM1: the primary kernel console.
 *
 * Output is polled, with interrupts off, so panic() can always print.
 * Receive is interrupt-driven (IRQ 4, received-data interrupt only); the
 * handler lives in core/input.c, which drains with serial_rx_drain().
 */
#ifndef MINIOS_DRIVERS_SERIAL_H
#define MINIOS_DRIVERS_SERIAL_H

#include <types.h>

/* Programs COM1 for 115200 8N1. Returns false if no UART responds. */
bool serial_init(void);

/* Blocking output. "\n" is translated to "\r\n". No-op without a UART. */
void serial_putc(char c);
void serial_write(const char *s, usize len);

/* True if serial_init() found a UART. */
bool serial_is_present(void);

/* The received-data interrupt on or off. */
void serial_rx_enable(void);
void serial_rx_disable(void);

/* Re-arms it (IER off, then on): a fresh edge for data still waiting. */
void serial_rx_kick(void);

/*
 * Reads up to `cap` received bytes into `buf` without waiting, and
 * returns how many. A byte with a parity or framing error, or a break,
 * is dropped and counted; an overrun is counted and the byte kept.
 * *more is set if data was still waiting at `cap`, *dead if the UART
 * reads as absent (line status 0xFF).
 */
usize serial_rx_drain(u8 *buf, usize cap, bool *more, bool *dead);

struct serial_rx_stats {
    u64 errors;   /* bytes dropped for a parity or framing error, or a break */
    u64 overruns; /* times the receiver overran: bytes were lost before these */
};

void serial_rx_get_stats(struct serial_rx_stats *out);

/*
 * For tests: sends `bytes` (at most 14, so the 16-byte FIFO has room)
 * through the UART's loopback, so they arrive as if typed on the other
 * end and raise IRQ 4 once interrupts come back on. It waits for the
 * console's last bytes to go out first, and interrupts stay off
 * throughout, so nothing printed can loop back with them. False if the
 * transmitter never drained.
 */
bool serial_test_loopback_inject(const u8 *bytes, usize n);

/*
 * For tests: as if transmit polling had read the line status while the
 * byte at the head of the receive FIFO had a framing error. Reading the
 * status clears those bits, so the drain must still drop that byte.
 */
void serial_test_framing_error_seen(void);

#endif /* MINIOS_DRIVERS_SERIAL_H */
