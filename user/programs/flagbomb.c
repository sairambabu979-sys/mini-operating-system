// SPDX-License-Identifier: MIT
/*
 * Sets RFLAGS bits a ring-3 program is allowed to set but the kernel must
 * never inherit: NT (an iretq with NT set raises #GP in 64-bit mode) and
 * AC (in ring 0 it switches SMAP off). Then makes syscalls and spins so
 * that both syscalls and timer interrupts arrive with them set.
 */
#include "libsys.h"

#define RFLAGS_NT (1ul << 14)
#define RFLAGS_AC (1ul << 18)

int main(void)
{
    unsigned long flags;

    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    flags |= RFLAGS_NT | RFLAGS_AC;
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "cc", "memory");

    for (int i = 0; i < 100; i++)
        sys_yield();

    long end = sys_time_ms() + 30; /* ~30 timer interrupts land in this loop */

    while (sys_time_ms() < end)
        for (volatile int spin = 0; spin < 10000; spin++)
            ;

    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return (flags & (RFLAGS_NT | RFLAGS_AC)) == (RFLAGS_NT | RFLAGS_AC) ? 0 : 1; /* still ours */
}
