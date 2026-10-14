// SPDX-License-Identifier: MIT
#include <arch/msr.h>
#include <arch/percpu.h>

struct percpu percpu;

void percpu_init(void)
{
    wrmsr(MSR_GS_BASE, (u64)(uptr)&percpu);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}
