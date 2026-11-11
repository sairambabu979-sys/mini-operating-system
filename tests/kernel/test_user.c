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
#include <core/input.h>
#include <core/ktest.h>
#include <core/preempt.h>
#include <core/printk.h>
#include <core/time.h>
#include <fs/file.h>
#include <fs/initrd.h>
#include <fs/pipe.h>
#include <fs/tarfs.h>
#include <fs/vfs.h>
#include <lib/errno.h>
#include <lib/printf.h>
#include <lib/string.h>
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
 * open, read, close, readdir and fstat on the initrd, every error in its
 * order, and spawn()'s stdio. Every fd fstest and its children had is
 * closed by the end, the child killed by a fault with a closed fd among open ones included.
 */
KTEST(file_calls_work_and_reject_hostile_arguments)
{
    u32 files = file_count(), in = console_input.refs, out = console_output.refs;

    KEXPECT_EQ(run("fstest"), 0); /* exit status = number of wrong answers, each printed */
    KEXPECT_EQ(file_count(), files);
    KEXPECT_EQ(console_input.refs, in);
    KEXPECT_EQ(console_output.refs, out);
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
     * iretq itself (#GP in ring 0, recovered via the exception table), and
     * so does QEMU 9.2's TCG (the x2APIC runs take that path). QEMU 8.2's
     * does not check, so the fault arrives one instruction later as a
     * user-mode #PF. Either way only the process dies.
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
    struct kmalloc_stats k0, k1;

    kmalloc_get_stats(&k0);
    KEXPECT_EQ(process_spawn("hello"), -EAGAIN);
    kmalloc_get_stats(&k1);
    KEXPECT_EQ(k1.small_live, k0.small_live); /* a refused spawn frees what it allocated */

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

/*
 * read(): `readkeys` first hands read() two bad buffers, which must fail
 * without taking a byte, then reads a line that must be exactly "ok\n",
 * with nothing written past the count. Input comes from input_inject(),
 * so these run with or without a device.
 */
KTEST(read_rejects_bad_buffers_without_consuming)
{
    bool was = input_test_set_present(true);
    struct input_stats st;

    input_test_flush();
    input_inject((const u8 *)"ok\n", 3); /* queued before the bad reads: they must leave it */
    KEXPECT_EQ(run("readkeys"), 0);
    input_get_stats(&st);
    KEXPECT_EQ(st.queued, 0);
    input_test_set_present(was);
}

KTEST(read_blocks_until_input)
{
    bool was = input_test_set_present(true);
    struct input_stats st;
    int status = -1;

    input_test_flush();
    input_get_stats(&st);
    u64 before = st.sleeps;
    int pid = process_spawn("readkeys");

    KEXPECT(pid > 0);
    if (pid <= 0) {
        input_test_set_present(was);
        return;
    }
    u64 deadline = time_ticks() + 2000;

    do { /* until the process is asleep in read() */
        sleep_ms(1);
        input_get_stats(&st);
    } while (st.sleeps == before && time_ticks() < deadline);
    KEXPECT(st.sleeps != before);
    input_inject((const u8 *)"ok\n", 3);
    deadline = time_ticks() + 2000;
    do { /* until it woke and took the line: a lost wakeup fails here, not in task_wait */
        sleep_ms(1);
        input_get_stats(&st);
    } while (st.queued && time_ticks() < deadline);
    KEXPECT_EQ(st.queued, 0);
    if (st.queued == 0) { /* else leave it asleep rather than hang the run */
        KEXPECT_EQ(task_wait(pid, &status), pid);
        KEXPECT_EQ(status, 0);
    }
    input_test_set_present(was);
}

/* The same, with the line typed on the PS/2 keyboard: IRQ 1 must wake the process. */
KTEST(kbd_irq_wakes_a_blocked_user_reader)
{
    static const u8 ok[] = {0x18, 0x98, 0x25, 0xA5, 0x1C, 0x9C}; /* o, k, Enter */
    struct input_stats st;
    int status = -1;

    if (!input_have_kbd())
        KTEST_SKIP("no 8042");
    input_test_flush();
    input_get_stats(&st);
    u64 before = st.sleeps;
    int pid = process_spawn("readkeys");

    KEXPECT(pid > 0);
    if (pid <= 0)
        return;
    u64 deadline = time_ticks() + 2000;

    do { /* until the process is asleep in read() */
        sleep_ms(1);
        input_get_stats(&st);
    } while (st.sleeps == before && time_ticks() < deadline);
    KEXPECT(st.sleeps != before);
    for (usize i = 0; i < sizeof(ok); i++)
        KEXPECT(input_test_kbd_put(ok[i]));
    deadline = time_ticks() + 2000;
    do { /* until it woke and took the line */
        sleep_ms(1);
        input_get_stats(&st);
    } while (st.queued && time_ticks() < deadline);
    KEXPECT_EQ(st.queued, 0);
    if (st.queued == 0) { /* else leave it asleep rather than hang the run */
        /*
         * Behind the typed line: a reader that got all of it has stopped at
         * the Enter, and one that lost a key reaches its 8 bytes and exits
         * with 4, instead of waiting in read() for an Enter never coming.
         */
        input_inject((const u8 *)"xxxxxxxx", 8);
        KEXPECT_EQ(task_wait(pid, &status), pid);
        KEXPECT_EQ(status, 0);
    }
    input_test_flush();
}

/* A read far longer than one read() returns: all that is queued, and not a byte past it. */
KTEST(read_returns_at_most_256_bytes)
{
    static u8 bytes[INPUT_READ_MAX];
    bool was = input_test_set_present(true);
    struct input_stats st;

    static_assert(INPUT_READ_MAX == 256 && INPUT_QUEUE_SIZE >= 256, "readmax expects 256 bytes");
    input_test_flush();
    for (usize i = 0; i < sizeof(bytes); i++)
        bytes[i] = (u8)(i * 3 + 1);
    input_inject(bytes, sizeof(bytes));
    KEXPECT_EQ(run("readmax"), 0);
    input_get_stats(&st);
    KEXPECT_EQ(st.queued, 0);
    input_test_set_present(was);
}

/* How often `needle` occurs in the newest 4 KiB of console output. */
static int log_count(const char *needle)
{
    static char tail[4096 + 1];
    usize n = console_log_read(tail, sizeof(tail) - 1), len = strlen(needle);
    int count = 0;

    for (usize i = 0; i + len <= n; i++)
        if (memcmp(tail + i, needle, len) == 0)
            count++;
    return count;
}

static bool log_has(const char *needle)
{
    return log_count(needle) > 0;
}

KTEST(read_without_input_device_is_enodev)
{
    bool was = input_test_set_present(false);

    KEXPECT_EQ(run("readkeys"), 100); /* the bad buffers still fail first, with -EFAULT */
    KEXPECT_EQ(run("sh"), 1);
    KEXPECT(log_has("sh: cannot read input (-19)\n"));
    input_test_set_present(was);
}

/*
 * The shell, fed a session through input_inject(): each line runs and
 * reports, and leaving it gives every task back. The output is checked in
 * the console log (sh writes it through the console like any program).
 */
KTEST(shell_runs_programs_and_reports_status)
{
    static const char session[] = "help\nhello\n   \n hello \nnosuch\nhello world\n"
                                  "echo a  b\nhelp me\necho 1 2 3 4 5 6 7 8\n"
                                  "echo 1 2 3 4 5 6 7 8 9\n"
                                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
                                  "faulter\nabc\x03"
                                  "exit\n\x04"; /* exit leaves the Ctrl-D unread */
    static const char *const want[] = {
        "  hello\n",
        "[exit 42]\n",
        "sh: nosuch: not found\n",
        "a b\n",
        /* 8 arguments, the most there may be: the echoed line, then echo's own */
        "echo 1 2 3 4 5 6 7 8\n1 2 3 4 5 6 7 8\n[exit 0]\n",
        "sh: help takes no arguments\n",
        "sh: too many arguments (8 at most)\n",
        "sh: name too long",
        "[exit 142]\n",
        "abc^C\n",
    };
    bool was = input_test_set_present(true);
    struct sched_stats before, after;
    char tail[9];

    input_test_flush();
    sched_get_stats(&before);
    input_inject((const u8 *)session, sizeof(session) - 1);
    KEXPECT_EQ(run("sh"), 0); /* a broken exit still ends at the Ctrl-D, not in a hang */
    KEXPECT_EQ(console_log_read(tail, sizeof(tail)), sizeof(tail));
    KEXPECT_EQ(memcmp(tail, "sh> exit\n", sizeof(tail)), 0); /* "exit" ended it, not the Ctrl-D */
    for (usize i = 0; i < sizeof(want) / sizeof(want[0]); i++)
        if (!log_has(want[i]))
            ktest_fail(__FILE__, __LINE__, "no '%s' in the console log", want[i]);
    KEXPECT_EQ(log_count("[exit 42]\n"), 3); /* " hello " and "hello world" are hello too */
    KEXPECT_EQ(log_count("not found\n"), 1); /* and "   " is nothing: only nosuch is unknown */
    sched_get_stats(&after);
    KEXPECT_EQ(after.tasks, before.tasks);
    input_test_flush();
    input_test_set_present(was);
}

/*
 * What was typed after a command's Enter belongs to the program it starts:
 * the shell must leave readkeys its "ok". A shell holding it would leave
 * readkeys waiting forever, so the test waits with a deadline, and then
 * types "ok" again to free it. "exit" comes only then: readkeys takes all
 * it can get, so typed with the rest it would join readkeys' line.
 */
KTEST(shell_leaves_typeahead_to_the_program_it_starts)
{
    static const char session[] = "readkeys\nok\n";
    bool was = input_test_set_present(true);
    int status = -1;

    input_test_flush();
    input_inject((const u8 *)session, sizeof(session) - 1);
    int pid = process_spawn("sh");

    KEXPECT(pid > 0);
    if (pid > 0) {
        u64 deadline = time_ticks() + 2000;

        while (!log_has("sh> readkeys\n[exit 0]\n") && time_ticks() < deadline)
            sleep_ms(1);
        if (!log_has("sh> readkeys\n[exit 0]\n")) {
            ktest_fail(__FILE__, __LINE__, "readkeys never got the 'ok' typed after it");
            input_inject((const u8 *)"ok\n", 3); /* free it */
            sleep_ms(100);
        }
        input_inject((const u8 *)"exit\n", 5);
        KEXPECT_EQ(task_wait(pid, &status), pid);
        KEXPECT_EQ(status, 0);
    }
    input_test_flush();
    input_test_set_present(was);
}

/*
 * pipe() from ring 3: pipetest and its children (writers that wait,
 * readers that wait, the end, -EPIPE, bad buffers, whole records, cat
 * between two pipes). Every pipe and every end is freed by the end.
 */
KTEST(pipes_work_and_reject_hostile_arguments)
{
    u32 files = file_count(), pipes = pipe_count();

    KEXPECT_EQ(run("pipetest"), 0); /* exit status = number of wrong answers, each printed */
    KEXPECT_EQ(file_count(), files);
    KEXPECT_EQ(pipe_count(), pipes);
}

/* cat with no arguments copies its standard input to its end: here a file the test opened. */
KTEST(cat_copies_its_standard_input)
{
    usize len, first_line = 0;
    const u8 *rd = initrd_get(&len);
    struct tarfs_node node;
    char want[128];
    int status = -1;

    KEXPECT(rd != NULL);
    if (!rd || tarfs_lookup(rd, len, "/etc/manifest", &node) != 0) {
        ktest_fail(__FILE__, __LINE__, "no /etc/manifest");
        return;
    }
    while (first_line < node.size && first_line < sizeof(want) - 1 && node.data[first_line] != '\n')
        first_line++;
    memcpy(want, node.data, first_line);
    want[first_line] = '\0';

    u32 files = file_count();
    struct file *in = file_open_initrd(node.data, node.size);
    struct file *std[3] = {in, &console_output, &console_output};
    int pid = in ? process_spawn_args("cat", NULL, std) : -ENOMEM;

    if (in)
        file_put(in); /* the child holds its own reference */
    KEXPECT(pid > 0);
    if (pid > 0) {
        KEXPECT_EQ(task_wait(pid, &status), pid);
        KEXPECT_EQ(status, 0);
    }
    KEXPECT(first_line > 0 &&
            log_has(want)); /* the manifest's first line, which nothing else prints */
    KEXPECT_EQ(file_count(), files);
}

/*
 * cat at the console: edited and echoed a line at a time (Ctrl-C drops
 * one), each line copied as it ends, and Ctrl-D on an empty line ends
 * it. What follows the Ctrl-D is left for the shell.
 */
KTEST(cat_reads_the_console_until_ctrl_d)
{
    static const char session[] = "cat\nhi\nabc\x03there\n\x04"
                                  "exit\n"; /* \x04e is one byte */
    bool was = input_test_set_present(true);

    input_test_flush();
    input_inject((const u8 *)session, sizeof(session) - 1);
    KEXPECT_EQ(run("sh"), 0);
    KEXPECT(log_has("sh> cat\nhi\nhi\nabc^C\nthere\nthere\n[exit 0]\nsh> exit\n"));
    input_test_flush();
    input_test_set_present(was);
}

/*
 * The same from a serial terminal, whose Enter is CRLF: the shell ends
 * "cat" at the CR, and the LF left queued is not an empty line for cat.
 */
KTEST(cat_reads_crlf_lines_from_the_console)
{
    static const char session[] = "cat\r\nhi\r\n\x04"
                                  "exit\r\n";
    bool was = input_test_set_present(true);

    input_test_flush();
    input_inject((const u8 *)session, sizeof(session) - 1);
    KEXPECT_EQ(run("sh"), 0);
    KEXPECT(log_has("sh> cat\nhi\nhi\n[exit 0]\nsh> exit\n"));
    input_test_flush();
    input_test_set_present(was);
}

/*
 * Pipelines: each program's output is the next one's input, with or
 * without spaces around the |, through as many as four; errors go to
 * the console, not down the pipe; a program that cannot start leaves the
 * rest to run on an empty pipe or a reader-less one; and a line that is
 * not a pipeline is refused before anything runs. Every pipe is freed.
 */
KTEST(shell_runs_pipelines)
{
    static const char session[] = "cat /etc/motd | wc\n"
                                  "cat /etc/motd|cat|cat|wc\n"
                                  "cat /nope | wc\n"
                                  "nosuch | wc\n"
                                  "ls /bin | nosuch\n"
                                  "cat /bin/sh | nosuch\n"
                                  "nosuch\n"
                                  "| wc\nwc |\nls || wc\nhelp | wc\nhelp x | wc\n"
                                  "wc|wc|wc|wc|wc|wc|wc|wc|wc\n"
                                  "wc /etc/motd /nope\n"
                                  "wc\nab cd\n\x04"
                                  "exit\n";
    static const char *const want[] = {
        "sh> cat /etc/motd | wc\n2 16 89\n[exit 0 0]\n",
        "sh> cat /etc/motd|cat|cat|wc\n2 16 89\n[exit 0 0 0 0]\n",
        "sh> cat /nope | wc\ncat: /nope: not found\n0 0 0\n[exit 1 0]\n",
        "sh> nosuch | wc\nsh: nosuch: not found\n0 0 0\n[exit - 0]\n",
        /* more than a pipe holds, to a reader that never came: cat must get -EPIPE, not wait */
        ("sh> cat /bin/sh | nosuch\nsh: nosuch: not found\ncat: standard output: broken pipe\n"
         "[exit 1 -]\n"),
        "sh> nosuch\nsh: nosuch: not found\nsh> ", /* alone, no [exit] line */
        "sh> | wc\nsh: a pipeline needs a program on each side of every |\n",
        "sh> wc |\nsh: a pipeline needs a program on each side of every |\n",
        "sh> ls || wc\nsh: a pipeline needs a program on each side of every |\n",
        "sh> help | wc\nsh: help cannot be in a pipeline or redirected\n",
        "sh> help x | wc\nsh: help cannot be in a pipeline or redirected\n",
        "sh> wc|wc|wc|wc|wc|wc|wc|wc|wc\nsh: too many programs in a pipeline (8 at most)\n",
        "2 16 89 /etc/motd\nwc: /nope: not found\n2 16 89 total\n[exit 1]\n",
        "sh> wc\nab cd\n1 2 6\n[exit 0]\nsh> exit\n",
    };
    bool was = input_test_set_present(true);
    u32 files = file_count(), pipes = pipe_count();

    input_test_flush();
    input_inject((const u8 *)session, sizeof(session) - 1);
    KEXPECT_EQ(run("sh"), 0);
    for (usize i = 0; i < ARRAY_SIZE(want); i++)
        if (!log_has(want[i]))
            ktest_fail(__FILE__, __LINE__, "no '%s' in the console log", want[i]);
    /* ls's output fits in the pipe: -EPIPE, unless it ran before the shell closed the read end */
    KEXPECT(log_has("sh> ls /bin | nosuch\nsh: nosuch: not found\n"
                    "ls: standard output: broken pipe\n[exit 1 -]\n") ||
            log_has("sh> ls /bin | nosuch\nsh: nosuch: not found\n[exit 0 -]\n"));
    KEXPECT_EQ(file_count(), files);
    KEXPECT_EQ(pipe_count(), pipes);
    input_test_flush();
    input_test_set_present(was);
}

/* Ctrl-D on an empty line leaves, and the output still ends with a newline. */
KTEST(shell_eof_exits_cleanly)
{
    bool was = input_test_set_present(true);
    char last;

    input_test_flush();
    input_inject((const u8 *)"\x04", 1);
    KEXPECT_EQ(run("sh"), 0);
    KEXPECT_EQ(console_log_read(&last, 1), 1);
    KEXPECT_EQ(last, '\n'); /* so the next TAP line starts a line of its own */
    input_test_set_present(was);
}

/* Runs `name` with `args` and returns its exit status, or a negative error. */
static int run_args(const char *name, const struct spawn_args *args)
{
    int pid = process_spawn_args(name, args, NULL);
    int status = -1000;

    if (pid < 0)
        return pid;
    if (task_wait(pid, &status) != pid)
        return -20000;
    return status;
}

KTEST(spawn_passes_arguments_on_the_stack)
{
    struct spawn_args args = {0};

    KEXPECT_EQ(spawn_args_add(&args, "one"), 0);
    KEXPECT_EQ(spawn_args_add(&args, "two two"), 0);
    KEXPECT_EQ(run_args("argcheck", &args), 3); /* argc, after argcheck's own checks */
    KEXPECT(log_has("argcheck: [argcheck] [one] [two two]\n"));
    KEXPECT_EQ(run("argcheck"), 1);
    KEXPECT(log_has("argcheck: [argcheck]\n"));
}

/* Exactly the limits run; one argument or one byte more does not fit. */
KTEST(spawn_arguments_fill_their_limits_exactly)
{
    static struct spawn_args args;
    char arg[SPAWN_ARGS_SIZE / SPAWN_ARGS_MAX]; /* 31 characters and the NUL: 8 of them fill it */

    args = (struct spawn_args){0};
    memset(arg, 'a', sizeof(arg) - 1);
    arg[sizeof(arg) - 1] = '\0';
    for (int i = 0; i < SPAWN_ARGS_MAX; i++)
        KEXPECT_EQ(spawn_args_add(&args, arg), 0);
    KEXPECT_EQ(args.len, SPAWN_ARGS_SIZE);
    KEXPECT_EQ(spawn_args_add(&args, ""), -E2BIG); /* a ninth */
    KEXPECT_EQ(run_args("argcheck", &args), 1 + SPAWN_ARGS_MAX);

    args = (struct spawn_args){0};
    for (int i = 0; i < SPAWN_ARGS_MAX - 1; i++)
        KEXPECT_EQ(spawn_args_add(&args, arg), 0);
    arg[sizeof(arg) - 1] = 'a'; /* now 32 characters: one byte past the size */
    char longer[sizeof(arg) + 1];

    memcpy(longer, arg, sizeof(arg));
    longer[sizeof(arg)] = '\0';
    KEXPECT_EQ(spawn_args_add(&args, longer), -E2BIG);
    KEXPECT_EQ(args.argc, SPAWN_ARGS_MAX - 1); /* a failed add changes nothing */
}

/*
 * wc counts what crosses many of its reads: /bin/sh (any bytes will do)
 * by name, and through a pipe the test makes, from cat to wc. The counts
 * are worked out here from the archive itself.
 */
KTEST(wc_counts_a_file_and_a_pipe)
{
    usize len;
    const u8 *rd = initrd_get(&len);
    struct tarfs_node node;
    struct spawn_args args = {0};
    u64 lines = 0, words = 0;
    bool in_word = false;
    char want[96];
    struct file *r, *w;
    int status = -1;

    if (!rd || tarfs_lookup(rd, len, "/bin/sh", &node) != 0) {
        ktest_fail(__FILE__, __LINE__, "no /bin/sh");
        return;
    }
    KEXPECT(node.size > 8ul * 512); /* many of wc's 512-byte reads */
    for (u64 i = 0; i < node.size; i++) {
        u8 b = node.data[i];
        bool space = b == ' ' || b == '\t' || b == '\r' || b == '\n';

        lines += b == '\n';
        words += !space && !in_word;
        in_word = !space;
    }
    KEXPECT_EQ(spawn_args_add(&args, "/bin/sh"), 0);
    k_snprintf(want, sizeof(want), "\n%lu %lu %lu /bin/sh\n", lines, words, node.size);
    KEXPECT_EQ(run_args("wc", &args), 0);
    KEXPECT(log_has(want));

    u32 files = file_count(), pipes = pipe_count();

    KEXPECT_EQ(pipe_create(&r, &w), 0);

    struct file *cat_std[3] = {&console_input, w, &console_output};
    struct file *wc_std[3] = {r, &console_output, &console_output};
    int cat = process_spawn_args("cat", &args, cat_std);
    int wc = process_spawn_args("wc", NULL, wc_std);

    file_put(r); /* the children hold their own ends: wc sees the end when cat exits */
    file_put(w);
    KEXPECT(cat > 0 && wc > 0);
    if (cat > 0) {
        KEXPECT_EQ(task_wait(cat, &status), cat);
        KEXPECT_EQ(status, 0);
    }
    if (wc > 0) {
        KEXPECT_EQ(task_wait(wc, &status), wc);
        KEXPECT_EQ(status, 0);
    }
    k_snprintf(want, sizeof(want), "\n%lu %lu %lu\n", lines, words, node.size);
    KEXPECT(log_has(want));
    KEXPECT_EQ(file_count(), files);
    KEXPECT_EQ(pipe_count(), pipes);
}

/*
 * /tmp from ring 3: tmptest checks open()'s flags and errors, writes,
 * removal, the limits and bad buffers, and leaves /tmp empty. Every
 * file, page and open file is given back.
 */
KTEST(tmp_files_work_and_reject_hostile_arguments)
{
    u32 files = file_count();

    KEXPECT_EQ(vfs_tmp_files(), 0);
    KEXPECT_EQ(run("tmptest"), 0); /* exit status = number of wrong answers, each printed */
    KEXPECT_EQ(vfs_tmp_files(), 0);
    KEXPECT_EQ(vfs_tmp_pages(), 0);
    KEXPECT_EQ(file_count(), files);
}

/* Opens `path` with kmalloc's allocation number n failing (n -1: none). */
static int open_failing(const char *path, u32 flags, long n, struct file **f)
{
    int err;

    preempt_disable(); /* no other task allocates meanwhile; the lock is free, so no sleep */
    long previous = kmalloc_test_fail_one(n);

    err = vfs_open(path, flags, true, f);
    (void)kmalloc_test_fail_one(previous);
    preempt_enable();
    return err;
}

/*
 * Making a /tmp file takes two allocations, the file and its open file;
 * either failing is -ENOMEM with nothing made. O_TRUNC on an existing
 * file whose open file cannot be had leaves it as it was.
 */
KTEST(tmp_open_out_of_memory_changes_nothing)
{
    struct kmalloc_stats k0, k1;
    struct spawn_args args = {0};
    struct file *f = NULL;
    u32 files = file_count();

    kmalloc_get_stats(&k0);
    KEXPECT_EQ(open_failing("/tmp/oom", O_WRONLY | O_CREAT, 0, &f), -ENOMEM);
    KEXPECT_EQ(vfs_tmp_files(), 0);
    KEXPECT_EQ(open_failing("/tmp/oom", O_WRONLY | O_CREAT, 1, &f), -ENOMEM);
    KEXPECT_EQ(vfs_tmp_files(), 0); /* made, then taken back */
    KEXPECT_EQ(open_failing("/tmp/oom", O_WRONLY | O_CREAT, 2, &f), 0);
    KEXPECT_EQ(vfs_tmp_files(), 1);
    file_put(f);
    KEXPECT_EQ(vfs_unlink("/tmp/oom"), 0);
    kmalloc_get_stats(&k1);
    KEXPECT_EQ(k1.small_live, k0.small_live);
    KEXPECT_EQ(k1.large_live, k0.large_live);

    KEXPECT_EQ(spawn_args_add(&args, "fill"), 0);
    KEXPECT_EQ(run_args("tmptest", &args), 0); /* /tmp/keep: "keep me" */
    KEXPECT_EQ(open_failing("/tmp/keep", O_WRONLY | O_TRUNC, 0, &f), -ENOMEM);
    KEXPECT_EQ(vfs_open("/tmp/keep", O_RDONLY, true, &f), 0);
    KEXPECT_EQ(f->ops->size(f), 7); /* not emptied */
    file_put(f);
    KEXPECT_EQ(vfs_unlink("/tmp/keep"), 0);
    KEXPECT_EQ(vfs_tmp_files(), 0);
    KEXPECT_EQ(vfs_tmp_pages(), 0);
    KEXPECT_EQ(file_count(), files);
}

/*
 * Runs the shell on `session` (less than the 256-byte input queue, which
 * drops the rest) and checks the console log holds each of want[].
 */
static void shell_session(const char *session, const char *const *want, usize nwant)
{
    bool was = input_test_set_present(true);

    input_test_flush();
    input_inject((const u8 *)session, strlen(session));
    KEXPECT_EQ(run("sh"), 0);
    for (usize i = 0; i < nwant; i++)
        if (!log_has(want[i]))
            ktest_fail(__FILE__, __LINE__, "no '%s' in the console log", want[i]);
    input_test_flush();
    input_test_set_present(was);
}

/*
 * Redirection: > makes or empties a /tmp file, >> adds to it, < reads
 * one, with or without spaces, alone and in a pipeline; rm removes them,
 * and /tmp is empty again at the end.
 */
KTEST(shell_redirects_to_and_from_files)
{
    static const char session[] = "echo hi > /tmp/a\n"
                                  "echo yo >>/tmp/a\n"
                                  "cat /tmp/a\n"
                                  "wc</tmp/a\n"
                                  "cat < /tmp/a | wc\n"
                                  "wc /tmp/a > /tmp/n\n"
                                  "cat /tmp/n\n"
                                  "echo new > /tmp/a\n"
                                  "cat /tmp/a\n"
                                  "rm /tmp/a /tmp/n\n"
                                  "rm /tmp/a\n"
                                  "rm\n"
                                  "ls /tmp\n"
                                  "exit\n";
    static const char *const want[] = {
        "sh> echo hi > /tmp/a\n[exit 0]\n",
        "sh> echo yo >>/tmp/a\n[exit 0]\n",
        "sh> cat /tmp/a\nhi\nyo\n[exit 0]\n",
        "sh> wc</tmp/a\n2 2 6\n[exit 0]\n",
        "sh> cat < /tmp/a | wc\n2 2 6\n[exit 0 0]\n",
        "sh> cat /tmp/n\n2 2 6 /tmp/a\n[exit 0]\n",
        "sh> cat /tmp/a\nnew\n[exit 0]\n", /* > empties it first */
        "sh> rm /tmp/a /tmp/n\n[exit 0]\n",
        "sh> rm /tmp/a\nrm: /tmp/a: not found\n[exit 1]\n",
        "sh> rm\nusage: rm FILE...\n[exit 2]\n",
        "sh> ls /tmp\n[exit 0]\nsh> exit\n", /* nothing left */
    };
    u32 files = file_count();

    _Static_assert(sizeof(session) <= INPUT_QUEUE_SIZE, "the input queue would drop the rest");
    shell_session(session, want, ARRAY_SIZE(want));
    KEXPECT_EQ(vfs_tmp_files(), 0);
    KEXPECT_EQ(file_count(), files);
}

/*
 * Each wrong redirection is refused before anything runs, and a file
 * that cannot be opened runs nothing: /tmp is still empty after.
 */
KTEST(shell_refuses_bad_redirections)
{
    static const char session[] = "cat < /tmp/nope\n"
                                  "echo x > /etc/motd\n"
                                  "echo x > out\n"
                                  "echo >\n"
                                  "cat <\n"
                                  "echo > | wc\n"
                                  "echo > > /tmp/x\n"
                                  "> /tmp/x\n"
                                  "cat | wc < /tmp/a\n"
                                  "echo a > /tmp/a | wc\n"
                                  "echo a >/tmp/a >/tmp/b\n"
                                  "help > /tmp/x\n"
                                  "ls /tmp\n"
                                  "exit\n";
    static const char *const want[] = {
        "sh> cat < /tmp/nope\nsh: /tmp/nope: not found\nsh> ",
        "sh> echo x > /etc/motd\nsh: /etc/motd: read-only file system\nsh> ",
        "sh> echo x > out\nsh: out: bad path",
        "sh> echo >\nsh: > needs a file name after it\n",
        "sh> cat <\nsh: < needs a file name after it\n",
        "sh> echo > | wc\nsh: > needs a file name after it\n",
        "sh> echo > > /tmp/x\nsh: > needs a file name after it\n",
        "sh> > /tmp/x\nsh: no program to run\n",
        "sh> cat | wc < /tmp/a\nsh: only the first program of a pipeline can read a file (<)\n",
        "sh> echo a > /tmp/a | wc\nsh: only the last program of a pipeline can write a file (>)\n",
        "sh> echo a >/tmp/a >/tmp/b\nsh: two > or >> for one program\n",
        "sh> help > /tmp/x\nsh: help cannot be in a pipeline or redirected\n",
        "sh> ls /tmp\n[exit 0]\nsh> exit\n", /* nothing made by any of them */
    };
    u32 files = file_count();

    _Static_assert(sizeof(session) <= INPUT_QUEUE_SIZE, "the input queue would drop the rest");
    shell_session(session, want, ARRAY_SIZE(want));
    KEXPECT_EQ(vfs_tmp_files(), 0);
    KEXPECT_EQ(file_count(), files);
}

/*
 * The shell closes each redirected file once its program has it: nine
 * of each would run out of fds (FILES_MAX is 8) if it kept one.
 */
KTEST(shell_closes_redirected_files)
{
    static const char session[] =
        "echo>/tmp/a\necho>/tmp/a\necho>/tmp/a\necho>/tmp/a\necho>/tmp/a\n"
        "echo>/tmp/a\necho>/tmp/a\necho>/tmp/a\necho>/tmp/a\n"
        "cat</tmp/a\ncat</tmp/a\ncat</tmp/a\ncat</tmp/a\ncat</tmp/a\n"
        "cat</tmp/a\ncat</tmp/a\ncat</tmp/a\ncat</tmp/a\n"
        "rm /tmp/a\nexit\n";
    static const char *const want[] = {"sh> rm /tmp/a\n[exit 0]\nsh> exit\n"};
    u32 files = file_count();

    _Static_assert(sizeof(session) <= INPUT_QUEUE_SIZE, "the input queue would drop the rest");
    shell_session(session, want, ARRAY_SIZE(want));
    KEXPECT_EQ(log_count("sh> echo>/tmp/a\n[exit 0]\n"), 9);
    KEXPECT_EQ(log_count("sh> cat</tmp/a\n\n[exit 0]\n"), 9);
    KEXPECT_EQ(log_count("too many open files"), 0);

    /* and the < file when the > file cannot be opened, so nothing runs */
    static const char failing[] =
        "echo>/tmp/a\n"
        "cat</tmp/a>/etc/x\ncat</tmp/a>/etc/x\ncat</tmp/a>/etc/x\ncat</tmp/a>/etc/x\n"
        "cat</tmp/a>/etc/x\ncat</tmp/a>/etc/x\ncat</tmp/a>/etc/x\ncat</tmp/a>/etc/x\n"
        "cat</tmp/a>/etc/x\n"
        "rm /tmp/a\nexit\n";
    static const char *const after[] = {"sh> rm /tmp/a\n[exit 0]\nsh> exit\n"};

    _Static_assert(sizeof(failing) <= INPUT_QUEUE_SIZE, "the input queue would drop the rest");
    shell_session(failing, after, ARRAY_SIZE(after));
    KEXPECT_EQ(log_count("sh: /etc/x: read-only file system\n"), 9);
    KEXPECT_EQ(log_count("too many open files"), 0);
    KEXPECT_EQ(vfs_tmp_files(), 0);
    KEXPECT_EQ(file_count(), files);
}
