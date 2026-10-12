// SPDX-License-Identifier: MIT
/*
 * Scheduler tests. They run in the boot task ("kmain") with preemption
 * live, so each one also stresses the code it is not directly about.
 */
#include <arch/setjmp.h>
#include <core/ktest.h>
#include <core/panic.h>
#include <core/preempt.h>
#include <core/time.h>
#include <lib/errno.h>
#include <mm/kmalloc.h>
#include <mm/pmm.h>
#include <sched/mutex.h>
#include <sched/sched.h>

static int return_arg(void *arg)
{
    return (int)(uptr)arg;
}

KTEST(kthread_runs_and_is_reaped_with_its_exit_code)
{
    struct task *t = kthread_create(return_arg, (void *)7, "ret7", 0);
    int status = -1;

    KEXPECT(t != NULL);
    int pid = t->pid;

    KEXPECT_EQ(task_wait(pid, &status), pid);
    KEXPECT_EQ(status, 7);
    KEXPECT_EQ(task_wait(pid, &status), -ECHILD); /* already reaped */
}

KTEST(wait_without_children_fails)
{
    KEXPECT_EQ(task_wait(-1, NULL), -ECHILD);
}

static int exit_directly(void *arg)
{
    task_exit((int)(uptr)arg + 100);
}

KTEST(task_exit_from_deep_inside_a_thread)
{
    struct task *t = kthread_create(exit_directly, (void *)5, "exit105", 0);
    int status = 0;
    int pid = t->pid; /* t is freed by task_wait: never touch it afterwards */

    KEXPECT_EQ(task_wait(pid, &status), pid);
    KEXPECT_EQ(status, 105);
}

struct spin_arg {
    volatile bool *stop;
    volatile u64 count;
};

/* Never yields, never blocks: only the timer can take the CPU away. */
static int spinner(void *opaque)
{
    struct spin_arg *a = opaque;

    while (!*a->stop)
        a->count++;
    return 0;
}

/*
 * Busy threads that never yield must all make progress, and the boot task
 * must get the CPU back from them to stop the test at all: both are only
 * possible with working preemption.
 */
KTEST(cpu_bound_threads_are_preempted_fairly)
{
    enum { N = 4 };
    volatile bool stop = false;
    struct spin_arg args[N];
    u64 total = 0;

    for (int i = 0; i < N; i++) {
        args[i] = (struct spin_arg){.stop = &stop, .count = 0};
        KEXPECT(kthread_create(spinner, &args[i], "spinner", 0) != NULL);
    }
    sleep_ms(400); /* ~40 slices each */
    stop = true;
    for (int i = 0; i < N; i++)
        KEXPECT(task_wait(-1, NULL) > 0);

    for (int i = 0; i < N; i++)
        total += args[i].count;
    u64 mean = total / N;

    for (int i = 0; i < N; i++) {
        KEXPECT(args[i].count > 0);
        /* Round robin with equal slices: every share within 25% of the mean. */
        KEXPECT(args[i].count * 100 >= mean * 75);
        KEXPECT(args[i].count * 100 <= mean * 125);
    }
}

KTEST(sleep_lasts_at_least_its_duration)
{
    u64 t0 = time_ticks();

    sleep_ms(30);
    u64 elapsed = time_ticks() - t0;

    KEXPECT(elapsed >= 30);
    KEXPECT(elapsed < 30 + 50); /* generous: emulator timing */
}

struct order_log {
    volatile int next;
    volatile int ids[3];
};

struct sleeper_arg {
    struct order_log *log;
    int id;
    u64 ms;
};

static int sleeper(void *opaque)
{
    struct sleeper_arg *a = opaque;

    sleep_ms(a->ms);
    a->log->ids[__atomic_fetch_add(&a->log->next, 1, __ATOMIC_RELAXED)] = a->id;
    return 0;
}

KTEST(sleepers_wake_in_deadline_order)
{
    struct order_log log = {0};
    struct sleeper_arg args[3] = {{&log, 3, 60}, {&log, 1, 20}, {&log, 2, 40}};

    for (int i = 0; i < 3; i++)
        KEXPECT(kthread_create(sleeper, &args[i], "sleeper", 0) != NULL);
    for (int i = 0; i < 3; i++)
        KEXPECT(task_wait(-1, NULL) > 0);
    KEXPECT_EQ(log.ids[0], 1);
    KEXPECT_EQ(log.ids[1], 2);
    KEXPECT_EQ(log.ids[2], 3);
}

/* Bounded buffer: producer and consumer block on each other constantly. */
struct ring {
    u32 buf[4];
    volatile u32 head, tail; /* head - tail = items queued */
    struct wait_queue not_full, not_empty;
    u64 sum;
};

enum { RING_ITEMS = 3000 };

static int producer(void *opaque)
{
    struct ring *r = opaque;

    for (u32 i = 1; i <= RING_ITEMS; i++) {
        wait_event(&r->not_full, r->head - r->tail < 4);
        r->buf[r->head % 4] = i;
        __atomic_add_fetch(&r->head, 1, __ATOMIC_RELEASE);
        wake_up_one(&r->not_empty);
    }
    return 0;
}

static int consumer(void *opaque)
{
    struct ring *r = opaque;

    for (u32 i = 0; i < RING_ITEMS; i++) {
        wait_event(&r->not_empty, r->head != r->tail);
        r->sum += r->buf[r->tail % 4];
        __atomic_add_fetch(&r->tail, 1, __ATOMIC_RELEASE);
        wake_up_one(&r->not_full);
    }
    return 0;
}

/*
 * A lost wakeup leaves one side blocked forever and the test hangs (the
 * QEMU runner's timeout turns that into a failure). Thousands of handoffs,
 * with preemption landing at random points inside wait_event, make the
 * race windows real.
 */
