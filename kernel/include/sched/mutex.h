/* SPDX-License-Identifier: MIT */
/*
 * Sleeping mutex: for long critical sections in task context. Waiters
 * block instead of spinning. Not usable from IRQ handlers. Non-recursive;
 * only the owner may unlock (both are checked).
 */
#ifndef MINIOS_SCHED_MUTEX_H
#define MINIOS_SCHED_MUTEX_H

#include <sched/sched.h>

struct mutex {
    struct task *owner;
    struct wait_queue waiters;
    const char *name;
};

#define MUTEX_INIT(m, n)                                                                           \
    {                                                                                              \
        .owner = NULL, .waiters = WAIT_QUEUE_INIT((m).waiters), .name = (n)                        \
    }

void mutex_init(struct mutex *m, const char *name);
void mutex_lock(struct mutex *m);
bool mutex_trylock(struct mutex *m);
void mutex_unlock(struct mutex *m);

#endif /* MINIOS_SCHED_MUTEX_H */
