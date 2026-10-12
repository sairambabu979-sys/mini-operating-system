// SPDX-License-Identifier: MIT
#include <core/panic.h>
#include <core/preempt.h>
#include <core/spinlock.h>

volatile u32 preempt_count;

void preempt_enable(void)
{
    barrier();
    preempt_count--;
}

__noreturn void spinlock_recursion(const struct spinlock *lock)
{
    panic("spinlock '%s': recursive acquisition (would deadlock)", lock->name);
}
