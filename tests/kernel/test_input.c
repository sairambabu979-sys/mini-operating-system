// SPDX-License-Identifier: MIT
/*
 * The console input queue, through input_inject(): no device needed, so
 * these never skip. Reads that block run in detached kernel threads that
 * flag when they are done, and the test task waits for that flag only
 * with a deadline, freeing a stuck reader by feeding it: a lost wakeup
 * fails a test instead of hanging the run. A reader that is never woken
 * stays blocked, but it is no child of the test task, so no later
 * task_wait() waits on it; the state it can still touch is static.
 * input_test_flush() starts the ring indices just below the u32 wrap, so
 * every test that moves 100 bytes or more crosses it.
 */
#include <core/input.h>
#include <core/irq.h>
#include <core/ktest.h>
#include <core/time.h>
#include <drivers/i8042.h>
#include <lib/string.h>
#include <sched/sched.h>

#include "../common/kbd_keymap.h"

#define DEADLINE_MS 2000

static void inject_str(const char *s)
{
    input_inject((const u8 *)s, strlen(s));
}

static u64 sleeps(void)
{
    struct input_stats st;

    input_get_stats(&st);
    return st.sleeps;
}

/* Polls until `*count` reaches `want`; false at the deadline. */
static bool wait_count(const volatile u32 *count, u32 want)
{
    u64 deadline = time_ticks() + DEADLINE_MS;

    while (*count < want && time_ticks() < deadline)
        sleep_ms(1);
    return *count >= want;
}

/*
 * Frees readers stuck by a lost wakeup: injects `bytes` every millisecond,
 * each inject waking them again, until `*count` reaches `want`. False at
 * the deadline: the readers were never woken at all.
 */
static bool feed_until(const volatile u32 *count, u32 want, const u8 *bytes, usize n)
{
    u64 deadline = time_ticks() + DEADLINE_MS;

    while (*count < want && time_ticks() < deadline) {
        input_inject(bytes, n);
        sleep_ms(1);
    }
    return *count >= want;
}

/* Polls until a reader has gone to sleep since `before`. */
static bool wait_sleeping(u64 before)
{
    u64 deadline = time_ticks() + DEADLINE_MS;

    while (sleeps() == before && time_ticks() < deadline)
        sleep_ms(1);
    return sleeps() != before;
}

KTEST(input_queue_keeps_order_and_partial_reads)
{
    u8 buf[8];

    input_test_flush();
    inject_str("abc");
    KEXPECT_EQ(input_read(buf, 1), 1);
    KEXPECT_EQ(buf[0], 'a');
    KEXPECT_EQ(input_read(buf, sizeof(buf)), 2);
    KEXPECT(buf[0] == 'b' && buf[1] == 'c');
}

KTEST(input_full_queue_drops_newest_and_counts)
{
    static u8 bytes[300], got[INPUT_READ_MAX];
    struct input_stats before, after;

    input_test_flush();
    for (usize i = 0; i < sizeof(bytes); i++)
        bytes[i] = (u8)i;
    input_get_stats(&before);
    input_inject(bytes, sizeof(bytes));
    input_get_stats(&after);
    KEXPECT_EQ(after.queued, INPUT_QUEUE_SIZE);
    KEXPECT_EQ(after.dropped - before.dropped, sizeof(bytes) - INPUT_QUEUE_SIZE);
    KEXPECT_EQ(input_read(got, sizeof(got)), INPUT_QUEUE_SIZE);
    KEXPECT_EQ(memcmp(got, bytes, INPUT_QUEUE_SIZE), 0); /* the oldest kept, in order */
}

struct reader {
    u8 buf[8];
    volatile u32 got;
    volatile u32 done;
};

static int read_once(void *arg)
{
    struct reader *r = arg;

    r->got = (u32)input_read(r->buf, sizeof(r->buf));
    r->done = 1;
    return 0;
}

/* Starts a detached reader on whatever is queued; false if it could not be made. */
static bool start_reader(struct reader *r)
{
    *r = (struct reader){0};
    struct task *t = kthread_create(read_once, r, "reader", KTHREAD_DETACHED);

    KEXPECT(t != NULL);
    return t != NULL;
}

