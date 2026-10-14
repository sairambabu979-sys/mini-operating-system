// SPDX-License-Identifier: MIT
/*
 * Microbenchmarks, run with "bench" on the command line (make bench).
 *
 * Each reports the median of several batches in TSC cycles, plus
 * nanoseconds via the boot-time TSC calibration. Under QEMU's TCG these
 * numbers measure the emulator as much as the kernel; compare them only
 * with each other, or rerun under KVM.
 */
#define pr_fmt(fmt) "bench: " fmt

#include <core/bench.h>
#include <core/klog.h>
#include <core/time.h>
#include <mm/kmalloc.h>
#include <mm/pmm.h>
#include <proc/process.h>
#include <sched/sched.h>

#define BATCHES 9
#define ITERS   2000

static void sort_u64(u64 *v, int n)
{
    for (int i = 1; i < n; i++) {
        u64 x = v[i];
        int j = i;

        for (; j > 0 && v[j - 1] > x; j--)
            v[j] = v[j - 1];
        v[j] = x;
    }
}

static void report(const char *what, u64 *per_op, int n)
{
    sort_u64(per_op, n);
    u64 median = per_op[n / 2];

    pr_info("%-32s %8llu cycles  %8llu ns\n", what, (unsigned long long)median,
            (unsigned long long)(median * 1000000 / tsc_khz()));
}

static void bench_pmm(void)
{
    u64 r[BATCHES];

    for (int b = 0; b < BATCHES; b++) {
        u64 t0 = rdtsc();

        for (int i = 0; i < ITERS; i++)
            pmm_free_page(pmm_alloc_page());
        r[b] = (rdtsc() - t0) / ITERS;
    }
    report("pmm alloc+free (4 KiB)", r, BATCHES);
}

static void bench_kmalloc(void)
{
    u64 r[BATCHES];

    for (int b = 0; b < BATCHES; b++) {
        u64 t0 = rdtsc();

        for (int i = 0; i < ITERS; i++)
            kfree(kmalloc(64));
        r[b] = (rdtsc() - t0) / ITERS;
    }
    report("kmalloc+kfree (64 B)", r, BATCHES);
}

static volatile bool pong_stop;

static int yielder(void *arg)
{
    (void)arg;
    while (!pong_stop)
        sched_yield();
    return 0;
}

/* Two runnable threads yielding to each other: one yield = one switch. */
static void bench_context_switch(void)
{
    u64 r[BATCHES];

    pong_stop = false;
    if (!kthread_create(yielder, NULL, "bench-peer", 0))
        return;
    for (int b = 0; b < BATCHES; b++) {
        u64 t0 = rdtsc();

        for (int i = 0; i < ITERS; i++)
            sched_yield();
        r[b] = (rdtsc() - t0) / (2ull * ITERS); /* each round trip is two switches */
    }
    pong_stop = true;
    (void)task_wait(-1, NULL);
    report("context switch (kthread)", r, BATCHES);
}

void bench_run_all(void)
{
    pr_info("median of %d batches x %d iterations; TSC %llu kHz\n", BATCHES, ITERS,
            (unsigned long long)tsc_khz());
    bench_pmm();
    bench_kmalloc();
    bench_context_switch();

    /* The syscall benchmark measures from ring 3 (user/programs/bench.c). */
    int pid = process_spawn("bench");
    int status = -1;

    if (pid > 0)
        (void)task_wait(pid, &status);
    pr_info("done (user benchmark exit %d)\n", status);
}
