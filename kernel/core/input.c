// SPDX-License-Identifier: MIT
/*
 * Console input queue. See <core/input.h>.
 *
 * The ring indices run free (u32) and are masked on access: head - tail
 * is the number of bytes queued. Both are touched only under input_lock.
 */
#define pr_fmt(fmt) "input: " fmt

#include <core/input.h>
#include <core/klog.h>
#include <core/spinlock.h>
#include <sched/sched.h>

static_assert((INPUT_QUEUE_SIZE & (INPUT_QUEUE_SIZE - 1)) == 0, "the ring is masked");

static struct spinlock input_lock = SPINLOCK_INIT("input");
static struct wait_queue input_wq = WAIT_QUEUE_INIT(input_wq);
static u8 ring[INPUT_QUEUE_SIZE];
static u32 head, tail; /* free-running: head - tail bytes queued */
static struct input_stats stats;
static bool present;

/* Under input_lock. Returns false if the byte was dropped. */
static bool push_locked(u8 byte)
{
    if (head - tail == INPUT_QUEUE_SIZE) {
        stats.dropped++;
        return false;
    }
    ring[head++ % INPUT_QUEUE_SIZE] = byte;
    return true;
}

void input_inject(const u8 *bytes, usize n)
{
    irqflags_t flags = spin_lock_irqsave(&input_lock);
    bool pushed = false;

    for (usize i = 0; i < n; i++) {
        stats.injected++;
        pushed |= push_locked(bytes[i]);
    }
    spin_unlock_irqrestore(&input_lock, flags);
    if (pushed)
        wake_up_all(&input_wq);
}

static bool racing_armed; /* see input_test_inject_at_next_sleep() */
static u8 racing_byte;

/*
 * The sleep condition of input_read(): takes up to max bytes, puts their
 * count in *taken, and is false if there were none. wait_event()
 * evaluates it only after the task is marked BLOCKED and on the queue, so
 * a byte pushed from then on wakes it and no wakeup is lost (the argument
 * is sched.c's, and is tested there). False is followed by a sleep, and is
 * counted so a test can tell that a reader is about to wait for the byte
 * it injects.
 */
static bool take(u8 *buf, usize max, usize *taken)
{
    irqflags_t flags = spin_lock_irqsave(&input_lock);
    bool race = false;
    usize n = 0;

    for (; n < max && tail != head; n++)
        buf[n] = ring[tail++ % INPUT_QUEUE_SIZE];
    if (!n) {
        stats.sleeps++;
        race = racing_armed;
        racing_armed = false;
    }
    spin_unlock_irqrestore(&input_lock, flags);
    if (race) /* as an interrupt between this check and the sleep would */
        input_inject(&racing_byte, 1);
    *taken = n;
    return n != 0;
}

usize input_read(u8 *buf, usize max)
{
    usize n;

    wait_event(&input_wq, take(buf, max, &n));
    return n;
}

bool input_present(void)
{
    return __atomic_load_n(&present, __ATOMIC_RELAXED);
}

void input_get_stats(struct input_stats *out)
{
    irqflags_t flags = spin_lock_irqsave(&input_lock);

    *out = stats;
    out->queued = head - tail;
    spin_unlock_irqrestore(&input_lock, flags);
}

/* Just below the u32 wrap: every test that moves 100 bytes or more crosses it. */
#define TEST_INDEX_START ((u32)0 - 100)

void input_test_flush(void)
{
    irqflags_t flags = spin_lock_irqsave(&input_lock);

    head = tail = TEST_INDEX_START;
    racing_armed = false;
    spin_unlock_irqrestore(&input_lock, flags);
}

void input_test_inject_at_next_sleep(u8 byte)
{
    irqflags_t flags = spin_lock_irqsave(&input_lock);

    racing_byte = byte;
    racing_armed = true;
    spin_unlock_irqrestore(&input_lock, flags);
}

bool input_test_set_present(bool value)
{
    return __atomic_exchange_n(&present, value, __ATOMIC_RELAXED);
}

void input_init(void)
{
    /* No input device yet: read() fails with -ENODEV until one is found. */
    present = false;
    pr_warn("no input device: read() returns -ENODEV\n");
}
