/* SPDX-License-Identifier: MIT */
/*
 * Kernel symbol table, for symbolized backtraces.
 *
 * Generated at link time by tools/gen_ksyms.py (a two-pass link: the table
 * lives in .rodata, after .text, so adding it never moves a function).
 */
#ifndef MINIOS_CORE_KSYMS_H
#define MINIOS_CORE_KSYMS_H

#include <types.h>

/*
 * Finds the function containing `addr`. Returns its name and stores the
 * offset into it in *offset, or returns NULL if `addr` is not kernel text.
 */
const char *ksym_lookup(uptr addr, uptr *offset);

/* Inverse lookup, mainly for tests. Returns 0 if not found. */
uptr ksym_address(const char *name);

#endif /* MINIOS_CORE_KSYMS_H */
