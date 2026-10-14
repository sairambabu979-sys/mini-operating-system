/* SPDX-License-Identifier: MIT */
/*
 * Exception table: maps instruction addresses that are allowed to fault to
 * the address execution resumes at. A fault at a listed RIP in kernel mode
 * is not a bug; it is how code that touches untrusted memory (user copies,
 * probes) reports failure without checking every address up front.
 */
#ifndef MINIOS_ARCH_EXTABLE_H
#define MINIOS_ARCH_EXTABLE_H

#include <types.h>

struct extable_entry {
    uptr fault_rip;
    uptr fixup_rip;
};

/*
 * Emits an entry from inline assembly. Usage:
 *
 *     asm volatile("1: movq (%1), %0\n"
 *                  "2:\n"
 *                  EXTABLE_ENTRY(1b, 2b) : ...);
 */
#define EXTABLE_ENTRY(fault, fixup)                                                                \
    ".pushsection __ex_table, \"a\"\n\t"                                                           \
    ".balign 8\n\t"                                                                                \
    ".quad " #fault ", " #fixup "\n\t"                                                             \
    ".popsection\n\t"

/* Returns the fixup address for `rip`, or 0 if the fault is not expected. */
uptr extable_find(uptr rip);

/*
 * Reads 8 bytes from an address that may be unmapped. Returns false instead
 * of faulting. For diagnostics (backtraces) over possibly-corrupt pointers.
 */
bool probe_read_u64(uptr addr, u64 *out);

#endif /* MINIOS_ARCH_EXTABLE_H */
