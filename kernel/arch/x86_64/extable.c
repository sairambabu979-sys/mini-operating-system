// SPDX-License-Identifier: MIT
/*
 * Exception table lookup. See <arch/extable.h>.
 *
 * A linear scan: the table holds a handful of entries (user-copy routines
 * and test probes), and it is consulted only on faults, never on a hot path.
 */
#include <arch/extable.h>

extern const struct extable_entry __extable_start[], __extable_end[];

uptr extable_find(uptr rip)
{
    for (const struct extable_entry *e = __extable_start; e < __extable_end; e++) {
        if (e->fault_rip == rip)
            return e->fixup_rip;
    }
    return 0;
}

bool probe_read_u64(uptr addr, u64 *out)
{
    u64 value = 0;
    u32 ok = 1;

    /* On a fault the fixup clears `ok` and resumes after the load. */
    __asm__ volatile("1: movq (%[addr]), %[value]\n"
                     "2:\n"
                     ".pushsection .text.fixup, \"ax\"\n"
                     "3: xorl %k[ok], %k[ok]\n"
                     "   jmp 2b\n"
                     ".popsection\n" EXTABLE_ENTRY(1b, 3b)
                     : [value] "+r"(value), [ok] "+r"(ok)
                     : [addr] "r"(addr)
                     : "memory");
    *out = value;
    return ok != 0;
}
