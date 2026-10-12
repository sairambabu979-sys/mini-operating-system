/* SPDX-License-Identifier: MIT */
/*
 * Spinlocks.
 *
 * Always IRQ-saving: any lock may be shared with an interrupt handler, and
 * a handler that interrupts the holder of the lock it wants would spin
 * forever. Taking a lock also disables preemption.
 *
 * On this single-CPU kernel a held lock can only be contended by its own
 * holder, so contention is reported as a recursive-locking bug instead of
 * spinning forever. The atomic exchange keeps the code correct for SMP.
 */
#ifndef MINIOS_CORE_SPINLOCK_H
#define MINIOS_CORE_SPINLOCK_H

#include <arch/irqflags.h>
#include <compiler.h>
#include <core/preempt.h>
#include <types.h>

struct spinlock {
    volatile u32 locked;
    const char *name;
};

#define SPINLOCK_INIT(lock_name)                                                                   \
    {                                                                                              \
        .locked = 0, .name = (lock_name)                                                           \
    }

__noreturn void spinlock_recursion(const struct spinlock *lock);

static inline __must_check irqflags_t spin_lock_irqsave(struct spinlock *lock)
{
    irqflags_t flags = arch_irq_save();

    preempt_disable();
    if (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE) != 0)
        spinlock_recursion(lock);
    return flags;
}

static inline void spin_unlock_irqrestore(struct spinlock *lock, irqflags_t flags)
{
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
    preempt_enable();
    arch_irq_restore(flags);
}

static inline bool spin_is_locked(const struct spinlock *lock)
{
    return __atomic_load_n(&lock->locked, __ATOMIC_RELAXED) != 0;
}

#endif /* MINIOS_CORE_SPINLOCK_H */
