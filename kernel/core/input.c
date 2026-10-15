// SPDX-License-Identifier: MIT
/*
 * Console input queue. See <core/input.h>.
 *
 * The ring indices run free (u32) and are masked on access: head - tail
 * is the number of bytes queued. Both change only under input_lock;
 * readers' sleep condition reads them without it, with acquire loads,
 * which is safe because a stale answer only costs one more check.
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
    ring[head % INPUT_QUEUE_SIZE] = byte;
    __atomic_store_n(&head, head + 1, __ATOMIC_RELEASE);
    return true;
}

static bool queue_empty(void)
{
    return __atomic_load_n(&head, __ATOMIC_ACQUIRE) == __atomic_load_n(&tail, __ATOMIC_ACQUIRE);
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

usize input_read(u8 *buf, usize max)
{
    for (;;) {
        /*
         * wait_event(), open-coded to count a sleep only once the task is
         * BLOCKED and has seen the queue empty: a test that sees the count
         * rise knows a byte it injects now must come through a wakeup.
         */
        for (;;) {
            sched_prepare_wait(&input_wq);
            if (!queue_empty())
                break;
            __atomic_fetch_add(&stats.sleeps, 1, __ATOMIC_RELAXED);
            schedule();
        }
        sched_finish_wait(&input_wq);

        irqflags_t flags = spin_lock_irqsave(&input_lock);
        u32 t = tail;
        usize n = 0;

        for (; n < max && t != head; n++)
            buf[n] = ring[t++ % INPUT_QUEUE_SIZE];
        __atomic_store_n(&tail, t, __ATOMIC_RELEASE);
        spin_unlock_irqrestore(&input_lock, flags);
        if (n)
            return n;
        /* another reader took the bytes first: wait again */
    }
}

bool input_present(void)
{
    return __atomic_load_n(&present, __ATOMIC_RELAXED);
}

void input_get_stats(struct input_stats *out)
{
    irqflags_t flags = spin_lock_irqsave(&input_lock);

    *out = stats;
    out->sleeps = __atomic_load_n(&stats.sleeps, __ATOMIC_RELAXED);
    out->queued = head - tail;
    spin_unlock_irqrestore(&input_lock, flags);
}

void input_test_flush(void)
{
    irqflags_t flags = spin_lock_irqsave(&input_lock);

    __atomic_store_n(&tail, head, __ATOMIC_RELEASE);
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
