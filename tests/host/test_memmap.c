// SPDX-License-Identifier: MIT
#include "test.h"

#include <mm/memmap.h>

#include <stdlib.h>
#include <string.h>

#define E(b, l, t)                                                                                 \
    {                                                                                              \
        .base = (b), .length = (l), .type = (t), .acpi_attrs = 1                                   \
    }

static usize sanitize(const struct e820_entry *in, usize n, struct mem_region *out)
{
    return memmap_sanitize(in, n, out, 64);
}

TEST(memmap_sorts_and_merges_adjacent)
{
    const struct e820_entry in[] = {
        E(0x200000, 0x100000, E820_USABLE),
        E(0x100000, 0x100000, E820_USABLE),
        E(0x0, 0x9F000, E820_USABLE),
    };
    struct mem_region out[64];

    EXPECT_EQ(sanitize(in, 3, out), 2);
    EXPECT_EQ(out[0].base, 0);
    EXPECT_EQ(out[0].end, 0x9F000);
    EXPECT_EQ(out[1].base, 0x100000); /* two adjacent usable entries merged */
    EXPECT_EQ(out[1].end, 0x300000);
}

TEST(memmap_overlap_resolves_to_more_restrictive)
{
    /* Usable RAM with a reserved hole punched through its middle. */
    const struct e820_entry in[] = {
        E(0x100000, 0x400000, E820_USABLE),
        E(0x200000, 0x1000, E820_RESERVED),
    };
    struct mem_region out[64];

    EXPECT_EQ(sanitize(in, 2, out), 3);
    EXPECT_EQ(out[0].type, E820_USABLE);
    EXPECT_EQ(out[0].end, 0x200000);
    EXPECT_EQ(out[1].type, E820_RESERVED);
    EXPECT_EQ(out[1].base, 0x200000);
    EXPECT_EQ(out[1].end, 0x201000);
    EXPECT_EQ(out[2].type, E820_USABLE);
    EXPECT_EQ(out[2].base, 0x201000);
    EXPECT_EQ(out[2].end, 0x500000);
}

TEST(memmap_usable_regions_shrink_to_whole_pages)
{
    const struct e820_entry in[] = {
        E(0x1001, 0x3000, E820_USABLE),  /* -> [0x2000, 0x4000) */
        E(0x10000, 0x800, E820_USABLE),  /* less than a page: dropped */
        E(0x20001, 0x10, E820_RESERVED), /* reserved stays byte-exact */
    };
    struct mem_region out[64];

    EXPECT_EQ(sanitize(in, 3, out), 2);
    EXPECT_EQ(out[0].base, 0x2000);
    EXPECT_EQ(out[0].end, 0x4000);
    EXPECT_EQ(out[1].type, E820_RESERVED);
    EXPECT_EQ(out[1].base, 0x20001);
}

TEST(memmap_unknown_types_become_reserved_and_zero_lengths_vanish)
{
    const struct e820_entry in[] = {
        E(0x0, 0x1000, 12), /* e.g. persistent memory, type 12 */
        E(0x5000, 0, E820_USABLE),
    };
    struct mem_region out[64];

    EXPECT_EQ(sanitize(in, 2, out), 1);
    EXPECT_EQ(out[0].type, E820_RESERVED);
}

TEST(memmap_handles_regions_ending_at_top_of_address_space)
{
    const struct e820_entry in[] = {
        E(0xFFFFFFFFFFFFF001ull, 0x2000, E820_USABLE), /* wraps: saturates, then drops */
        E(0xFD00000000ull, 0x300000000ull, E820_RESERVED),
    };
    struct mem_region out[64];
    usize n = sanitize(in, 2, out);

    EXPECT_EQ(n, 1);
    EXPECT_EQ(out[0].type, E820_RESERVED);
    EXPECT_EQ(out[0].end, 0x10000000000ull);
}

TEST(memmap_output_is_sorted_and_disjoint_for_random_maps)
{
    unsigned seed = 42;

    for (int iter = 0; iter < 2000; iter++) {
        struct e820_entry in[16];
        struct mem_region out[64];
        int n = 1 + rand_r(&seed) % 16;

        for (int i = 0; i < n; i++) {
            in[i] = (struct e820_entry){
                .base = (u64)(rand_r(&seed) % 64) * 0x800,
                .length = (u64)(rand_r(&seed) % 16) * 0x800,
                .type = 1 + (u32)(rand_r(&seed) % 5),
                .acpi_attrs = 1,
            };
        }
        usize m = sanitize(in, (usize)n, out);

        for (usize i = 0; i < m; i++) {
            EXPECT_TRUE(out[i].base < out[i].end);
            if (i > 0)
                EXPECT_TRUE(out[i - 1].end <= out[i].base);
            if (out[i].type == E820_USABLE)
                EXPECT_TRUE(out[i].base % PAGE_SIZE == 0 && out[i].end % PAGE_SIZE == 0);
            /* Every usable byte must be covered by a usable input entry and no
             * more restrictive one: check the midpoint. */
            u64 mid = out[i].base + (out[i].end - out[i].base) / 2;
            int worst = -1;
            u32 want = 0;

            for (int j = 0; j < n; j++) {
                if (in[j].length && in[j].base <= mid && mid < in[j].base + in[j].length) {
                    int r = in[j].type == 1   ? 0
                            : in[j].type == 3 ? 1
                            : in[j].type == 4 ? 2
                            : in[j].type == 5 ? 4
                                              : 3;

                    if (r > worst) {
                        worst = r;
                        want = in[j].type;
                    }
                }
            }
            EXPECT_EQ(out[i].type, want);
        }
    }
}
