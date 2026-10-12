// SPDX-License-Identifier: MIT
/*
 * System call table and implementations. ABI: <uapi/syscall.h>.
 *
 * Every argument is untrusted. Pointers go through <proc/uaccess.h>,
 * never dereferenced directly; sizes are bounded before any work is done.
 */
#define pr_fmt(fmt) "syscall: " fmt

#include <arch/cpu.h>
#include <arch/msr.h>
#include <arch/segments.h>
#include <arch/trap.h>
#include <core/printk.h>
#include <core/time.h>
#include <lib/errno.h>
#include <proc/process.h>
#include <proc/uaccess.h>
#include <sched/sched.h>
#include <uapi/syscall.h>

#define WRITE_CHUNK 256
#define NAME_MAX    TASK_NAME_MAX

typedef s64 (*syscall_fn)(u64 a0, u64 a1, u64 a2);

void syscall_entry(void);
void syscall_dispatch(struct trap_frame *f);

static s64 sys_exit(u64 code, u64 a1, u64 a2)
{
    (void)a1;
    (void)a2;
    task_exit((int)code);
}

static s64 sys_write(u64 fd, u64 user_buf, u64 len)
{
    char chunk[WRITE_CHUNK];

    if (fd != 1 && fd != 2)
        return -EBADF;
    if (!user_range_ok(user_buf, len))
        return -EFAULT;
    for (u64 done = 0; done < len;) {
        usize n = (usize)MIN(len - done, (u64)WRITE_CHUNK);

        if (copy_from_user(chunk, (const void *)(user_buf + done), n) != 0)
            return done ? (s64)done : -EFAULT;
        console_write(chunk, n);
        done += n;
    }
    return (s64)len;
}

static s64 sys_yield(u64 a0, u64 a1, u64 a2)
{
    (void)a0;
    (void)a1;
    (void)a2;
    sched_yield();
    return 0;
}

static s64 sys_getpid(u64 a0, u64 a1, u64 a2)
{
    (void)a0;
    (void)a1;
    (void)a2;
    return task_current()->pid;
}

static s64 sys_sleep_ms(u64 ms, u64 a1, u64 a2)
{
    (void)a1;
    (void)a2;
    sleep_ms(MIN(ms, 3600ull * 1000)); /* cap at an hour: no tick-count overflow games */
    return 0;
}

static s64 sys_spawn(u64 user_name, u64 a1, u64 a2)
{
    char name[NAME_MAX];
    int len;

    (void)a1;
    (void)a2;
    len = strncpy_from_user(name, (const char *)user_name, sizeof(name));
    if (len < 0)
        return len;
    return process_spawn(name);
}

static s64 sys_wait(u64 pid, u64 user_status, u64 a2)
{
    int status = 0;

    (void)a2;
    /* Validate the destination first: a bad pointer must not consume the child. */
    if (user_status && !user_range_ok(user_status, sizeof(int)))
        return -EFAULT;
    int r = task_wait((int)pid, &status);

    if (r > 0 && user_status && copy_to_user((void *)user_status, &status, sizeof(status)) != 0)
        return -EFAULT;
    return r;
}

static s64 sys_time_ms(u64 a0, u64 a1, u64 a2)
{
    (void)a0;
    (void)a1;
    (void)a2;
    return (s64)(time_ns() / 1000000);
}

static const syscall_fn syscall_table[SYS_COUNT] = {
    [SYS_EXIT] = sys_exit,     [SYS_WRITE] = sys_write,       [SYS_YIELD] = sys_yield,
    [SYS_GETPID] = sys_getpid, [SYS_SLEEP_MS] = sys_sleep_ms, [SYS_SPAWN] = sys_spawn,
    [SYS_WAIT] = sys_wait,     [SYS_TIME_MS] = sys_time_ms,
};

/* Called from syscall.asm with interrupts enabled. Result goes in RAX. */
void syscall_dispatch(struct trap_frame *f)
{
    u64 nr = f->rax;

    if (nr >= SYS_COUNT || !syscall_table[nr]) {
        f->rax = (u64)-ENOSYS;
        return;
    }
    f->rax = (u64)syscall_table[nr](f->rdi, f->rsi, f->rdx);
}

void syscall_init(void)
{
    /*
     * STAR[47:32]: SYSCALL loads CS = 0x08, SS = 0x10.
     * STAR[63:48]: SYSRET loads CS = 0x18 + 16 = 0x28 (|3), SS = 0x18 + 8 = 0x20 (|3).
     */
    wrmsr(MSR_STAR, ((u64)(GDT_USER_CODE32 | RPL_USER) << 48) | ((u64)GDT_KERNEL_CODE << 32));
    wrmsr(MSR_LSTAR, (u64)(uptr)syscall_entry);
    /* Entry runs with IF off (until the stack switch), DF clear, no single-step, SMAP on. */
    wrmsr(MSR_FMASK, RFLAGS_IF | RFLAGS_DF | RFLAGS_TF | RFLAGS_AC);
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}
