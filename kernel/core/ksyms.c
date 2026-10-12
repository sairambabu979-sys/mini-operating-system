// SPDX-License-Identifier: MIT
/*
 * Symbol lookup over the generated table (see <core/ksyms.h>).
 */
#include <core/ksyms.h>
#include <core/sections.h>
#include <lib/string.h>

/* Defined by the generated build/<config>/gen/ksyms.c. */
extern const u32 ksyms_count;
extern const uptr ksyms_addrs[];       /* sorted ascending */
extern const u32 ksyms_name_offsets[]; /* into ksyms_names */
extern const char ksyms_names[];

const char *ksym_lookup(uptr addr, uptr *offset)
{
    if (ksyms_count == 0 || addr < ksyms_addrs[0] || addr >= (uptr)__text_end)
        return NULL;

    /* Binary search for the last symbol starting at or below addr. */
    u32 lo = 0, hi = ksyms_count;

    while (hi - lo > 1) {
        u32 mid = lo + (hi - lo) / 2;

        if (ksyms_addrs[mid] <= addr)
            lo = mid;
        else
            hi = mid;
    }
    *offset = addr - ksyms_addrs[lo];
    return &ksyms_names[ksyms_name_offsets[lo]];
}

uptr ksym_address(const char *name)
{
    for (u32 i = 0; i < ksyms_count; i++) {
        if (strcmp(&ksyms_names[ksyms_name_offsets[i]], name) == 0)
            return ksyms_addrs[i];
    }
    return 0;
}
