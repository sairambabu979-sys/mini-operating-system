// SPDX-License-Identifier: MIT
/*
 * A new process must start with clean FPU/SSE registers: whatever the
 * firmware or the kernel's boot left in XMM0-15 or the x87 stack is not
 * this process's to read. Saves its own state before doing anything else
 * and exits 0 only if every register is zero.
 */
#include "libsys.h"

static unsigned char area[512] __attribute__((aligned(16)));

int main(void)
{
    __asm__ volatile("fxsave %0" : "=m"(area));

    int dirty = 0;

    for (int i = 32; i < 416; i++) /* ST0-7 (32..159), XMM0-15 (160..415) */
        dirty |= area[i];
    if (dirty)
        printf("fpuclean: inherited non-zero FPU/SSE register state\n");
    return dirty ? 1 : 0;
}
