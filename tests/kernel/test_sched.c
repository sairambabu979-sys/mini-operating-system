// SPDX-License-Identifier: MIT
/*
 * Scheduler tests. They run in the boot task ("kmain") with preemption
 * live, so each one also stresses the code it is not directly about.
 */
#include <arch/irqflags.h>
#include <arch/lapic.h>
#include <arch/setjmp.h>
#include <arch/trap.h>
#include <core/irq.h>
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

static bool all_zombies(struct task *const *ts, int n)
{
    bool all = true;
    irqflags_t flags = arch_irq_save();

    for (int i = 0; i < n; i++)
        all &= ts[i]->state == TASK_ZOMBIE;
    arch_irq_restore(flags);
    return all;
}

/*
 * Busy threads that never yield must all make progress, and the boot task
 * must get the CPU back from them to stop the test at all: both are only
 * possible with working preemption.
 *
 * Fairness is judged in ticks of CPU time, which is what round robin
 * divides: every slice is exactly TIME_SLICE_TICK ticks, so no thread can
 * be more than one slice behind another (plus a tick caught while one
 * runs to exit). The threads are created with preemption off, so the
 * first gets no head start. Loop counts are no measure of it
 * under emulation: iterations per tick follow the host, and a +-25% bound
 * on them failed once on a cold 4 GiB run (host faulting in guest RAM)
 * while instrumented runs showed the ticks exactly equal.
 */
KTEST(cpu_bound_threads_are_preempted_fairly)
{
    enum { N = 4 };
    volatile bool stop = false;
    struct spin_arg args[N];
    struct task *ts[N];
    u64 deadline = time_ticks() + 2000;

    /* All queued before any runs: none gets a head start. */
    preempt_disable();
    for (int i = 0; i < N; i++) {
        args[i] = (struct spin_arg){.stop = &stop, .count = 0};
        ts[i] = kthread_create(spinner, &args[i], "spinner", 0);
        KEXPECT(ts[i] != NULL);
        if (!ts[i]) {
            preempt_enable();
            stop = true; /* `stop` lives in this frame: reap the others before leaving */
            for (int j = 0; j < i; j++)
                KEXPECT(task_wait(-1, NULL) > 0);
            return;
        }
    }
    preempt_enable();
    sleep_ms(400); /* ~10 slices each */
    stop = true;
    while (!all_zombies(ts, N) && time_ticks() < deadline)
        sleep_ms(1);
    KEXPECT(all_zombies(ts, N));

    u64 lo = ~0ull, hi = 0;

    for (int i = 0; i < N; i++) {
        KEXPECT(args[i].count > 0);
        lo = MIN(lo, ts[i]->runtime_ticks);
        hi = MAX(hi, ts[i]->runtime_ticks);
    }
    KEXPECT(lo >= 5ull * TIME_SLICE_TICK); /* each had several slices of the 400 ms */
    /* Stopped mid-round: one slice apart at most, plus a tick in each exit run. */
    KEXPECT(hi - lo <= TIME_SLICE_TICK + 2);
    for (int i = 0; i < N; i++)
        KEXPECT(task_wait(-1, NULL) > 0);
}

/*
 * A sleep of n ms must span n whole tick periods, as a driver waiting out a
 * hardware settle time relies on. The current tick is already partly over,
 * so that takes n + 1 tick deliveries; waiting for only n let sleep_ms(1)
 * return 130 us after it was called, if a tick edge followed at once.
 *
 * Counted in ticks, not TSC time: under emulation a tick due just before
 * the call can be delivered late, just after it, so even a correct sleep
 * can be short in host time (this test, written first against the TSC,
 * failed on a loaded host). On hardware, interrupt latency is microseconds.
 */
KTEST(short_sleeps_span_whole_tick_periods)
{
    for (int i = 0; i < 20; i++) {
        tsc_delay_us(37 * (u64)i % 1000); /* start at varied points within a tick */
        u64 t0 = time_ticks();

        sleep_ms(1);
        KEXPECT(time_ticks() - t0 >= 2);
    }
}

/*
 * A wakeup from interrupt context can find its target still on the CPU:
 * between sched_prepare_wait() (state BLOCKED) and the condition check.
 * Here the condition itself raises the interrupt. The waiter must not end
 * up on the run queue while it runs: when it then exits, a stale entry is
 * a zombie scheduled again (panic) or a freed task still queued.
 */
static struct wait_queue irq_wq = WAIT_QUEUE_INIT(irq_wq);
static volatile bool irq_woke, ipi_sent;

static void waking_irq(struct trap_frame *frame, void *ctx)
{
    (void)frame;
    (void)ctx;
    irq_woke = true;
    wake_up_one(&irq_wq);
}

/* The wait condition: on first evaluation, interrupt ourselves; the handler wakes us. */
static bool ipi_then_check(void)
{
    if (!ipi_sent) {
        ipi_sent = true;
        lapic_send_self_ipi(VEC_APIC_TEST); /* interrupts are on: delivered here */
    }
    return irq_woke;
}

static int irq_woken_waiter(void *arg)
{
    (void)arg;
    wait_event(&irq_wq, ipi_then_check());
    KEXPECT(!task_current()->on_rq); /* running, so not queued */
    return 7;
}

KTEST(irq_wakeup_of_a_running_waiter_does_not_queue_it)
{
    if (irq_mode() != IRQ_MODE_APIC)
        KTEST_SKIP("needs a local APIC self-IPI");

    int status = -1;

    irq_woke = ipi_sent = false;
    KEXPECT_EQ(irq_register_local(VEC_APIC_TEST, waking_irq, NULL), 0);
    for (int i = 0; i < 4; i++) { /* the stale entry bites only when it heads the queue */
        irq_woke = ipi_sent = false;
        struct task *t = kthread_create(irq_woken_waiter, NULL, "irq-waiter", 0);

        KEXPECT(t != NULL);
        if (!t)
            break;
        int pid = t->pid;

        KEXPECT_EQ(task_wait(pid, &status), pid);
        KEXPECT_EQ(status, 7);
    }
    irq_unregister_local(VEC_APIC_TEST);
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
 * Creating and reaping hundreds of threads must give everything back,
 * to the frame. A round first warms the slab caches and fills vmalloc's
 * reuse ring; after that, kernel stacks recycle VA and need no new page
 * tables (fresh VA leaked one frame per ~100 tasks).
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
    KEXPECT_EQ(p1.free_frames, p0.free_frames);
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
