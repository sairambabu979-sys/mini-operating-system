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
