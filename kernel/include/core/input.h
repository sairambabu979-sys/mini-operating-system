/* SPDX-License-Identifier: MIT */
/*
 * Console input: one queue of bytes that read(0, ...) drains.
 *
 * Producers push bytes from any context: interrupt handlers for the
 * input devices, and input_inject() (tests). Readers block in
 * input_read() until at least one byte is queued. The kernel delivers
 * bytes as they come, with no line discipline and no echo: the reading
 * program edits and echoes.
 *
 * Invariants (see docs/design/input.md):
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
    u64 injected; /* bytes pushed by input_inject() */
    u64 dropped;  /* bytes lost to a full queue */
    u64 sleeps;   /* times a reader found the queue empty and went to wait */
    u32 queued;   /* bytes waiting now */
};

/* Finds the input devices. Needs irq_init() and the scheduler. */
void input_init(void);

/* True if some device can deliver input; read() fails with -ENODEV otherwise. */
bool input_present(void);

/* Queues bytes as if a device had delivered them. Any context. */
void input_inject(const u8 *bytes, usize n);

/* Blocks until at least one byte is queued; returns 1 to max bytes. Process context only. */
usize input_read(u8 *buf, usize max);

void input_get_stats(struct input_stats *out);

/* For tests only: drops whatever is queued. */
void input_test_flush(void);

/* For tests only: overrides input_present(); returns the previous value. */
bool input_test_set_present(bool present);

#endif /* MINIOS_CORE_INPUT_H */
