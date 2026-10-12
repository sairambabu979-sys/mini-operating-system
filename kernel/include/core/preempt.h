/* SPDX-License-Identifier: MIT */
/*
 * Preemption control.
 *
 * preempt_count > 0 means the current context must not be switched away
 * from involuntarily: it holds a spinlock or explicitly disabled
 * preemption. The timer only preempts at count 0. (Per CPU; this kernel
 * runs on one.)
 */
#ifndef MINIOS_CORE_PREEMPT_H
#define MINIOS_CORE_PREEMPT_H

#include <compiler.h>
#include <types.h>

extern volatile u32 preempt_count;

static __always_inline void preempt_disable(void)
{
    preempt_count++;
    barrier();
}

/* Re-enables preemption; reschedules now if the timer asked for it. */
void preempt_enable(void);

/* Like preempt_enable(), but never reschedules (used while switching). */
static __always_inline void preempt_enable_no_resched(void)
{
    barrier();
    preempt_count--;
}

#endif /* MINIOS_CORE_PREEMPT_H */
