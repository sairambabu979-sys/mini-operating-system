// SPDX-License-Identifier: MIT
#include <core/irq.h>
#include <core/panic.h>
#include <sched/mutex.h>

void mutex_init(struct mutex *m, const char *name)
{
    m->owner = NULL;
    wait_queue_init(&m->waiters);
    m->name = name;
}

bool mutex_trylock(struct mutex *m)
{
    struct task *expected = NULL;

    return __atomic_compare_exchange_n(&m->owner, &expected, task_current(), false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void mutex_lock(struct mutex *m)
{
    if (in_irq())
        panic("mutex '%s': lock from an IRQ handler", m->name);
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == task_current())
        panic("mutex '%s': recursive lock by %s", m->name, task_current()->name);
    wait_event(&m->waiters, mutex_trylock(m));
}

void mutex_unlock(struct mutex *m)
{
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) != task_current())
        panic("mutex '%s': unlocked by %s, which does not own it", m->name, task_current()->name);
    __atomic_store_n(&m->owner, NULL, __ATOMIC_RELEASE);
    /* One waiter suffices: if someone else grabs the lock first, it re-blocks, and is
     * woken again by that owner's unlock. */
    wake_up_one(&m->waiters);
}
