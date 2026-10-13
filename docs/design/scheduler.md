# Scheduler

Sources: `kernel/sched/`, `kernel/arch/x86_64/switch.asm`,
`kernel/include/sched/`.

## Policy

Preemptive **round robin**, 10 ms time slices (10 ticks at 1000 Hz), one
FIFO run queue. An idle task (`sti; hlt`) runs when the queue is empty and
is never queued itself. The boot context becomes task 1, `kmain`, which
also adopts orphans; idle is task 0.

## Task lifecycle

```
 kthread_create ──► RUNNABLE ◄──────► RUNNING ──task_exit──► ZOMBIE ──task_wait──► freed
                       ▲                 │
                       └──── BLOCKED ◄───┘   sleep_ms, wait_event, task_wait
```

A task never frees its own kernel stack, since it is running on it:
- **Joinable** tasks are reaped by their parent in `task_wait()`, which also
  delivers the exit code.
- **Detached** tasks (`KTHREAD_DETACHED`) are freed by whichever task runs
  next (see below).

A process's address space and FPU area go back at exit, not at reaping.
`task_exit()` first calls `sched_arch_task_exit()`, which detaches both,
moves the task onto the kernel's page tables (interrupts off for that
step, so a preemption can only switch it back in on the kernel's tables),
and then frees both. A zombie holds only its task struct and its 16 KiB
kernel stack, so a parent that never waits pins little.

Orphans are re-parented to `kmain`.

**At most `TASK_MAX` (256) tasks.** The count (`sched_stats.allocated`)
runs from allocation to the final free, so it includes zombies, tasks
being built or freed, and the boot and idle tasks. Creation beyond it
fails: `kthread_create()` returns NULL and `spawn` returns `-EAGAIN`. A
spawn loop in user mode therefore cannot take every frame for kernel
stacks and address spaces. A build-time check keeps `TASK_MAX` plus
vmalloc's quarantine within its reuse ring, so kernel stacks recycle
freed VA instead of growing the page tables (see [memory.md](memory.md)).

## The context switch

```nasm
switch_context:            ; rdi = prev, rsi = next
    push rbp, rbx, r12, r13, r14, r15
    mov  [rdi + saved_rsp], rsp
    mov  rsp, [rsi + saved_rsp]
    mov  rax, rdi          ; "who ran before me", for the resumed task
    pop  r15, r14, r13, r12, rbx, rbp
    ret
```

Only callee-saved registers are saved: `switch_context` is an ordinary C
call, so the compiler already treats the others as clobbered. The return
value matters. The task that resumes learns which task it replaced, and
that is the first moment a detached zombie's stack is provably unused,
so `finish_switch()` frees it there.

A new task's stack is built so that the first `ret` lands in
`task_trampoline` with RSP 16-byte aligned, the entry function in R12, its
argument in R13, and RBP = 0, which terminates panic backtraces.

## Locking across the switch

One IRQ-safe lock, `sched_lock`, protects every task's state, the run
queue, the sleep list, the wait queues and the task tree. `schedule()`
takes it, and the task that resumes *after* the switch releases it
(`finish_switch`), so the lock is held across `switch_context` with
interrupts off. `preempt_count` (per CPU) passes from one task to the next
the same way.

## Preemption

The timer tick only sets `need_resched` when the slice runs out. The
switch itself happens in two places:
- **interrupt exit**: `irq_exit_hook()`, after the PIC has been sent EOI;
- **re-enabling preemption** (`spin_unlock_irqrestore`, `preempt_enable`),
  so a slice that expired while a lock was held is honoured as soon as it
  is released.

In both cases only when `preempt_count == 0`.

`schedule()` panics if called while atomic (`preempt_count > 0`) or from
an IRQ handler. Both are bugs that would otherwise hang the machine
somewhere else entirely.

## Never losing a wakeup

`wait_event(wq, cond)` is:

```c
for (;;) {
    sched_prepare_wait(wq);  /* join wq, state = BLOCKED */
    if (cond) break;         /* re-check AFTER becoming BLOCKED */
    schedule();
}
sched_finish_wait(wq);       /* leave wq, state = RUNNING */
```

A waker that runs after `prepare_wait` finds the task on the queue and
makes it runnable, so the wakeup cannot fall in the gap.

