// SPDX-License-Identifier: MIT
/*
 * Architecture side of a context switch.
 */
#include <arch/segments.h>

#include "sched_internal.h"

void sched_arch_switch(struct task *prev, struct task *next)
{
    (void)prev;
    /* Ring 3 -> ring 0 transitions land on the incoming task's stack. */
    if (next->kstack_top)
        tss_set_kernel_stack(next->kstack_top);
}

void sched_arch_task_free(struct task *t)
{
    (void)t;
}
