// SPDX-License-Identifier: MIT
#include <arch/irqflags.h>
#include <core/irq.h>
#include <core/panic.h>
#include <core/preempt.h>
#include <core/spinlock.h>
#include <sched/sched.h>

volatile u32 preempt_count;
volatile bool need_resched;

void preempt_check_resched(void)
{
    if (need_resched && preempt_count == 0 && arch_irqs_enabled() && !in_irq() &&
        sched_is_running())
        sched_preempt();
}

void preempt_enable(void)
{
    barrier();
    preempt_count--;
    preempt_check_resched();
}

__noreturn void spinlock_recursion(const struct spinlock *lock)
{
    panic("spinlock '%s': recursive acquisition (would deadlock)", lock->name);
}
