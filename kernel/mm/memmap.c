// SPDX-License-Identifier: MIT
/*
 * Memory-map sanitizer. See <mm/memmap.h>.
 *
 * Boundary sweep: every entry contributes its start and end as cut points;
 * between consecutive cut points each byte has a single winning type.
 * O(n^2) over at most BOOT_MMAP_MAX entries, run once at boot.
 */
#include <compiler.h>
#include <mm/memmap.h>

#define MAX_BOUNDARIES (2 * BOOT_MMAP_MAX)

/* Higher wins when entries overlap. */
static int restrictiveness(u32 type)
{
    switch (type) {
    case E820_USABLE:
        return 0;
    case E820_ACPI_RECLAIMABLE:
        return 1;
    case E820_ACPI_NVS:
        return 2;
    case E820_BAD:
        return 4;
    case E820_RESERVED:
    default:
        return 3; /* unknown types are treated as reserved */
    }
}

static u32 normalize_type(u32 type)
{
    switch (type) {
    case E820_USABLE:
    case E820_ACPI_RECLAIMABLE:
    case E820_ACPI_NVS:
    case E820_BAD:
        return type;
    default:
        return E820_RESERVED;
    }
}

static void sort_u64(u64 *v, usize n)
{
    for (usize i = 1; i < n; i++) {
        u64 x = v[i];
        usize j = i;

        while (j > 0 && v[j - 1] > x) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
}

/* End of an entry, saturating instead of wrapping past 2^64. */
static u64 entry_end(const struct e820_entry *e)
{
    return e->base + e->length < e->base ? ~0ull : e->base + e->length;
}

usize memmap_sanitize(const struct e820_entry *in, usize n, struct mem_region *out, usize max)
{
    u64 cuts[MAX_BOUNDARIES];
    usize ncuts = 0;
    usize nout = 0;

    if (n > BOOT_MMAP_MAX)
        n = BOOT_MMAP_MAX;

    for (usize i = 0; i < n; i++) {
        if (in[i].length == 0)
            continue;
        cuts[ncuts++] = in[i].base;
        cuts[ncuts++] = entry_end(&in[i]);
    }
    sort_u64(cuts, ncuts);

    for (usize c = 0; c + 1 < ncuts; c++) {
        u64 lo = cuts[c], hi = cuts[c + 1];
        int best = -1;
        u32 type = 0;

        if (lo == hi)
            continue;
        for (usize i = 0; i < n; i++) {
            if (in[i].length == 0 || in[i].base > lo || entry_end(&in[i]) < hi)
                continue;
            int r = restrictiveness(in[i].type);

            if (r > best) {
                best = r;
                type = normalize_type(in[i].type);
            }
        }
        if (best < 0)
            continue; /* hole: not described by firmware, so not memory */

        /* Merge with the previous region when contiguous and same type. */
        if (nout > 0 && out[nout - 1].end == lo && out[nout - 1].type == type) {
            out[nout - 1].end = hi;
            continue;
        }
        if (nout == max)
            break;
        out[nout++] = (struct mem_region){.base = lo, .end = hi, .type = type};
    }

    /* Usable RAM shrinks inward to whole pages; slivers disappear. */
    usize w = 0;

    for (usize r = 0; r < nout; r++) {
        struct mem_region reg = out[r];

        if (reg.type == E820_USABLE) {
            if (reg.base > ~0ull - (PAGE_SIZE - 1))
                continue; /* rounding up would wrap past 2^64 */
            reg.base = ALIGN_UP(reg.base, PAGE_SIZE);
            reg.end = ALIGN_DOWN(reg.end, PAGE_SIZE);
            if (reg.base >= reg.end)
                continue;
        }
        out[w++] = reg;
    }
    return w;
}
