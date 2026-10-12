// SPDX-License-Identifier: MIT
/*
 * Scheduler. See <sched/sched.h> and docs/design/scheduler.md.
 *
 * Locking: one IRQ-safe lock, sched_lock, protects every task's state, the
 * run queue, the sleep list, wait queues and the task tree. schedule()
 * takes it, and the task that resumes after the switch releases it, so
 * the lock is held across switch_context. Interrupts stay off throughout.
 *
 * Two kinds of switch:
 *   - voluntary (schedule): the task chose to stop. A BLOCKED task stays
 *     off the run queue until something wakes it.
 *   - involuntary (sched_preempt, on interrupt exit): the task is put back
 *     on the run queue *whatever its state*. That is what makes wait loops
 *     safe: a task preempted between marking itself BLOCKED and re-checking
 *     its condition must not sleep, or a wakeup that already happened
 *     would be lost.
 */
#define pr_fmt(fmt) "sched: " fmt

#include <arch/cpu.h>
#include <arch/irqflags.h>
#include <arch/segments.h>
#include <core/irq.h>
#include <core/klog.h>
#include <core/panic.h>
#include <core/preempt.h>
#include <core/spinlock.h>
#include <core/time.h>
#include <lib/errno.h>
#include <lib/string.h>
#include <mm/kmalloc.h>
#include <mm/vmalloc.h>
#include <sched/sched.h>

#include "sched_internal.h"

struct task *switch_context(struct task *prev, struct task *next);
void task_trampoline(void);
void sched_task_start(struct task *last);

static struct spinlock sched_lock = SPINLOCK_INIT("sched");
static struct list_head run_queue = LIST_HEAD_INIT(run_queue);
static struct list_head sleep_list = LIST_HEAD_INIT(sleep_list);
static struct list_head all_tasks = LIST_HEAD_INIT(all_tasks);

static struct task boot_task;
static struct task *idle_task;
static struct task *current_task;
static int next_pid = 2; /* 0: idle, 1: the boot task (kmain) */
static bool running;
static u64 context_switches;
static u32 ntasks;

struct task *task_current(void)
{
    return current_task;
}

bool sched_is_running(void)
{
    return running;
}

static void lock_sched(irqflags_t *flags)
{
    *flags = arch_irq_save();
    spin_lock_raw(&sched_lock);
}

static void unlock_sched(irqflags_t flags)
{
    spin_unlock_raw(&sched_lock);
    arch_irq_restore(flags);
}

/* --- run queue and wakeups (sched_lock held) ------------------------------ */

static void enqueue(struct task *t)
{
    if (t->on_rq || t == idle_task)
        return;
    list_push_back(&run_queue, &t->run_link);
    t->on_rq = true;
}

static struct task *dequeue(void)
{
    if (list_empty(&run_queue))
        return NULL;
    struct task *t = list_first_entry(&run_queue, struct task, run_link);

    list_remove(&t->run_link);
    t->on_rq = false;
    return t;
}

static void make_runnable(struct task *t)
{
    t->state = TASK_RUNNABLE;
    enqueue(t);
}

static void wake_locked(struct task *t)
{
    if (t->waiting_on) {
        list_remove(&t->wait_link);
        t->waiting_on = NULL;
    }
    if (t->sleeping) {
        list_remove(&t->wait_link);
        t->sleeping = false;
    }
    if (t->state == TASK_BLOCKED)
        make_runnable(t);
}

static void wake_queue_locked(struct wait_queue *wq, bool all)
{
    while (!list_empty(&wq->waiters)) {
        wake_locked(list_first_entry(&wq->waiters, struct task, wait_link));
        if (!all)
            break;
    }
}

/* --- task memory ----------------------------------------------------------- */

static void task_free(struct task *t)
{
    sched_arch_task_free(t); /* address space, FPU state */
    vfree(t->kstack);
    kfree(t);
}

