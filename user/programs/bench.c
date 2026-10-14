// SPDX-License-Identifier: MIT
/* Syscall round-trip cost, measured from ring 3 (RDTSC is allowed in user mode). */
#include "libsys.h"

static u64 rdtsc(void)
{
    u32 lo, hi;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

int main(void)
{
    enum { BATCHES = 9, ITERS = 5000 };
    u64 r[BATCHES];

    for (int b = 0; b < BATCHES; b++) {
        u64 t0 = rdtsc();

        for (int i = 0; i < ITERS; i++)
            sys_getpid();
        r[b] = (rdtsc() - t0) / ITERS;
    }
    for (int i = 1; i < BATCHES; i++) {
        u64 x = r[i];
        int j = i;

        for (; j > 0 && r[j - 1] > x; j--)
            r[j] = r[j - 1];
        r[j] = x;
    }
    printf("bench: %-32s %8lu cycles\n", "syscall round trip (getpid)", r[BATCHES / 2]);
    return 0;
}
