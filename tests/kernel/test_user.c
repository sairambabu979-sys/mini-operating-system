// SPDX-License-Identifier: MIT
/*
 * User mode: real ring-3 programs, spawned from the test task and judged
 * by their exit status. The hostile ones must die and leave the kernel,
 * and every other process, unharmed.
 */
#include <arch/cpu.h>
#include <arch/cpuid.h>
#include <arch/irqflags.h>
#include <arch/paging.h>
#include <arch/segments.h>
#include <arch/trap.h>
#include <core/ktest.h>
#include <core/time.h>
#include <lib/errno.h>
#include <mm/kmalloc.h>
#include <mm/pmm.h>
#include <proc/process.h>
#include <proc/uaccess.h>
#include <sched/sched.h>
#include <uapi/syscall.h>

/* Runs one program to completion; returns its exit status. */
static int run(const char *name)
{
    int pid = process_spawn(name);
    int status = -1000;

    if (pid < 0)
        return pid - 10000; /* distinguishable from any exit status */
    if (task_wait(pid, &status) != pid)
        return -20000;
    return status;
}

KTEST(user_process_runs_and_exits)
{
    u64 slow = syscall_slow_exits;

    KEXPECT_EQ(run("hello"), 42);
    KEXPECT_EQ(syscall_slow_exits, slow); /* ordinary syscalls return via SYSRET */
}

KTEST(user_null_dereference_kills_only_the_process)
{
    KEXPECT_EQ(run("faulter"), EXIT_STATUS_FAULT(VEC_PAGE_FAULT));
}

KTEST(user_privileged_instruction_is_refused)
{
    KEXPECT_EQ(run("privileged"), EXIT_STATUS_FAULT(VEC_GENERAL_PROTECTION));
}

KTEST(user_cannot_read_kernel_memory)
{
    KEXPECT_EQ(run("kread"), EXIT_STATUS_FAULT(VEC_PAGE_FAULT));
}

KTEST(user_text_is_not_writable)
{
    KEXPECT_EQ(run("selfmod"), EXIT_STATUS_FAULT(VEC_PAGE_FAULT));
}

KTEST(syscalls_reject_hostile_arguments)
{
    KEXPECT_EQ(run("badptr"), 0); /* exit status = number of wrong answers */
}

/*
 * The CVE-2012-0217 scenario: the user's RIP after syscall is
 * non-canonical. The kernel must avoid SYSRET, and the iretq it uses
 * instead faults in ring 0; that fault must kill the process, not the
 * kernel.
 */
KTEST(sysret_to_noncanonical_rip_kills_only_the_process)
{
    u64 slow = syscall_slow_exits;
    int status = run("sysret_hazard");

    /* The kernel refused SYSRET for this return... */
    KEXPECT_EQ(syscall_slow_exits, slow + 1);
    /*
     * ...and the process died. Real CPUs reject the non-canonical RIP in
     * iretq itself (#GP in ring 0, recovered via the exception table);
     * QEMU's TCG does not check, so the fault arrives one instruction
     * later as a user-mode #PF. Either way only the process dies.
     */
    KEXPECT(status == EXIT_STATUS_FAULT(VEC_GENERAL_PROTECTION) ||
            status == EXIT_STATUS_FAULT(VEC_PAGE_FAULT));
}

/* A kernel thread "returns to user mode" with an SS no CPU will accept. */
static int iret_with_bad_ss(void *arg)
{
    struct trap_frame frame = {0};

    (void)arg;
    frame.rip = 0x400000;
    frame.cs = GDT_USER_CODE | RPL_USER;
    frame.ss = GDT_KERNEL_DATA | RPL_USER; /* DPL 0 segment with RPL 3: #GP in iretq */
    frame.rsp = USER_STACK_TOP;
    frame.rflags = RFLAGS_IF | RFLAGS_RESERVED;
    enter_user(&frame);
}

/*
 * The bad-iretq recovery path, deterministically on any CPU: iretq faults
 * in ring 0 after swapgs already switched to the user GS. The exception
 * table must restore the kernel GS and kill the task; a later syscall
 * (which depends on the kernel GS) proves the state is intact.
 */
KTEST(faulting_iretq_kills_only_the_task)
{
    struct task *t = kthread_create(iret_with_bad_ss, NULL, "bad-iret", 0);
    int status = 0;
    int pid = t->pid;

    KEXPECT_EQ(task_wait(pid, &status), pid);
    KEXPECT_EQ(status, EXIT_STATUS_FAULT(VEC_GENERAL_PROTECTION));
    KEXPECT_EQ(run("hello"), 42); /* syscalls still find the kernel stack via GS */
}

/*
 * NT and AC are the process's to set, never the kernel's to inherit: with
 * NT, the next iretq anywhere in the kernel raises #GP; with AC, SMAP is off
 * in ring 0. The program sets both and keeps them across 100 syscalls and
 * ~30 timer interrupts, while a second process runs, and both must finish
 * normally.
 */
KTEST(user_rflags_nt_and_ac_do_not_leak_into_the_kernel)
{
    u64 ac_entries = trap_ac_entries();
    int bomb = process_spawn("flagbomb"), other = process_spawn("hello");
    int status, done = 0, ok = 0;

    KEXPECT(bomb > 0 && other > 0);
    while (done < 2) {
        int pid = task_wait(-1, &status);

        if (pid < 0)
            break;
        done++;
        ok += (pid == bomb && status == 0) || (pid == other && status == 42);
    }
    KEXPECT_EQ(ok, 2);
    /* The hazard was real: interrupts did arrive with the user's AC set. */
    if (cpu_features()->smap)
        KEXPECT(trap_ac_entries() > ac_entries);
}