/*
 * Builds the frame switch_context pops for a task that has never run:
 *
 *   top - 8    0               fake return address: backtraces stop here
 *   top - 16   0               padding: RSP is 16-aligned after the ret
 *   top - 24   task_trampoline
 *   top - 32   rbp = 0         frame-pointer chain terminator
 *   top - 40   rbx = 0
 *   top - 48   r12 = fn
 *   top - 56   r13 = arg
 *   top - 64   r14 = 0
 *   top - 72   r15 = 0         <- saved_rsp
 */
static void task_setup_stack(struct task *t, int (*fn)(void *), void *arg)
{
    uptr *sp = (uptr *)t->kstack_top;

    *--sp = 0;
    *--sp = 0;
    *--sp = (uptr)task_trampoline;
    *--sp = 0;
    *--sp = 0;
    *--sp = (uptr)fn;
    *--sp = (uptr)arg;
    *--sp = 0;
    *--sp = 0;
    t->saved_rsp = (uptr)sp;
}

static void task_init_common(struct task *t, const char *name)
{
    strlcpy(t->name, name, sizeof(t->name));
    list_init(&t->children);
    list_init(&t->sibling);
    list_init(&t->run_link);
    list_init(&t->wait_link);
    list_init(&t->all_link);
    wait_queue_init(&t->child_exit);
}

struct task *sched_task_alloc(int (*fn)(void *), void *arg, const char *name)
{
    struct task *t = kzalloc(sizeof(*t));

    if (!t)
        return NULL;
    t->kstack = vmalloc(KSTACK_SIZE);
    if (!t->kstack) {
        kfree(t);
        return NULL;
    }
    t->kstack_top = (uptr)t->kstack + KSTACK_SIZE;
    task_init_common(t, name);
    task_setup_stack(t, fn, arg);
    return t;
}

void sched_task_discard(struct task *t)
{
    task_free(t);
}

/* Adds a fully built task to the task tree and makes it runnable. */
void sched_admit(struct task *t, unsigned flags)
{
    irqflags_t irq;

    lock_sched(&irq);
    t->pid = next_pid++;
    t->detached = flags & KTHREAD_DETACHED;
    if (!t->detached) {
        t->parent = current_task;
        list_push_back(&current_task->children, &t->sibling);
    }
    list_push_back(&all_tasks, &t->all_link);
    ntasks++;
    make_runnable(t);
    unlock_sched(irq);
}

struct task *kthread_create(int (*fn)(void *), void *arg, const char *name, unsigned flags)
{
    struct task *t = sched_task_alloc(fn, arg, name);

    if (t)
        sched_admit(t, flags);
    return t;
}

/* --- switching -------------------------------------------------------------- */

/*
 * Runs on the *next* task's stack right after a switch, with sched_lock
 * still held by the switch. `last` is the task switched away from; if it
 * was a detached zombie, nothing references it any more and its stack is
 * no longer in use, so this is the moment it can be freed.
 */
static void finish_switch(struct task *last)
{
    bool reap = last->state == TASK_ZOMBIE && last->detached;

    if (reap) {
        list_remove(&last->all_link);
        ntasks--;
    }
    spin_unlock_raw(&sched_lock);
    if (reap)
        task_free(last);
}

/* First C code of every new task (called from task_trampoline). */
void sched_task_start(struct task *last)
{
    finish_switch(last);
    cpu_irq_enable();
}

static void __schedule(bool preempted)
{
    struct task *prev = current_task;
    irqflags_t flags;

    if (preempt_count != 0)
        panic("schedule() while atomic (preempt_count %u, task %s)", preempt_count, prev->name);
    if (in_irq())
        panic("schedule() called from an IRQ handler");

    lock_sched(&flags);
    need_resched = false;

    if (prev->state == TASK_RUNNING || (preempted && prev->state == TASK_BLOCKED))
        make_runnable(prev);
    else if (prev->state == TASK_RUNNABLE)
        enqueue(prev); /* woken between preparing to wait and getting here */

    struct task *next = dequeue();

    if (!next)
        next = idle_task;
    next->state = TASK_RUNNING;
    next->time_slice = TIME_SLICE_TICK;

    if (next == prev) {
        unlock_sched(flags);
        return;
    }
    context_switches++;
    next->switches_in++;

    sched_arch_switch(prev, next); /* address space, kernel stack, FPU */
    current_task = next;
    struct task *last = switch_context(prev, next);

    /* We are `prev` again, resumed by someone else's switch. */
    finish_switch(last);
    arch_irq_restore(flags);
}