KTEST(wait_queues_never_lose_wakeups)
{
    struct ring r = {.head = 0, .tail = 0, .sum = 0};

    wait_queue_init(&r.not_full);
    wait_queue_init(&r.not_empty);
    KEXPECT(kthread_create(consumer, &r, "consumer", 0) != NULL);
    KEXPECT(kthread_create(producer, &r, "producer", 0) != NULL);
    KEXPECT(task_wait(-1, NULL) > 0);
    KEXPECT(task_wait(-1, NULL) > 0);
    KEXPECT_EQ(r.sum, (u64)RING_ITEMS * (RING_ITEMS + 1) / 2);
}

struct counter {
    struct mutex lock;
    volatile u64 value;
};

/* Read-yield-write: without mutual exclusion, updates would be lost. */
static int locked_incrementer(void *opaque)
{
    struct counter *c = opaque;

    for (int i = 0; i < 300; i++) {
        mutex_lock(&c->lock);
        u64 v = c->value;

        sched_yield(); /* hand the CPU to a competitor mid-update */
        c->value = v + 1;
        mutex_unlock(&c->lock);
    }
    return 0;
}

KTEST(mutex_provides_mutual_exclusion)
{
    struct counter c = {.value = 0};

    mutex_init(&c.lock, "test counter");
    for (int i = 0; i < 4; i++)
        KEXPECT(kthread_create(locked_incrementer, &c, "incr", 0) != NULL);
    for (int i = 0; i < 4; i++)
        KEXPECT(task_wait(-1, NULL) > 0);
    KEXPECT_EQ(c.value, 1200);
}

KTEST(mutex_misuse_is_detected)
{
    struct mutex m;
    struct kjmp_buf jb;
    struct panic_expectation e;

    mutex_init(&m, "misuse");
    panic_expect(&e, &jb);
    if (ksetjmp(&jb) == 0)
        mutex_unlock(&m); /* not the owner */
    panic_expect_clear();
    KEXPECT(e.hit);

    mutex_lock(&m);
    panic_expect(&e, &jb);
    if (ksetjmp(&jb) == 0)
        mutex_lock(&m); /* recursive */
    panic_expect_clear();
    KEXPECT(e.hit);
    mutex_unlock(&m);
}

static int long_sleeper(void *arg)
{
    (void)arg;
    sleep_ms(30);
    return 33;
}

static int abandon_child(void *arg)
{
    struct task **child = (struct task **)arg;

    *child = kthread_create(long_sleeper, NULL, "orphan", 0);
    return 0; /* exits while its child still sleeps */
}

KTEST(orphans_are_adopted_by_kmain)
{
    struct task *orphan = NULL;
    struct task *parent = kthread_create(abandon_child, (void *)&orphan, "parent", 0);
    int status = 0;
    int ppid = parent->pid;

    KEXPECT_EQ(task_wait(ppid, &status), ppid);
    KEXPECT(orphan != NULL);
    int opid = orphan->pid;

    /* The orphan is now our child, and we can reap it. */
    KEXPECT_EQ(task_wait(opid, &status), opid);
    KEXPECT_EQ(status, 33);
}

KTEST(schedule_while_atomic_is_caught)
{
    struct kjmp_buf jb;
    struct panic_expectation e;

    preempt_disable();
    panic_expect(&e, &jb);
    if (ksetjmp(&jb) == 0)
        sched_yield();
    panic_expect_clear();
    preempt_enable_no_resched();
    KEXPECT(e.hit);
}

static int nop_thread(void *arg)
{
    (void)arg;
    return 0;
}

/*
 * Creating and reaping hundreds of threads must give everything back.
 * A round first warms the slab caches; the slack covers page tables for
 * the vmalloc VA the stacks advance through (VA is never reused).
 */
KTEST(thread_churn_returns_all_memory)
{
    struct pmm_stats p0, p1;
    struct kmalloc_stats k0, k1;

    for (int round = 0; round < 2; round++) {
        if (round == 1) {
            pmm_get_stats(&p0);
            kmalloc_get_stats(&k0);
        }
        for (int i = 0; i < 200; i++) {
            struct task *t = kthread_create(nop_thread, NULL, "churn", 0);

            KEXPECT(t != NULL);
            if (i % 8 == 7) /* keep a few alive at once */
                while (task_wait(-1, NULL) > 0)
                    ;
        }
        while (task_wait(-1, NULL) > 0)
            ;
    }
    pmm_get_stats(&p1);
    kmalloc_get_stats(&k1);
    KEXPECT_EQ(k1.small_live, k0.small_live);
    KEXPECT(p1.free_frames + 8 >= p0.free_frames);
}

static volatile int detached_ran;

static int detached_body(void *arg)
{
    (void)arg;
    detached_ran = 1;
    return 0;
}

KTEST(detached_threads_free_themselves)
{
    struct kmalloc_stats k0, k1;
    struct sched_stats s0, s1;

    kmalloc_get_stats(&k0);
    sched_get_stats(&s0);
    detached_ran = 0;
    KEXPECT(kthread_create(detached_body, NULL, "detached", KTHREAD_DETACHED) != NULL);
    sleep_ms(20);
    KEXPECT_EQ(detached_ran, 1);
    KEXPECT_EQ(task_wait(-1, NULL), -ECHILD); /* not our child */
    sleep_ms(5);                              /* the next switch reaps it */
    kmalloc_get_stats(&k1);
    sched_get_stats(&s1);
    KEXPECT_EQ(k1.small_live, k0.small_live);
    KEXPECT_EQ(s1.tasks, s0.tasks);
}