**A wakeup can find its target still running.** An interrupt that arrives
between `prepare_wait` and `schedule()` may wake the very task it
interrupted. That task is still on the CPU, so `wake_locked()` only makes
it RUNNING again and does not queue it. Queueing it would leave a stale
run-queue entry once it carried on, and if it then exited, the zombie
would be scheduled (or a freed task left queued). If the task still calls
`schedule()`, a RUNNING task there is simply a yield. `task_exit()` checks
the rule: an exiting task found on the run queue is a `BUG_ON`.

**The subtle case is preemption.** If the timer preempts a task between
`prepare_wait` and `schedule()`, the task is BLOCKED. A naive scheduler
would leave it off the run queue, and if the wakeup had already happened
(the condition is true), nothing would ever wake it. So two paths:

- `schedule()` (voluntary) leaves a BLOCKED task off the run queue.
- `sched_preempt()` (involuntary) puts the task back on the run queue
  **whatever its state**. The worst outcome is one extra loop iteration.

`sleep_ms()` loops on its deadline for the same reason: a preemption can
make `schedule()` return early, but never the sleep. The sleep list is
sorted by deadline, so the tick only ever inspects its head.

**Sleeps are a minimum.** `sleep_ms(ms)` waits for `ms` rounded up to whole
ticks, plus one. The current tick is already partly over when the sleep
starts, so `n` tick deliveries can come just over `n - 1` tick periods
later: without the extra tick, `sleep_ms(1)` returned as little as 130 µs
after it was called. (`sleep_ticks(n)` itself counts tick deliveries and
adds nothing.)

## Sleeping mutex

`struct mutex`: blocking, non-recursive, owner-checked. Lock is
`wait_event(&m->waiters, compare_exchange(owner, NULL, current))`; unlock
clears the owner and wakes one waiter. If a running task grabs the lock
first, the woken waiter just blocks again and is woken by that owner's
unlock. Recursive locking, unlocking by a non-owner, and use from IRQ
context all panic.

## Tests (`tests/kernel/test_sched.c`)

| Test | What it proves |
|------|----------------|
| exit code through `task_wait`; `-ECHILD` | lifecycle and reaping |
| four CPU-bound threads that never yield all make progress, and their CPU time in ticks is within one slice of each other | preemption works, and round robin is fair (loop counts are no measure under emulation: they follow the host) |
| `sleep_ms(30)` lasts ≥ 30 ticks (and, loosely, < 80); `sleep_ms(1)`, started at 20 points within a tick, always spans ≥ 2 tick deliveries; sleepers wake in deadline order | sleep list, sleep rounding |
| a waiter whose wait condition sends a self-IPI, so the interrupt handler wakes it while it is still running, 4 times; the waiter checks it is not queued, then exits | the `wake_locked()` rule (needs a local APIC) |
| producer/consumer through a 4-slot buffer, 3000 items | no lost wakeups (a lost one hangs the test, which the runner fails on timeout) |
| 4 threads doing read-*yield*-write on a counter under a mutex: exactly 1200 | mutual exclusion under forced interleaving |
| mutex misuse; `schedule()` while atomic | misuse is caught, not hung |
| orphan adopted and reaped by `kmain` | re-parenting |
| 200 threads created and reaped, after a warm-up round of 200; detached threads | all memory is returned: every frame and heap object; a detached thread frees itself |

The timing-sensitive tests were run 18 times with 6 QEMU instances
competing for 4 host cores, with no failures. Two harness bugs turned up
along the way:
- concurrent runs contended for the disk image's write lock (fixed with
  `-snapshot`),
- the original TSC-vs-PIT test was asserting emulator timing fidelity;
  it now checks the PIT programming exactly. Rate checks for the TSC and
  the LAPIC timer were added later, against the test's own PIT window
  and with a measured error bound (see [interrupts.md](interrupts.md)).

`tests/kernel/test_user.c` covers the rest of the lifecycle: zombies that
hold no user memory (reaping 8 of them frees at most a kernel stack and a
task struct each), and creation stopping at exactly `TASK_MAX`, where
`kthread_create()` returns NULL and `spawn` returns `-EAGAIN`, then
working again once tasks are reaped.

Two use-after-frees in the first draft of these tests were caught by the
kmalloc poison: they read `t->pid` after `task_wait()` had freed `t`.