/* Frees the reader if it is stuck, and checks it ran to its end. */
static void finish_reader(struct reader *r)
{
    KEXPECT(feed_until(&r->done, 1, (const u8 *)"!", 1));
}

KTEST(input_reader_sleeps_until_injected)
{
    static struct reader r;

    input_test_flush();
    u64 before = sleeps();

    if (!start_reader(&r))
        return;
    KEXPECT(wait_sleeping(before)); /* it found the queue empty */
    u64 asleep = sleeps();

    sleep_ms(20);
    KEXPECT_EQ(sleeps(), asleep); /* and sleeps: a reader that polled would look again */
    KEXPECT_EQ(r.got, 0);
    inject_str("x");
    KEXPECT(wait_count(&r.got, 1)); /* the wakeup came */
    KEXPECT_EQ(r.buf[0], 'x');
    finish_reader(&r);
}

/*
 * A byte that arrives after the reader found the queue empty but before
 * it sleeps, which only an interrupt can do in a real run: the reader
 * must still get it. A reader that checked before it was on the wait
 * queue would sleep through this one.
 */
KTEST(input_byte_arriving_as_reader_sleeps_is_not_lost)
{
    static struct reader r;

    input_test_flush();
    input_test_inject_at_next_sleep('z');
    if (!start_reader(&r))
        return;
    KEXPECT(wait_count(&r.got, 1));
    KEXPECT_EQ(r.buf[0], 'z');
    finish_reader(&r);
}

#define STREAM_BYTES 3000

struct stream {
    volatile u32 produced, consumed, consumer_done;
    volatile bool out_of_order;
    u64 deadline; /* the producer gives up then, so a stuck consumer cannot hang it */
};

static int produce(void *arg)
{
    struct stream *s = arg;

    for (u32 i = 0; i < STREAM_BYTES; i++) {
        u8 b = (u8)(i * 7);

        input_inject(&b, 1);
        s->produced = i + 1;
        if (i % 3 == 0)
            sched_yield();
        /* stay under the queue size: the consumer, not a full queue, sets the pace */
        while (s->produced - s->consumed > INPUT_QUEUE_SIZE / 2) {
            if (time_ticks() > s->deadline)
                return 1;
            sched_yield();
        }
    }
    return 0;
}

static int consume(void *arg)
{
    struct stream *s = arg;
    u8 buf[16];

    while (s->consumed < STREAM_BYTES) {
        usize n = input_read(buf, 1 + s->consumed % sizeof(buf));

        for (usize i = 0; i < n; i++)
            if (buf[i] != (u8)((s->consumed + i) * 7))
                s->out_of_order = true;
        s->consumed += (u32)n;
    }
    s->consumer_done = 1;
    return 0;
}

KTEST(input_never_loses_a_wakeup)
{
    static struct stream s;
    static const u8 pad[16];
    int status;

    s = (struct stream){.deadline = time_ticks() + DEADLINE_MS};
    input_test_flush();
    struct task *c = kthread_create(consume, &s, "consume", KTHREAD_DETACHED);

    KEXPECT(c != NULL);
    if (!c)
        return;
    /* the producer always ends, by its deadline at the latest: it is reaped */
    struct task *p = kthread_create(produce, &s, "produce", 0);

    KEXPECT(p != NULL);
    if (!p) {
        s.deadline = 0;
        feed_until(&s.consumer_done, 1, pad, sizeof(pad));
        return;
    }
    int ppid = p->pid;

    KEXPECT(wait_count(&s.consumed, STREAM_BYTES));
    KEXPECT(!s.out_of_order);
    KEXPECT_EQ(task_wait(ppid, &status), ppid);
    /* more bytes may be missing than the queue holds: feed a little at a time */
    KEXPECT(feed_until(&s.consumer_done, 1, pad, sizeof(pad)));
}

#define SHARED_BYTES 200
#define END          0xFF /* never among the shared bytes */

