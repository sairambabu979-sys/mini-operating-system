// SPDX-License-Identifier: MIT
#define pr_fmt(fmt) "cpu: " fmt

#include <arch/cpuid.h>
#include <core/klog.h>
#include <lib/string.h>

static struct cpu_features features;

void cpu_detect_features(void)
{
    struct cpuid_regs r = cpuid(0, 0);
    u32 max_leaf = r.eax;

    memcpy(&features.vendor[0], &r.ebx, 4);
    memcpy(&features.vendor[4], &r.edx, 4);
    memcpy(&features.vendor[8], &r.ecx, 4);
    features.vendor[12] = '\0';

    r = cpuid(1, 0);
    features.pge = r.edx & (1u << 13);
    features.apic = r.edx & (1u << 9);
    features.pat = r.edx & (1u << 16);
    features.x2apic = r.ecx & (1u << 21);

    if (max_leaf >= 7) {
        r = cpuid(7, 0);
        features.smep = r.ebx & (1u << 7);
        features.smap = r.ebx & (1u << 20);
    }

    u32 max_ext = cpuid(0x80000000, 0).eax;

    if (max_ext >= 0x80000001) {
        r = cpuid(0x80000001, 0);
        features.nx = r.edx & (1u << 20);
        features.page_1g = r.edx & (1u << 26);
    }
    if (max_ext >= 0x80000007)
        features.invariant_tsc = cpuid(0x80000007, 0).edx & (1u << 8);
    if (max_ext >= 0x80000008) {
        r = cpuid(0x80000008, 0);
        features.phys_addr_bits = r.eax & 0xFF;
        features.virt_addr_bits = (r.eax >> 8) & 0xFF;
    } else {
        features.phys_addr_bits = 36;
        features.virt_addr_bits = 48;
    }

    pr_info("%s, %u-bit physical / %u-bit virtual; nx=%d pge=%d 1g=%d smep=%d smap=%d apic=%d "
            "x2apic=%d\n",
            features.vendor, features.phys_addr_bits, features.virt_addr_bits, features.nx,
            features.pge, features.page_1g, features.smep, features.smap, features.apic,
            features.x2apic);
}

const struct cpu_features *cpu_features(void)
{
    return &features;
}
