/* SPDX-License-Identifier: MIT */
/*
 * CPUID and the feature set the kernel cares about.
 */
#ifndef MINIOS_ARCH_CPUID_H
#define MINIOS_ARCH_CPUID_H

#include <types.h>

struct cpuid_regs {
    u32 eax, ebx, ecx, edx;
};

static inline struct cpuid_regs cpuid(u32 leaf, u32 subleaf)
{
    struct cpuid_regs r;

    __asm__ volatile("cpuid"
                     : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                     : "a"(leaf), "c"(subleaf));
    return r;
}

struct cpu_features {
    bool nx;      /* execute-disable page bit (required) */
    bool pge;     /* global pages */
    bool page_1g; /* 1 GiB pages */
    bool smep;    /* supervisor-mode execution prevention */
    bool smap;    /* supervisor-mode access prevention */
    bool invariant_tsc;
    bool apic;   /* on-chip local APIC */
    bool x2apic; /* x2APIC mode available (MSR interface) */
    u32 phys_addr_bits;
    u32 virt_addr_bits;
    char vendor[13];
};

/* Fills the global feature set; call once, before paging setup. */
void cpu_detect_features(void);
const struct cpu_features *cpu_features(void);

#endif /* MINIOS_ARCH_CPUID_H */
