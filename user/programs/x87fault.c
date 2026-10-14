// SPDX-License-Identifier: MIT
/*
 * Unmasks the x87 divide-by-zero exception and divides by zero. With
 * CR0.NE set the next x87 instruction raises #MF and the kernel kills this
 * process (exit status 128 + 16). Exiting normally means the error went
 * nowhere: the legacy FERR# path, which nothing handles.
 */
#include "libsys.h"

int main(void)
{
    unsigned short cw;

    __asm__ volatile("fninit; fnstcw %0" : "=m"(cw));
    cw &= (unsigned short)~0x04; /* ZM clear: zero-divide unmasked */
    __asm__ volatile("fldcw %0" : : "m"(cw));
    __asm__ volatile("fld1; fldz; fdivrp; fwait" ::: "memory", "st", "st(1)");
    return 0; /* not reached when #MF works */
}
