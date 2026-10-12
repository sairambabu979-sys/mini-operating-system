/* SPDX-License-Identifier: MIT */
/*
 * Tasks and the scheduler.
 *
 * Policy: preemptive round robin with a 10-tick (10 ms) time slice. The
 * timer never switches tasks itself; it sets need_resched, and the switch
 * happens on interrupt exit (after EOI) or when preemption is re-enabled.
 *
 * Task lifecycle:
 *
 *   created -> RUNNABLE <-> RUNNING -> ZOMBIE -> reaped (task_wait)
 *                  ^           |
 *                  +- BLOCKED <+   (sleep, wait queue)
 *
 * A task never frees its own kernel stack: a zombie's resources are freed
 * by its parent in task_wait(), or, for detached threads, by whichever
 * task runs next.
 */
#ifndef MINIOS_SCHED_SCHED_H
#define MINIOS_SCHED_SCHED_H

#include <compiler.h>
#include <lib/list.h>
#include <types.h>

#define TASK_NAME_MAX   16
#define KSTACK_SIZE     (16ul * 1024)
#define TIME_SLICE_TICK 10

enum task_state {
    TASK_RUNNING,  /* on the CPU */
    TASK_RUNNABLE, /* on the run queue */
    TASK_BLOCKED,  /* sleeping or on a wait queue */
    TASK_ZOMBIE,   /* exited, waiting to be reaped */
};

struct wait_queue {
    struct list_head waiters;
};

#define WAIT_QUEUE_INIT(name)                                                                      \
    {                                                                                              \
        .waiters = LIST_HEAD_INIT((name).waiters)                                                  \
    }

struct address_space;

struct task {
    uptr saved_rsp; /* switch.asm: must stay first (asm-offsets checks) */
    void *kstack;   /* vmalloc base; NULL for the boot task */
    uptr kstack_top;
    struct address_space *as; /* NULL: kernel thread */
    void *fpu_state;          /* FXSAVE area for user tasks */

    enum task_state state;
    int pid;
    char name[TASK_NAME_MAX];
    int exit_code;
    bool detached; /* freed automatically on exit; cannot be waited for */

    struct task *parent;
    struct list_head children;
    struct list_head sibling;
    struct wait_queue child_exit; /* this task waits here for children */

    struct list_head run_link; /* run queue */
    bool on_rq;
    struct list_head wait_link; /* wait queue or sleep list */
    struct wait_queue *waiting_on;
    u64 wake_tick;
    bool sleeping;
    struct list_head all_link;

    u32 time_slice;
    u64 runtime_ticks;
    u64 switches_in;
};

static_assert(__builtin_offsetof(struct task, saved_rsp) == 0, "switch.asm assumes offset 0");

/* Turns the boot context into the first task ("kmain") and starts the timer hook. */
void sched_init(void);
bool sched_is_running(void);

struct task *task_current(void);

enum {
    KTHREAD_DETACHED = 1u << 0,
};

/* Creates a runnable kernel thread running fn(arg). NULL on out-of-memory. */
__must_check struct task *kthread_create(int (*fn)(void *), void *arg, const char *name,
                                         unsigned flags);

/*
 * Waits for a child to exit (pid, or -1 for any), reaps it, stores its
 * exit code in *status, and returns its pid. -ECHILD if there is no such
 * child.
 */
int task_wait(int pid, int *status);

__noreturn void task_exit(int code);

void sched_yield(void);
void sleep_ticks(u64 ticks);
void sleep_ms(u64 ms);

/* The core: picks the next task and switches to it. Not for IRQ handlers. */
void schedule(void);

/* Involuntary switch (time slice over): the task stays runnable. */
void sched_preempt(void);

/* Wait queues. wait_event() re-checks `cond` after every wakeup. */
void wait_queue_init(struct wait_queue *wq);
void wake_up_one(struct wait_queue *wq);
void wake_up_all(struct wait_queue *wq);

/* Internal halves of wait_event(); see sched.c for the lost-wakeup argument. */
void sched_prepare_wait(struct wait_queue *wq);
void sched_finish_wait(struct wait_queue *wq);

#define wait_event(wq, cond)                                                                       \
    do {                                                                                           \
        for (;;) {                                                                                 \
            sched_prepare_wait(wq);                                                                \
            if (cond)                                                                              \
                break;                                                                             \
            schedule();                                                                            \
        }                                                                                          \
        sched_finish_wait(wq);                                                                     \
    } while (0)

struct sched_stats {
    u64 context_switches;
    u32 tasks;
    u32 runnable;
};

void sched_get_stats(struct sched_stats *out);

#endif /* MINIOS_SCHED_SCHED_H */