/* Nothing from boot (firmware SSE use, say) shows through in a new process's registers. */
KTEST(user_processes_start_with_clean_fpu_registers)
{
    KEXPECT_EQ(run("fpuclean"), 0);
}

/* An unmasked x87 error must raise #MF (CR0.NE), killing only the process. */
KTEST(user_x87_error_raises_mf)
{
    KEXPECT(read_cr0() & CR0_NE);
    KEXPECT_EQ(run("x87fault"), EXIT_STATUS_FAULT(VEC_X87_FP));
}

KTEST(fpu_state_is_isolated_between_processes)
{
    int a = process_spawn("fpu"), b = process_spawn("fpu"), c = process_spawn("fpu");
    int status;

    KEXPECT(a > 0 && b > 0 && c > 0);
    for (int i = 0; i < 3; i++) {
        KEXPECT(task_wait(-1, &status) > 0);
        KEXPECT_EQ(status, 0);
    }
}

KTEST(spawning_unknown_programs_fails_cleanly)
{
    KEXPECT_EQ(process_spawn("does-not-exist"), -ENOENT);
}

KTEST(kernel_pointers_are_rejected_by_user_copies)
{
    u64 value = 0;

    KEXPECT_EQ(copy_from_user(&value, (const void *)&value, sizeof(value)), -EFAULT);
    KEXPECT_EQ(copy_to_user((void *)0xFFFF800000000000ull, &value, 8), -EFAULT);
    KEXPECT_EQ(copy_from_user(&value, (const void *)(USER_TOP - 4), 8), -EFAULT);
}

KTEST(process_churn_returns_all_memory)
{
    struct pmm_stats p0, p1;
    struct kmalloc_stats k0, k1;

    KEXPECT_EQ(run("hello"), 42); /* warm up caches */
    pmm_get_stats(&p0);
    kmalloc_get_stats(&k0);
    for (int i = 0; i < 40; i++)
        KEXPECT_EQ(run(i % 2 ? "hello" : "faulter"), i % 2 ? 42 : EXIT_STATUS_FAULT(14));
    pmm_get_stats(&p1);
    kmalloc_get_stats(&k1);
    /* Page tables, stacks and frames all come back, to the frame. */
    KEXPECT_EQ(p1.free_frames, p0.free_frames);
    KEXPECT_EQ(k1.small_live, k0.small_live);
}

static bool is_zombie_child(int pid)
{
    bool zombie = false;
    irqflags_t flags = arch_irq_save();

    list_for_each(it, &task_current()->children)
    {
        struct task *c = list_entry(it, struct task, sibling);

        zombie |= c->pid == pid && c->state == TASK_ZOMBIE;
    }
    arch_irq_restore(flags);
    return zombie;
}

/*
 * A process's memory goes back when it exits, not when its parent reaps
 * it: a parent that never waits must not pin every child's pages. Until
 * then a zombie holds only its task struct and kernel stack.
 */
KTEST(zombies_hold_no_user_memory)
{
    enum { N = 8 };
    struct pmm_stats zombies, reaped;
    struct kmalloc_stats kzombies, kreaped;
    int pids[N];
    u64 deadline = time_ticks() + 2000;

    KEXPECT_EQ(run("hello"), 42); /* warm up caches */
    for (int i = 0; i < N; i++) {
        pids[i] = process_spawn("hello");
        KEXPECT(pids[i] > 0);
    }
    for (int i = 0; i < N; i++) {
        while (pids[i] > 0 && !is_zombie_child(pids[i]) && time_ticks() < deadline)
            sleep_ms(1);
        KEXPECT(pids[i] <= 0 || is_zombie_child(pids[i]));
    }
    pmm_get_stats(&zombies);
    kmalloc_get_stats(&kzombies);
    for (int i = 0; i < N; i++) {
        int status = -1;

        if (pids[i] > 0) {
            KEXPECT_EQ(task_wait(pids[i], &status), pids[i]);
            KEXPECT_EQ(status, 42);
        }
    }
    pmm_get_stats(&reaped);
    kmalloc_get_stats(&kreaped);
    /* Reaping frees a kernel stack and a task struct each, and nothing else. */
    KEXPECT(reaped.free_frames - zombies.free_frames <= N * (KSTACK_SIZE / PAGE_SIZE_4K + 1));
    /* Of the heap, only the task structs: the FPU areas went at exit too. */
    KEXPECT_EQ(kzombies.small_live - kreaped.small_live, N);
}

static struct wait_queue hold_queue = WAIT_QUEUE_INIT(hold_queue);
static volatile bool hold_released;

static int hold(void *arg)
{
    (void)arg;
    wait_event(&hold_queue, hold_released);
    return 0;
}

/* A spawn loop stops at TASK_MAX with -EAGAIN, and creation works again after. */
KTEST(task_creation_stops_at_the_limit)
{
    struct sched_stats st;
    int held = 0;

    hold_released = false;
    for (;;) {
        sched_get_stats(&st);
        if (st.allocated >= TASK_MAX || !kthread_create(hold, NULL, "hold", 0))
            break;
        held++;
    }
    sched_get_stats(&st);
    KEXPECT_EQ(st.allocated, TASK_MAX);
    KEXPECT(kthread_create(hold, NULL, "hold", 0) == NULL);
    KEXPECT_EQ(process_spawn("hello"), -EAGAIN);

    hold_released = true;
    wake_up_all(&hold_queue);
    for (int i = 0; i < held; i++)
        KEXPECT(task_wait(-1, NULL) > 0);
    KEXPECT_EQ(run("hello"), 42);
}

KTEST(init_runs_the_whole_tour)
{
    KEXPECT_EQ(run("init"), 0); /* 0 = every child ended exactly as expected */
}
