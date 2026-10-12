// SPDX-License-Identifier: MIT
/*
 * Generates <asm-offsets.inc>: structure offsets and constants for NASM.
 *
 * This file is compiled to assembly only (never linked). Each DEFINE()
 * leaves a "->NAME value" marker in the output, which the build turns into
 * "%define NAME value". The technique is borrowed from Linux; it lets
 * assembly use C layouts without anyone keeping two copies in sync.
 */
#include <boot/boot_info.h>

#define DEFINE(sym, val)      __asm__ volatile("\n.ascii \"->" #sym " %c0\"" : : "i"((u64)(val)))
#define OFFSET(sym, str, mem) DEFINE(sym, __builtin_offsetof(struct str, mem))

void asm_offsets(void);

void asm_offsets(void)
{
    /* Boot protocol (boot/boot_info.h) */
    DEFINE(BOOT_INFO_MAGIC, BOOT_INFO_MAGIC);
    DEFINE(BOOT_INFO_VERSION, BOOT_INFO_VERSION);
    DEFINE(BOOT_INFO_PHYS, BOOT_INFO_PHYS);
    DEFINE(BOOT_INFO_SIZE, sizeof(struct boot_info));
    DEFINE(BOOT_MMAP_MAX, BOOT_MMAP_MAX);
    DEFINE(BOOT_CMDLINE_MAX, BOOT_CMDLINE_MAX);
    DEFINE(E820_ENTRY_SIZE, sizeof(struct e820_entry));
    DEFINE(E820_USABLE, E820_USABLE);
    OFFSET(BI_MAGIC, boot_info, magic);
    OFFSET(BI_VERSION, boot_info, version);
    OFFSET(BI_MMAP_COUNT, boot_info, mmap_count);
    OFFSET(BI_KERNEL_PHYS_START, boot_info, kernel_phys_start);
    OFFSET(BI_KERNEL_PHYS_END, boot_info, kernel_phys_end);
    OFFSET(BI_BOOT_DRIVE, boot_info, boot_drive);
    OFFSET(BI_CMDLINE, boot_info, cmdline);
    OFFSET(BI_MMAP, boot_info, mmap);
    OFFSET(E820_BASE, e820_entry, base);
    OFFSET(E820_LENGTH, e820_entry, length);
    OFFSET(E820_TYPE, e820_entry, type);
    OFFSET(E820_ACPI, e820_entry, acpi_attrs);
}
