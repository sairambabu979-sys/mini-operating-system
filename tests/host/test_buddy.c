// SPDX-License-Identifier: MIT
#include "test.h"

#include <lib/errno.h>
#include <mm/buddy.h>

#include <stdlib.h>
#include <string.h>

static struct buddy *make(u64 nframes)
{
    struct buddy *b = calloc(1, sizeof(*b));
    struct page *pages = calloc(nframes, sizeof(*pages));

    buddy_init(b, pages, nframes);
    return b;
}

static void destroy(struct buddy *b)
{
    free(b->pages);
    free(b);
}

static void expect_consistent(const struct buddy *b)
{
    const char *why = NULL;

    if (buddy_verify(b, &why) != 0)
        test_fail(__FILE__, __LINE__, "buddy_verify: %s", why);
}

TEST(buddy_carves_ranges_into_aligned_blocks)
{
    struct buddy *b = make(4096);

    /* [3, 1029): 1 + 4 + 8 + ... up to an order-10 block and a tail. */
    EXPECT_EQ(buddy_add_range(b, 3, 1029), 0);
    EXPECT_EQ(b->free_frames, 1026);
    EXPECT_EQ(b->managed_frames, 1026);
    expect_consistent(b);
    destroy(b);
}

TEST(buddy_split_and_merge_restore_the_initial_state)
{
    struct buddy *b = make(1024);

    EXPECT_EQ(buddy_add_range(b, 0, 1024), 0);
    EXPECT_EQ(b->free_blocks[BUDDY_MAX_ORDER], 1);

    u64 a = buddy_alloc(b, 0);

    EXPECT_EQ(a, 0);
    /* One order-10 block split into one free block of each order 0..9. */
    for (unsigned k = 0; k < BUDDY_MAX_ORDER; k++)
        EXPECT_EQ(b->free_blocks[k], 1);
    expect_consistent(b);

    EXPECT_EQ(buddy_free(b, a, 0), 0);
    EXPECT_EQ(b->free_blocks[BUDDY_MAX_ORDER], 1); /* everything merged back */
    EXPECT_EQ(b->free_frames, 1024);
    expect_consistent(b);
    destroy(b);
}

TEST(buddy_allocations_are_aligned_and_disjoint)
{
    struct buddy *b = make(2048);
    unsigned char *owner = calloc(2048, 1);

    EXPECT_EQ(buddy_add_range(b, 0, 2048), 0);
    for (unsigned order = 0; order <= 4; order++) {
        for (int i = 0; i < 8; i++) {
            u64 pfn = buddy_alloc(b, order);

            EXPECT_TRUE(pfn != BUDDY_NONE);
            EXPECT_EQ(pfn % (1u << order), 0);
            for (u64 f = pfn; f < pfn + (1u << order); f++) {
                EXPECT_EQ(owner[f], 0);
                owner[f] = 1;
            }
        }
    }
    expect_consistent(b);
    free(owner);
    destroy(b);
}

TEST(buddy_exhaustion_returns_none)
{
    struct buddy *b = make(16);

    EXPECT_EQ(buddy_add_range(b, 0, 16), 0);
    EXPECT_TRUE(buddy_alloc(b, 5) == BUDDY_NONE); /* larger than all memory */
    for (int i = 0; i < 16; i++)
        EXPECT_TRUE(buddy_alloc(b, 0) != BUDDY_NONE);
    EXPECT_TRUE(buddy_alloc(b, 0) == BUDDY_NONE);
    EXPECT_EQ(b->free_frames, 0);
    expect_consistent(b);
    destroy(b);
}

TEST(buddy_rejects_bad_frees)
{
    struct buddy *b = make(64);

    EXPECT_EQ(buddy_add_range(b, 0, 64), 0);
    u64 p = buddy_alloc(b, 2);

    EXPECT_EQ(buddy_free(b, p, 1), -EINVAL);     /* wrong order */
    EXPECT_EQ(buddy_free(b, p + 1, 2), -EINVAL); /* interior frame */
    EXPECT_EQ(buddy_free(b, 1000, 0), -EINVAL);  /* out of range */
    EXPECT_EQ(buddy_free(b, p, 2), 0);
    EXPECT_EQ(buddy_free(b, p, 2), -EINVAL); /* double free */
    expect_consistent(b);
    destroy(b);
}

TEST(buddy_never_hands_out_reserved_frames)
{
    struct buddy *b = make(256);

    /* Two islands with a reserved gap; merging must not bridge the gap. */
    EXPECT_EQ(buddy_add_range(b, 0, 100), 0);
    EXPECT_EQ(buddy_add_range(b, 160, 256), 0);
    EXPECT_EQ(buddy_add_range(b, 90, 170), -EINVAL); /* overlaps managed frames */

    u64 pfn;

    while ((pfn = buddy_alloc(b, 0)) != BUDDY_NONE)
        EXPECT_TRUE(pfn < 100 || pfn >= 160);
    EXPECT_EQ(b->free_frames, 0);
    expect_consistent(b);
    destroy(b);
}

/* Randomized alloc/free with full verification after every step. */
TEST(buddy_fuzz_random_alloc_free)
{
    enum { NFRAMES = 3000, MAX_LIVE = 256 };
    struct buddy *b = make(NFRAMES);
    struct {
        u64 pfn;
        unsigned order;
    } live[MAX_LIVE];
    int nlive = 0;
    unsigned seed = 1234;

    EXPECT_EQ(buddy_add_range(b, 7, 1500), 0);
    EXPECT_EQ(buddy_add_range(b, 1600, NFRAMES), 0);
    u64 total = b->free_frames;

    for (int step = 0; step < 20000; step++) {
        bool do_alloc = nlive == 0 || (nlive < MAX_LIVE && rand_r(&seed) % 2);

        if (do_alloc) {
            unsigned order = (unsigned)(rand_r(&seed) % 6);
            u64 pfn = buddy_alloc(b, order);

            if (pfn != BUDDY_NONE)
                live[nlive++] = (typeof(live[0])){pfn, order};
        } else {
            int i = rand_r(&seed) % nlive;

            EXPECT_EQ(buddy_free(b, live[i].pfn, live[i].order), 0);
            live[i] = live[--nlive];
        }
        if (step % 97 == 0)
            expect_consistent(b);
    }
    while (nlive > 0) {
        nlive--;
        EXPECT_EQ(buddy_free(b, live[nlive].pfn, live[nlive].order), 0);
    }
    EXPECT_EQ(b->free_frames, total);
    expect_consistent(b);
    destroy(b);
}