struct shared {
    volatile u8 seen[SHARED_BYTES];
    volatile u32 total;
    volatile u32 per_reader[2];
    volatile u32 done;
    volatile u32 bad_returns; /* reads of one byte that returned another count */
};

struct share_arg {
    struct shared *s;
    int id;
};

static int share_reader(void *arg)
{
    struct share_arg *a = arg;

    for (;;) {
        u8 b;

        /* one byte at a time: a reader never takes both ends */
        if (input_read(&b, 1) != 1)
            __atomic_fetch_add(&a->s->bad_returns, 1, __ATOMIC_RELAXED);
        if (b == END) {
            __atomic_fetch_add(&a->s->done, 1, __ATOMIC_RELAXED);
            return 0;
        }
        if (b < SHARED_BYTES)
            a->s->seen[b]++;
        a->s->per_reader[a->id]++;
        __atomic_fetch_add(&a->s->total, 1, __ATOMIC_RELAXED);
    }
}

KTEST(input_two_readers_share_bytes)
{
    static struct shared s;
    static struct share_arg args[2] = {{&s, 0}, {&s, 1}};
    static const u8 ends[2] = {END, END};
    u8 bytes[SHARED_BYTES];

    memset((void *)&s, 0, sizeof(s));
    input_test_flush();
    for (int i = 0; i < 2; i++) {
        struct task *t = kthread_create(share_reader, &args[i], "sharer", KTHREAD_DETACHED);

        KEXPECT(t != NULL);
        if (!t) {
            feed_until(&s.done, (u32)i, ends, 1);
            return;
        }
    }
    for (int i = 0; i < SHARED_BYTES; i++)
        bytes[i] = (u8)i;
    for (int i = 0; i < SHARED_BYTES; i += 20) {
        input_inject(bytes + i, 20);
        sched_yield();
    }
    KEXPECT(wait_count(&s.total, SHARED_BYTES));
    input_inject(ends, 2); /* one push for both: it must wake both readers */
    KEXPECT(wait_count(&s.done, 2));
    if (!feed_until(&s.done, 2, ends, 1))
        return;
    for (int i = 0; i < SHARED_BYTES; i++)
        if (s.seen[i] != 1)
            ktest_fail(__FILE__, __LINE__, "byte %d delivered %u times", i, s.seen[i]);
    KEXPECT_EQ(s.per_reader[0] + s.per_reader[1], SHARED_BYTES);
    KEXPECT_EQ(s.bad_returns, 0);
}

/*
 * The PS/2 keyboard, typed through the real 8042 with command 0xD2: the
 * byte comes back on IRQ 1 as if the keyboard had sent it, but skips
 * translation, so these type set 1 codes. Each is consumed before the
 * next (input_test_kbd_put() waits), since a second 0xD2 overwrites the
 * first. All but the first skip on a machine without a controller.
 */

/* Types `n` scan codes; false at the first one IRQ 1 did not take. */
static bool type_codes(const u8 *codes, usize n)
{
    for (usize i = 0; i < n; i++)
        if (!input_test_kbd_put(codes[i]))
            return false;
    return true;
}

/* Checks the queue holds exactly `want`, without blocking on a short queue. */
static void expect_queued(const char *want)
{
    usize n = strlen(want);
    struct input_stats st;
    u8 got[INPUT_READ_MAX];

    input_get_stats(&st);
    KEXPECT_EQ(st.queued, n);
    if (st.queued != n || n == 0 || n > sizeof(got)) {
        input_test_flush();
        return;
    }
    KEXPECT_EQ(input_read(got, n), n);
    KEXPECT_EQ(memcmp(got, want, n), 0);
}

KTEST(kbd_controller_is_configured)
{
    struct input_stats st;
    u8 config;

    input_get_stats(&st);
    if (!input_have_kbd()) {
        KEXPECT_EQ(st.scancodes, 0); /* nothing was ever read from a missing controller */
        return;
    }
    KEXPECT(i8042_read_config(&config));
    /* keyboard IRQ on, mouse IRQ off, keyboard clock on, mouse clock off, translation on */
    KEXPECT_EQ(config & 0x73, 0x61);
}

