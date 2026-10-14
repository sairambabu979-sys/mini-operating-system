/* SPDX-License-Identifier: MIT */
/*
 * Intrusive circular doubly-linked list.
 *
 * A list_head is both the sentinel of a list and the link embedded in each
 * element; container_of() recovers the element. No allocation, O(1)
 * insert/remove, and removal needs no knowledge of the list.
 */
#ifndef MINIOS_LIB_LIST_H
#define MINIOS_LIB_LIST_H

#include <compiler.h>
#include <types.h>

struct list_head {
    struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name)                                                                       \
    {                                                                                              \
        &(name), &(name)                                                                           \
    }

static inline void list_init(struct list_head *h)
{
    h->next = h;
    h->prev = h;
}

static inline bool list_empty(const struct list_head *h)
{
    return h->next == h;
}

static inline void list_insert_between(struct list_head *n, struct list_head *prev,
                                       struct list_head *next)
{
    n->prev = prev;
    n->next = next;
    prev->next = n;
    next->prev = n;
}

/* Inserts n at the front of the list headed by h. */
static inline void list_push_front(struct list_head *h, struct list_head *n)
{
    list_insert_between(n, h, h->next);
}

/* Inserts n at the back of the list headed by h. */
static inline void list_push_back(struct list_head *h, struct list_head *n)
{
    list_insert_between(n, h->prev, h);
}

/* Unlinks n and leaves it self-linked, so a double removal is harmless. */
static inline void list_remove(struct list_head *n)
{
    n->prev->next = n->next;
    n->next->prev = n->prev;
    list_init(n);
}

#define list_entry(ptr, type, member)     container_of(ptr, type, member)
#define list_first_entry(h, type, member) list_entry((h)->next, type, member)

/* `pos` names the loop variable, so it cannot be parenthesized. */
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define list_for_each(pos, h) for (struct list_head *pos = (h)->next; pos != (h); pos = pos->next)

#endif /* MINIOS_LIB_LIST_H */
