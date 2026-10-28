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
#include <core/ktest.h>
#include <core/time.h>
#include <lib/string.h>
#include <sched/sched.h>

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
