// SPDX-License-Identifier: MIT
/*
 * The console input queue, through input_inject(): no device needed, so
 * these never skip. Readers that block run as kernel threads, and the
 * test task only ever polls with a deadline: a lost wakeup fails a test
 * instead of hanging the run.
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
};

static int read_once(void *arg)
{
    struct reader *r = arg;

    r->got = (u32)input_read(r->buf, sizeof(r->buf));
    return 0;
}

KTEST(input_reader_sleeps_until_injected)
{
    struct reader r = {0};
    int status;

    input_test_flush();
    u64 before = sleeps();
    struct task *t = kthread_create(read_once, &r, "reader", 0);

    KEXPECT(t != NULL);
    if (!t)
        return;
    int pid = t->pid;

    KEXPECT(wait_sleeping(before)); /* asleep on an empty queue, not spinning */
    KEXPECT_EQ(r.got, 0);
    inject_str("x");
    KEXPECT(wait_count(&r.got, 1)); /* the wakeup came */
    KEXPECT_EQ(r.buf[0], 'x');
    if (!r.got)
        inject_str("x"); /* free a stuck reader before reaping it */
    KEXPECT_EQ(task_wait(pid, &status), pid);
}

#define STREAM_BYTES 3000

struct stream {
    volatile u32 produced, consumed;
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
    return 0;
}

KTEST(input_never_loses_a_wakeup)
{
    struct stream s = {.deadline = time_ticks() + DEADLINE_MS};
    int status;

    input_test_flush();
    struct task *c = kthread_create(consume, &s, "consume", 0);
    struct task *p = kthread_create(produce, &s, "produce", 0);

    KEXPECT(c != NULL && p != NULL);
    if (!c || !p)
        return; /* cannot happen at boot; leaking a stuck thread beats hanging */
    int cpid = c->pid, ppid = p->pid;

    KEXPECT(wait_count(&s.consumed, STREAM_BYTES));
    KEXPECT(!s.out_of_order);
    KEXPECT_EQ(task_wait(ppid, &status), ppid);
    if (s.consumed < STREAM_BYTES) { /* lost a wakeup: feed the consumer to its end */
        static u8 pad[STREAM_BYTES];

        input_inject(pad, STREAM_BYTES - s.consumed);
    }
    KEXPECT_EQ(task_wait(cpid, &status), cpid);
}

#define SHARED_BYTES 200
#define END          0xFF /* never among the shared bytes */

struct shared {
    volatile u8 seen[SHARED_BYTES];
    volatile u32 total;
    volatile u32 per_reader[2];
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

        input_read(&b, 1); /* one byte at a time: a reader never takes both ends */
        if (b == END)
            return 0;
        if (b < SHARED_BYTES)
            a->s->seen[b]++;
        a->s->per_reader[a->id]++;
        __atomic_fetch_add(&a->s->total, 1, __ATOMIC_RELAXED);
    }
}

KTEST(input_two_readers_share_bytes)
{
    static struct shared s;
    struct share_arg args[2] = {{&s, 0}, {&s, 1}};
    static const u8 ends[2] = {END, END};
    u8 bytes[SHARED_BYTES];
    struct task *t[2];
    int pid[2], status;

    memset((void *)&s, 0, sizeof(s));
    input_test_flush();
    for (int i = 0; i < 2; i++) {
        t[i] = kthread_create(share_reader, &args[i], "sharer", 0);
        KEXPECT(t[i] != NULL);
        if (!t[i])
            return;
        pid[i] = t[i]->pid;
    }
    for (int i = 0; i < SHARED_BYTES; i++)
        bytes[i] = (u8)i;
    for (int i = 0; i < SHARED_BYTES; i += 20) {
        input_inject(bytes + i, 20);
        sched_yield();
    }
    KEXPECT(wait_count(&s.total, SHARED_BYTES));
    input_inject(ends, 2);
    for (int i = 0; i < 2; i++)
        KEXPECT_EQ(task_wait(pid[i], &status), pid[i]);
    for (int i = 0; i < SHARED_BYTES; i++)
        if (s.seen[i] != 1)
            ktest_fail(__FILE__, __LINE__, "byte %d delivered %u times", i, s.seen[i]);
    KEXPECT_EQ(s.per_reader[0] + s.per_reader[1], SHARED_BYTES);
}
