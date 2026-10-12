/* SPDX-License-Identifier: MIT */
/*
 * Model-specific registers.
 */
#ifndef MINIOS_ARCH_MSR_H
#define MINIOS_ARCH_MSR_H

#include <types.h>

#define MSR_PAT            0x00000277u /* page attribute table */
#define MSR_EFER           0xC0000080u
#define MSR_STAR           0xC0000081u /* syscall/sysret segment selectors */
#define MSR_LSTAR          0xC0000082u /* syscall entry point */
#define MSR_FMASK          0xC0000084u /* RFLAGS bits cleared on syscall */
#define MSR_GS_BASE        0xC0000101u
#define MSR_KERNEL_GS_BASE 0xC0000102u /* swapped in by swapgs */

/*
 * PAT entries the kernel uses, indexed by PTE bits PAT:PCD:PWT. Entries 0,
 * 2 and 3 keep their power-on types (WB, UC-, UC), so PCD|PWT still means
 * strong uncached; entry 1 (PWT alone) becomes write-combining instead of
 * write-through, which nothing used.
 */
#define PAT_UC          0x00ull
#define PAT_WC          0x01ull
#define PAT_WT          0x04ull
#define PAT_WB          0x06ull
#define PAT_UC_MINUS    0x07ull
#define PAT_ENTRY(i, t) ((t) << ((i) * 8))
#define PAT_KERNEL                                                                                 \
    (PAT_ENTRY(0, PAT_WB) | PAT_ENTRY(1, PAT_WC) | PAT_ENTRY(2, PAT_UC_MINUS) |                    \
     PAT_ENTRY(3, PAT_UC) | PAT_ENTRY(4, PAT_WB) | PAT_ENTRY(5, PAT_WC) |                          \
     PAT_ENTRY(6, PAT_UC_MINUS) | PAT_ENTRY(7, PAT_UC))

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
