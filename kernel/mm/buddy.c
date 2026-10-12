// SPDX-License-Identifier: MIT
/*
 * Binary buddy allocator. See <mm/buddy.h>.
 */
#include <lib/errno.h>
#include <mm/buddy.h>

static u64 block_frames(unsigned order)
{
    return 1ull << order;
}

static void set_block(struct buddy *b, u64 pfn, unsigned order, enum page_state head)
{
    b->pages[pfn].state = (u8)head;
    b->pages[pfn].order = (u8)order;
    for (u64 i = 1; i < block_frames(order); i++)
        b->pages[pfn + i].state = PAGE_TAIL;
}

static void push_free(struct buddy *b, u64 pfn, unsigned order)
{
    set_block(b, pfn, order, PAGE_FREE_HEAD);
    list_push_front(&b->free_lists[order], &b->pages[pfn].link);
    b->free_blocks[order]++;
    b->free_frames += block_frames(order);
}

static void unlink_free(struct buddy *b, u64 pfn, unsigned order)
{
    list_remove(&b->pages[pfn].link);
    b->free_blocks[order]--;
    b->free_frames -= block_frames(order);
}

void buddy_init(struct buddy *b, struct page *pages, u64 nframes)
{
    b->pages = pages;
    b->nframes = nframes;
    b->free_frames = 0;
    b->managed_frames = 0;
    for (unsigned k = 0; k <= BUDDY_MAX_ORDER; k++) {
        list_init(&b->free_lists[k]);
        b->free_blocks[k] = 0;
    }
    for (u64 i = 0; i < nframes; i++) {
        list_init(&pages[i].link);
        pages[i].state = PAGE_RESERVED;
        pages[i].order = 0;
        pages[i].refcount = 0;
    }
}

/* Inserts a free block, merging with free buddies as far as possible. */
static void free_and_merge(struct buddy *b, u64 pfn, unsigned order)
{
    while (order < BUDDY_MAX_ORDER) {
        u64 buddy = pfn ^ block_frames(order);

        if (buddy + block_frames(order) > b->nframes)
            break;
        const struct page *bp = &b->pages[buddy];

        if (bp->state != PAGE_FREE_HEAD || bp->order != order)
            break;
        unlink_free(b, buddy, order);
        pfn &= ~block_frames(order); /* merged block starts at the lower buddy */
        order++;
    }
    push_free(b, pfn, order);
}

int buddy_add_range(struct buddy *b, u64 start, u64 end)
{
    if (start > end || end > b->nframes)
        return -EINVAL;
    for (u64 i = start; i < end; i++) {
        if (b->pages[i].state != PAGE_RESERVED)
            return -EINVAL; /* overlapping ranges */
    }

    /* Carve the range into the largest naturally aligned blocks. */
    while (start < end) {
        unsigned order = BUDDY_MAX_ORDER;

        while (order > 0 &&
               ((start & (block_frames(order) - 1)) != 0 || start + block_frames(order) > end))
            order--;
        b->managed_frames += block_frames(order);
        free_and_merge(b, start, order);
        start += block_frames(order);
    }
    return 0;
}

u64 buddy_alloc(struct buddy *b, unsigned order)
{
    unsigned k = order;

    if (order > BUDDY_MAX_ORDER)
        return BUDDY_NONE;
    while (k <= BUDDY_MAX_ORDER && list_empty(&b->free_lists[k]))
        k++;
    if (k > BUDDY_MAX_ORDER)
        return BUDDY_NONE;

    struct page *head = list_first_entry(&b->free_lists[k], struct page, link);
    u64 pfn = (u64)(head - b->pages);

    unlink_free(b, pfn, k);
    /* Split: keep the lower half, return upper halves to the free lists. */
    while (k > order) {
        k--;
        push_free(b, pfn + block_frames(k), k);
    }
    set_block(b, pfn, order, PAGE_ALLOC_HEAD);
    b->pages[pfn].refcount = 1;
    return pfn;
}

int buddy_free(struct buddy *b, u64 pfn, unsigned order)
{
    if (pfn >= b->nframes || order > BUDDY_MAX_ORDER)
        return -EINVAL;
    struct page *p = &b->pages[pfn];

    if (p->state != PAGE_ALLOC_HEAD || p->order != order)
        return -EINVAL;
    p->refcount = 0;
    free_and_merge(b, pfn, order);
    return 0;
}

int buddy_block_order(const struct buddy *b, u64 pfn)
{
    if (pfn >= b->nframes || b->pages[pfn].state != PAGE_ALLOC_HEAD)
        return -1;
    return b->pages[pfn].order;
}

int buddy_verify(const struct buddy *b, const char **reason)
{
    u64 free_frames = 0;
    u64 blocks[BUDDY_MAX_ORDER + 1] = {0};

#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        *reason = (msg);                                                                           \
        return -EINVAL;                                                                            \
    } while (0)

    /* Walk every frame: heads must be aligned and own exactly their tails. */
    for (u64 pfn = 0; pfn < b->nframes;) {
        const struct page *p = &b->pages[pfn];

        if (p->state == PAGE_RESERVED) {
            pfn++;
            continue;
        }
        if (p->state == PAGE_TAIL)
            FAIL("tail frame without a head");
        if (p->order > BUDDY_MAX_ORDER)
            FAIL("order out of range");
        u64 n = block_frames(p->order);

        if ((pfn & (n - 1)) != 0)
            FAIL("misaligned block");
        if (pfn + n > b->nframes)
            FAIL("block runs past the end");
        for (u64 i = 1; i < n; i++) {
            if (b->pages[pfn + i].state != PAGE_TAIL)
                FAIL("block interior is not tail frames");
        }
        if (p->state == PAGE_FREE_HEAD) {
            free_frames += n;
            blocks[p->order]++;
            /* Fully merged: a free block's buddy must not be free at the same order. */
            u64 buddy = pfn ^ n;

            if (p->order < BUDDY_MAX_ORDER && buddy + n <= b->nframes &&
                b->pages[buddy].state == PAGE_FREE_HEAD && b->pages[buddy].order == p->order)
                FAIL("two free buddies were not merged");
        }
        pfn += n;
    }

    /* Free lists must contain exactly the free heads, with matching orders. */
    for (unsigned k = 0; k <= BUDDY_MAX_ORDER; k++) {
        u64 listed = 0;

        list_for_each(node, &b->free_lists[k])
        {
            const struct page *p = list_entry(node, struct page, link);

            if (p->state != PAGE_FREE_HEAD || p->order != k)
                FAIL("free list holds a block of the wrong state or order");
            if (++listed > b->nframes)
                FAIL("free list is cyclic");
        }
        if (listed != blocks[k] || listed != b->free_blocks[k])
            FAIL("free-list length disagrees with frame states");
    }
    if (free_frames != b->free_frames)
        FAIL("free frame counter is wrong");
    if (b->free_frames > b->managed_frames)
        FAIL("more frames free than managed");
#undef FAIL
    *reason = NULL;
    return 0;
}
