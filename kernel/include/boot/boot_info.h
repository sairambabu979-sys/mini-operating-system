/* SPDX-License-Identifier: MIT */
/*
 * Boot protocol: the contract between the stage-2 loader and the kernel.
 *
 * This header is the single source of truth. The loader's NASM view of the
 * layout is generated from it by kernel/asm-offsets.c, so the two cannot
 * drift apart. Bump BOOT_INFO_VERSION on any layout change.
 *
 * On entry to the kernel, RDI holds the physical address of a struct
 * boot_info located at BOOT_INFO_PHYS. The loader writes `magic` last, so
 * a valid magic implies every other field is filled in.
 */
#ifndef MINIOS_BOOT_BOOT_INFO_H
#define MINIOS_BOOT_BOOT_INFO_H

#include <compiler.h>
#include <types.h>

#define BOOT_INFO_MAGIC   0x4F464E49544F4F42ull /* "BOOTINFO" in little-endian */
#define BOOT_INFO_VERSION 1u
#define BOOT_INFO_PHYS    0x1000u /* fixed: one page in conventional memory */
#define BOOT_MMAP_MAX     128u
#define BOOT_CMDLINE_MAX  128u

/* E820 region types (ACPI 6.5, table 15.4). */
enum e820_type {
    E820_USABLE = 1,
    E820_RESERVED = 2,
    E820_ACPI_RECLAIMABLE = 3,
    E820_ACPI_NVS = 4,
    E820_BAD = 5,
};

/* One BIOS INT 15h/E820 descriptor, ACPI 3.0 extended form. */
struct e820_entry {
    u64 base;
    u64 length;
    u32 type;
    u32 acpi_attrs; /* bit 0 set = entry valid; loader drops invalid ones */
} __packed;

struct boot_info {
    u64 magic;
    u32 version;
    u32 mmap_count;                 /* entries used in mmap[], raw BIOS order */
    u64 kernel_phys_start;          /* lowest p_paddr of any PT_LOAD segment */
    u64 kernel_phys_end;            /* highest p_paddr + p_memsz, exclusive */
    u64 boot_drive;                 /* BIOS drive number (DL at boot) */
    char cmdline[BOOT_CMDLINE_MAX]; /* NUL-terminated */
    struct e820_entry mmap[BOOT_MMAP_MAX];
};

static_assert(sizeof(struct e820_entry) == 24, "E820 entries are 24 bytes (ACPI 3.0)");
static_assert(__builtin_offsetof(struct boot_info, mmap) % 8 == 0, "mmap must be 8-aligned");
static_assert(sizeof(struct boot_info) <= 4096, "boot_info must fit in its page");

#endif /* MINIOS_BOOT_BOOT_INFO_H */
