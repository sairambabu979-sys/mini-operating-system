/* SPDX-License-Identifier: MIT */
/*
 * Console input: one queue of bytes that read(0, ...) drains.
 *
 * Producers push bytes from any context: the PS/2 keyboard's IRQ 1
 * handler, which decodes scan codes (set 1, <lib/kbdmap.h>) into bytes,
 * and input_inject() (tests). Readers block in
 * input_read() until at least one byte is queued. The kernel delivers
 * bytes as they come, with no line discipline and no echo: the reading
 * program edits and echoes.
 *
 * Invariants:
 *   - one leaf spinlock guards the ring and the stats; nothing else is
 *     taken under it, and wakeups happen after it is released;
 *   - a producer publishes `head` under the lock, then wakes every
 *     reader; a reader sleeps only after it found the ring empty with
 *     its task already marked BLOCKED, so no wakeup is lost;
 *   - when the ring is full the newest byte is dropped and counted.
 */
#ifndef MINIOS_CORE_INPUT_H
#define MINIOS_CORE_INPUT_H

#include <types.h>

#define INPUT_QUEUE_SIZE 256 /* bytes; a power of two */
#define INPUT_READ_MAX   256 /* the most one read() returns */

struct input_stats {
    u64 injected;        /* bytes pushed by input_inject() */
    u64 dropped;         /* bytes lost to a full queue */
    u64 sleeps;          /* times a reader found the queue empty and went to wait */
    u64 scancodes;       /* bytes read from the keyboard */
    u64 empty_kbd_polls; /* keyboard polls (IRQ 1 or input_poll_kbd()) that found nothing */
    u32 queued;          /* bytes waiting now */
};

/* Finds the input devices. Needs irq_init(), time_init() and the scheduler. */
void input_init(void);

/* True if the PS/2 keyboard is up, on IRQ 1. */
bool input_have_kbd(void);

/*
 * Reads what the keyboard has sent, at most 16 bytes, decodes it and
 * queues the result: IRQ 1's handler, also safe from process context.
 */
void input_poll_kbd(void);

/* True if some device can deliver input; read() fails with -ENODEV otherwise. */
bool input_present(void);

/* Queues bytes as if a device had delivered them. Any context. */
void input_inject(const u8 *bytes, usize n);

/* Blocks until at least one byte is queued; returns 1 to max (>= 1) bytes. Process context only. */
usize input_read(u8 *buf, usize max);

void input_get_stats(struct input_stats *out);

/*
 * For tests only: drops whatever is queued, disarms the hook below,
 * resets the scan code decoder (no key held, no prefix pending), and
 * restarts the ring indices just below the u32 wrap so tests cross it.
 */
void input_test_flush(void);

/*
 * For tests only: the next reader to find the queue empty has `byte`
 * pushed right after, before it sleeps, as an interrupt there would.
 */
void input_test_inject_at_next_sleep(u8 byte);

/*
 * For tests only: has the 8042 deliver one set 1 scan code as if typed,
 * then waits up to 100 ms for IRQ 1 to take it. False without a
 * keyboard, or if it was not taken.
 */
bool input_test_kbd_put(u8 scancode);

/* For tests only: overrides input_present(); returns the previous value. */
bool input_test_set_present(bool present);

#endif /* MINIOS_CORE_INPUT_H */
