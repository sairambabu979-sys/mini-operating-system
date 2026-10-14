// SPDX-License-Identifier: MIT
/*
 * Generates <asm-offsets.inc>: structure offsets and constants for NASM.
 *
 * This file is compiled to assembly only (never linked). Each DEFINE()
 * leaves a "->NAME value" marker in the output, which the build turns into
 * "%define NAME value". The technique is borrowed from Linux; it lets
 * assembly use C layouts without anyone keeping two copies in sync.
 */
#include <arch/percpu.h>
#include <arch/segments.h>
#include <arch/setjmp.h>
#include <arch/trap.h>
#include <boot/boot_info.h>
#include <sched/sched.h>

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
    OFFSET(BI_LOADER, boot_info, loader);
    DEFINE(BOOT_LOADER_BIOS, BOOT_LOADER_BIOS);
    OFFSET(BI_CMDLINE, boot_info, cmdline);
    OFFSET(BI_MMAP, boot_info, mmap);
    OFFSET(E820_BASE, e820_entry, base);
    OFFSET(E820_LENGTH, e820_entry, length);
    OFFSET(E820_TYPE, e820_entry, type);
    OFFSET(E820_ACPI, e820_entry, acpi_attrs);

    /* Trap frame (arch/trap.h) */
    DEFINE(TF_SIZE, sizeof(struct trap_frame));
    OFFSET(TF_VECTOR, trap_frame, vector);
    OFFSET(TF_ERROR_CODE, trap_frame, error_code);
    OFFSET(TF_RIP, trap_frame, rip);
    OFFSET(TF_CS, trap_frame, cs);
    OFFSET(TF_RFLAGS, trap_frame, rflags);
    OFFSET(TF_RSP, trap_frame, rsp);
    OFFSET(TF_SS, trap_frame, ss);
    DEFINE(TRAP_VECTOR_SYSCALL, TRAP_VECTOR_SYSCALL);
    DEFINE(USER_CS, GDT_USER_CODE | RPL_USER);
    DEFINE(USER_SS, GDT_USER_DATA | RPL_USER);

    /* Per-CPU area (arch/percpu.h) */
    OFFSET(PERCPU_KERNEL_RSP, percpu, kernel_rsp);
    OFFSET(PERCPU_USER_RSP, percpu, user_rsp);

    /* Non-local jumps (arch/setjmp.h) */
    OFFSET(KJMP_RBX, kjmp_buf, rbx);
    OFFSET(KJMP_RBP, kjmp_buf, rbp);
    OFFSET(KJMP_R12, kjmp_buf, r12);
    OFFSET(KJMP_R13, kjmp_buf, r13);
    OFFSET(KJMP_R14, kjmp_buf, r14);
    OFFSET(KJMP_R15, kjmp_buf, r15);
    OFFSET(KJMP_RSP, kjmp_buf, rsp);
    OFFSET(KJMP_RIP, kjmp_buf, rip);

    /* Tasks (sched/sched.h) */
    OFFSET(TASK_SAVED_RSP, task, saved_rsp);
}