void schedule(void)
{
    __schedule(false);
}

void sched_preempt(void)
{
    __schedule(true);
}

/* Strong definition of the IRQ layer's weak hook: the preemption point. */
void irq_exit_hook(void)
{
    if (need_resched && preempt_count == 0 && running)
        sched_preempt();
}

void sched_yield(void)
{
    schedule(); /* RUNNING -> back of the run queue */
}

/* Timer tick, in IRQ context (interrupts off). */
static void sched_tick(void)
{
    u64 now = time_ticks();

    spin_lock_raw(&sched_lock);
    current_task->runtime_ticks++;

    while (!list_empty(&sleep_list)) {
        struct task *t = list_first_entry(&sleep_list, struct task, wait_link);

        if (t->wake_tick > now)
            break;
        wake_locked(t);
    }

    if (current_task == idle_task) {
        if (!list_empty(&run_queue))
            need_resched = true;
    } else if (current_task->time_slice > 0) {
        current_task->time_slice--;
        if (current_task->time_slice == 0)
            need_resched = true;
    }
    spin_unlock_raw(&sched_lock);
}

/* --- sleeping and waiting --------------------------------------------------- */

void sleep_ticks(u64 n)
{
    u64 deadline = time_ticks() + n;
    struct task *t = current_task;

    if (n == 0) {
        sched_yield();
        return;
    }
    while (time_ticks() < deadline) {
        irqflags_t flags;

        lock_sched(&flags);
        if (!t->sleeping) {
            /* Keep the sleep list sorted by deadline: the tick scans its head only. */
            struct list_head *pos = &sleep_list;

            list_for_each(it, &sleep_list)
            {
                if (list_entry(it, struct task, wait_link)->wake_tick > deadline)
                    break;
                pos = it;
            }
            t->wake_tick = deadline;
            t->sleeping = true;
            list_insert_between(&t->wait_link, pos, pos->next);
        }
        t->state = TASK_BLOCKED;
        unlock_sched(flags);
        schedule();
    }

    irqflags_t flags;

    lock_sched(&flags);
    if (t->sleeping) {
        list_remove(&t->wait_link);
        t->sleeping = false;
    }
    t->state = TASK_RUNNING;
    unlock_sched(flags);
}

void sleep_ms(u64 ms)
{
    sleep_ticks(time_ms_to_ticks(ms));
}

void wait_queue_init(struct wait_queue *wq)
{
    list_init(&wq->waiters);
}

void sched_prepare_wait(struct wait_queue *wq)
{
    struct task *t = current_task;
    irqflags_t flags;

    lock_sched(&flags);
    if (t->waiting_on != wq) {
        BUG_ON(t->waiting_on != NULL || t->sleeping);
        list_push_back(&wq->waiters, &t->wait_link);
        t->waiting_on = wq;
    }
    /* BLOCKED before the caller re-checks its condition: see the file comment. */
    t->state = TASK_BLOCKED;
    unlock_sched(flags);
}

void sched_finish_wait(struct wait_queue *wq)
{
    struct task *t = current_task;
    irqflags_t flags;

    lock_sched(&flags);
    if (t->waiting_on == wq) {
        list_remove(&t->wait_link);
        t->waiting_on = NULL;
    }
    t->state = TASK_RUNNING;
    unlock_sched(flags);
}

void wake_up_one(struct wait_queue *wq)
{
    irqflags_t flags;

    lock_sched(&flags);
    wake_queue_locked(wq, false);
    unlock_sched(flags);
}

