/* SPDX-License-Identifier: MIT */
/*
 * Scheduler internals shared with the process layer. Not a public API.
 */
#ifndef MINIOS_SCHED_INTERNAL_H
#define MINIOS_SCHED_INTERNAL_H

#include <sched/sched.h>

/* Allocates a task with a kernel stack, ready to run fn(arg); not yet admitted. */
struct task *sched_task_alloc(int (*fn)(void *), void *arg, const char *name);

/* Frees a task that was allocated but never admitted. */
void sched_task_discard(struct task *t);

/* Inserts a built task into the tree and run queue (assigns its pid). */
void sched_admit(struct task *t, unsigned flags);

/* Per-switch architecture work: kernel stack for ring transitions, CR3, FPU. */
void sched_arch_switch(struct task *prev, struct task *next);

/* Frees what sched_arch_* attached to a task (address space, FPU area). */
void sched_arch_task_free(struct task *t);

#endif /* MINIOS_SCHED_INTERNAL_H */
