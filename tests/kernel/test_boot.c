// SPDX-License-Identifier: MIT
/*
 * Boot-protocol tests: verify the state the loader promises the kernel
 * (docs/design/boot.md) against the actual machine.
 */
#include <arch/cpu.h>
#include <arch/msr.h>
#include <arch/paging.h>
#include <core/boot.h>
#include <core/ktest.h>
#include <core/sections.h>
#include <lib/string.h>

KTEST(boot_info_is_valid)
{
    const struct boot_info *bi = boot_info_get();

    KEXPECT_EQ(bi->magic, BOOT_INFO_MAGIC);
    KEXPECT_EQ(bi->version, BOOT_INFO_VERSION);
    KEXPECT(bi->mmap_count > 0 && bi->mmap_count <= BOOT_MMAP_MAX);
    KEXPECT(strnlen(bi->cmdline, BOOT_CMDLINE_MAX) < BOOT_CMDLINE_MAX);
}

KTEST(kernel_loaded_at_link_address)
{
    const struct boot_info *bi = boot_info_get();

    KEXPECT_EQ(bi->kernel_phys_start, (uptr)__kernel_start - KERNEL_VMA);
    KEXPECT_EQ(bi->kernel_phys_end, (uptr)__kernel_end - KERNEL_VMA);
    KEXPECT_EQ(bi->kernel_phys_start, 0x100000);
}

KTEST(sections_are_page_aligned_and_ordered)
{
    uptr bounds[] = {(uptr)__text_start, (uptr)__text_end,   (uptr)__rodata_start,
                     (uptr)__rodata_end, (uptr)__data_start, (uptr)__data_end};

    for (usize i = 0; i < ARRAY_SIZE(bounds); i++) {
        KEXPECT(IS_ALIGNED(bounds[i], 4096));
        if (i > 0)
            KEXPECT(bounds[i] >= bounds[i - 1]);
    }
}

/* volatile: the compiler must not assume a never-written static reads 0. */
static volatile u8 bss_probe[8192];

KTEST(bss_is_zeroed_by_loader)
{
    for (usize i = 0; i < sizeof(bss_probe); i++) {
        if (bss_probe[i] != 0) {
            KEXPECT_EQ(bss_probe[i], 0);
            return;
        }
    }
}

KTEST(memory_map_is_sane)
{
    const struct boot_info *bi = boot_info_get();
    bool usable_above_1m = false;

    for (u32 i = 0; i < bi->mmap_count; i++) {
        const struct e820_entry *e = &bi->mmap[i];

        KEXPECT(e->length != 0);
        KEXPECT(e->acpi_attrs & 1);             /* loader drops entries marked invalid */
        KEXPECT(e->base + e->length > e->base); /* no wraparound */
        if (e->type == E820_USABLE && e->base <= bi->kernel_phys_start &&
            e->base + e->length >= bi->kernel_phys_end)
            usable_above_1m = true;
    }
    KEXPECT(usable_above_1m); /* the kernel must sit in usable RAM */
}

KTEST(cpu_is_in_long_mode)
{
    KEXPECT(rdmsr(MSR_EFER) & EFER_LMA);
}

KTEST(a20_line_is_enabled)
{
    /*
     * With A20 off, physical 0x100500 aliases 0x500. 0x100500 is kernel text
     * (read-only in the direct map), so write two values to 0x500 and check
     * the high address never follows.
     */
    volatile u8 *low = phys_to_virt(0x500);
    volatile const u8 *high = phys_to_virt(0x100500);
    u8 saved = *low, before = *high;

    *low = (u8)~before;
    KEXPECT_EQ(*high, before);
    *low = before;
    KEXPECT_EQ(*high, before);
    *low = saved;
}

KTEST(loader_handed_over_with_interrupts_disabled)
{
    KEXPECT_EQ(boot_entry_rflags() & RFLAGS_IF, 0);
    KEXPECT_EQ(read_rflags() & RFLAGS_DF, 0); /* the ABI requires DF clear */
}