void wake_up_all(struct wait_queue *wq)
{
    irqflags_t flags;

    lock_sched(&flags);
    wake_queue_locked(wq, true);
    unlock_sched(flags);
}

/* --- exit and wait ---------------------------------------------------------- */

__noreturn void task_exit(int code)
{
    struct task *t = current_task;
    irqflags_t flags;

    if (t == &boot_task || t == idle_task)
        panic("task %s (pid %d) must never exit", t->name, t->pid);

    lock_sched(&flags);
    t->exit_code = code;

    /* Orphans are adopted by the boot task, which reaps whatever it is given. */
    while (!list_empty(&t->children)) {
        struct task *c = list_first_entry(&t->children, struct task, sibling);

        list_remove(&c->sibling);
        c->parent = &boot_task;
        list_push_back(&boot_task.children, &c->sibling);
        if (c->state == TASK_ZOMBIE)
            wake_queue_locked(&boot_task.child_exit, true);
    }
    t->state = TASK_ZOMBIE;
    if (t->parent)
        wake_queue_locked(&t->parent->child_exit, true);
    spin_unlock_raw(&sched_lock);
    (void)flags; /* interrupts stay off: this task never runs again */

    schedule();
    panic("zombie task %d was scheduled", t->pid);
}

int task_wait(int pid, int *status)
{
    struct task *self = current_task;

    for (;;) {
        irqflags_t flags;
        bool have_child = false;
        struct task *zombie = NULL;

        lock_sched(&flags);
        list_for_each(it, &self->children)
        {
            struct task *c = list_entry(it, struct task, sibling);

            if (pid != -1 && c->pid != pid)
                continue;
            have_child = true;
            if (c->state == TASK_ZOMBIE) {
                zombie = c;
                break;
            }
        }
        if (!have_child) {
            unlock_sched(flags);
            return -ECHILD;
        }
        if (zombie) {
            list_remove(&zombie->sibling);
            list_remove(&zombie->all_link);
            ntasks--;
            unlock_sched(flags);

            int zpid = zombie->pid;

            if (status)
                *status = zombie->exit_code;
            task_free(zombie);
            return zpid;
        }
        /* Block until a child exits. A preemption here just loops again. */
        if (self->waiting_on != &self->child_exit) {
            list_push_back(&self->child_exit.waiters, &self->wait_link);
            self->waiting_on = &self->child_exit;
        }
        self->state = TASK_BLOCKED;
        unlock_sched(flags);
        schedule();

        lock_sched(&flags);
        if (self->waiting_on) {
            list_remove(&self->wait_link);
            self->waiting_on = NULL;
        }
        self->state = TASK_RUNNING;
        unlock_sched(flags);
    }
}

/* --- init ------------------------------------------------------------------- */

static int idle_main(void *arg)
{
    (void)arg;
    /* sti; hlt is atomic with respect to interrupts: no wakeup is missed. */
    for (;;)
        __asm__ volatile("sti; hlt" ::: "memory");
    return 0;
}

void sched_init(void)
{
    task_init_common(&boot_task, "kmain");
    boot_task.pid = 1;
    boot_task.state = TASK_RUNNING;
    list_push_back(&all_tasks, &boot_task.all_link);
    current_task = &boot_task;

    idle_task = sched_task_alloc(idle_main, NULL, "idle");
    if (!idle_task)
        panic("sched: cannot allocate the idle task");
    idle_task->pid = 0;
    idle_task->state = TASK_RUNNABLE; /* never on the run queue: picked when it is empty */

    ntasks = 2;
    time_set_tick_hook(sched_tick);
    running = true;
    pr_info("round robin, %u ms slices, %lu KiB kernel stacks\n", TIME_SLICE_TICK,
            KSTACK_SIZE / 1024);
}

void sched_get_stats(struct sched_stats *out)
{
    irqflags_t flags;
    u32 runnable = 0;

    lock_sched(&flags);
    list_for_each(it, &run_queue) runnable++;
    out->context_switches = context_switches;
    out->tasks = ntasks;
    out->runnable = runnable;
    unlock_sched(flags);
}
