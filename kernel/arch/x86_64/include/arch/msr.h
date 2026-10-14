/* SPDX-License-Identifier: MIT */
/*
 * Model-specific registers.
 */
#ifndef MINIOS_ARCH_MSR_H
#define MINIOS_ARCH_MSR_H

#include <types.h>

#define MSR_EFER           0xC0000080u
#define MSR_STAR           0xC0000081u /* syscall/sysret segment selectors */
#define MSR_LSTAR          0xC0000082u /* syscall entry point */
#define MSR_FMASK          0xC0000084u /* RFLAGS bits cleared on syscall */
#define MSR_GS_BASE        0xC0000101u
#define MSR_KERNEL_GS_BASE 0xC0000102u /* swapped in by swapgs */

#define EFER_SCE (1ull << 0)  /* syscall/sysret enable */
#define EFER_LME (1ull << 8)  /* long mode enable */
#define EFER_LMA (1ull << 10) /* long mode active (read-only) */
#define EFER_NXE (1ull << 11) /* no-execute enable */

static inline u64 rdmsr(u32 msr)
{
    u32 lo, hi;

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static inline void wrmsr(u32 msr, u64 value)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((u32)value), "d"((u32)(value >> 32)) : "memory");
}

#endif /* MINIOS_ARCH_MSR_H */