KTEST(kbd_scancodes_reach_the_queue_through_irq1)
{
    static const u8 hi[] = {0x2A, 0x23, 0xA3, 0xAA, 0x17, 0x97, 0x1C, 0x9C}; /* H, i, Enter */

    if (!input_have_kbd())
        KTEST_SKIP("no 8042");
    input_test_flush();
    u64 irqs = irq_count(1);

    KEXPECT(type_codes(hi, sizeof(hi)));
    KEXPECT(irq_count(1) - irqs >= sizeof(hi));
    expect_queued("Hi\n");
}

KTEST(kbd_types_every_printable_character)
{
    static char want[sizeof(test_keys) / sizeof(test_keys[0]) + 1];
    usize n = 0;

    if (!input_have_kbd())
        KTEST_SKIP("no 8042");
    input_test_flush();
    for (usize i = 0; i < sizeof(test_keys) / sizeof(test_keys[0]); i++) {
        const struct test_key *k = &test_keys[i];
        u8 codes[4];
        usize len = 0;

        if (k->shift)
            codes[len++] = TEST_LSHIFT_MAKE;
        codes[len++] = k->make;
        codes[len++] = k->make | 0x80;
        if (k->shift)
            codes[len++] = TEST_LSHIFT_MAKE | 0x80;
        if (!type_codes(codes, len)) {
            ktest_fail(__FILE__, __LINE__, "IRQ 1 did not take the codes for '%c'", k->ch);
            input_test_flush();
            return;
        }
        if (k->ch == '\b')
            want[n++] = '\x7f'; /* Backspace types DEL */
        else
            want[n++] = k->ch;
        if (n == INPUT_QUEUE_SIZE / 2) { /* check in halves: the queue holds 256 */
            want[n] = '\0';
            expect_queued(want);
            n = 0;
        }
    }
    want[n] = '\0';
    expect_queued(want);
}

/*
 * A byte that reaches the controller while IRQ 1 is masked raises its
 * edge then, and on the APIC path the I/O APIC drops a masked edge: no
 * IRQ comes when the line is unmasked, and the controller, holding the
 * byte, raises no new one. input_init() polls once after registering for
 * this reason; this shows the hazard is real and the poll recovers it.
 */
KTEST(kbd_byte_left_pending_while_masked_is_recovered)
{
    static const u8 rest[] = {0x9E, 0x30, 0xB0, 0x1C, 0x9C}; /* a's break, b, Enter */
    struct input_stats st;

    if (!input_have_kbd())
        KTEST_SKIP("no 8042");
    input_test_flush();
    input_get_stats(&st);
    u64 before = st.scancodes;

    irq_mask(1);
    KEXPECT(i8042_test_put(0x1E)); /* a */
    irq_unmask(1);
    sleep_ms(50);
    input_get_stats(&st);
    if (irq_mode() == IRQ_MODE_APIC) {
        KEXPECT_EQ(st.scancodes, before); /* the edge was lost */
        KEXPECT(i8042_output_full());
    }
    input_poll_kbd();
    input_get_stats(&st);
    KEXPECT_EQ(st.scancodes, before + 1);
    KEXPECT(type_codes(rest, sizeof(rest)));
    expect_queued("ab\n");
}

KTEST(kbd_control_and_extended_keys)
{
    static const u8 codes[] = {
        0x1D, 0x2E, 0xAE, 0x9D,             /* Ctrl-C */
        0xE0, 0x48, 0xE0, 0xC8,             /* Up: nothing */
        0xE0, 0x1C, 0xE0, 0x9C,             /* keypad Enter */
        0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5, /* Pause: nothing */
        0x1E, 0x9E,                         /* a */
    };

    if (!input_have_kbd())
        KTEST_SKIP("no 8042");
    input_test_flush();
    KEXPECT(type_codes(codes, sizeof(codes)));
    expect_queued("\x03\na");
}
