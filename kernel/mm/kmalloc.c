// SPDX-License-Identifier: MIT
/*
 * Slab-style kernel heap. See <mm/kmalloc.h>.
 *
 * Slab page layout:
 *
 *     [struct slab header, 64 bytes][obj][obj][obj] ... [obj]
 *
 * Free objects form a singly linked list through their first word. Slab
 * objects are therefore never page-aligned, which is how kfree() tells
 * them apart from large (page-aligned) allocations without extra metadata.
 * Empty slabs are returned to the PMM, except one per class kept warm.
 *
 * Debug builds also give every small object a redzone: the size class is
 * chosen with room for >= 8 guard bytes plus a hidden u16 holding the
 * requested size at the object's very end. kfree() verifies the guard
 * bytes, so writing past the end of an allocation is reported (with the
 * offset) instead of silently corrupting the neighbouring object:
 *
 *     [ requested bytes ][ 0xCC redzone ... >= 8 bytes ][ u16 size ]
 *
 * Locking: heap_lock protects all slab state; it nests outside pmm_lock.
 * Misuse checks unlock before panicking so a caught panic (tests) leaves
 * the heap usable.
 */
#include <arch/paging.h>
#include <core/panic.h>
#include <core/spinlock.h>
#include <lib/list.h>
#include <lib/string.h>
#include <mm/kmalloc.h>
#include <mm/pmm.h>

#define SLAB_MAGIC   0x534C4142u /* "SLAB" */
#define POISON_FREE  KMALLOC_POISON
#define REDZONE_BYTE KMALLOC_REDZONE

#ifdef CONFIG_DEBUG
#define REDZONE_MIN    8
#define DEBUG_OVERHEAD (REDZONE_MIN + sizeof(u16))
#else
#define DEBUG_OVERHEAD 0
#endif
#define SLAB_HDR_SZ  64
#define NUM_CLASSES  8 /* 16, 32, ..., 2048 */
#define MIN_CLASS_SH 4

/* A free object's first word links it into its slab's free list. */
struct free_obj {
    struct free_obj *next;
};

struct slab {
    u32 magic;
    u16 class_idx;
    u16 free_count;
    u16 capacity;
    u16 reserved[3];
    struct free_obj *free_list;
    struct list_head link; /* in its class's partial or full list */
};

static_assert(sizeof(struct slab) <= SLAB_HDR_SZ, "slab header must fit its reserved space");

struct size_class {
    usize obj_size;
    struct list_head partial; /* slabs with at least one free object */
    struct list_head full;
    u64 slabs;
};

static struct size_class classes[NUM_CLASSES];
static struct spinlock heap_lock = SPINLOCK_INIT("kmalloc");
static u64 small_live, large_live;
static bool initialized;

/* Runs once, before the first allocation (single-threaded boot). */
static void init_classes(void)
{
    for (int i = 0; i < NUM_CLASSES; i++) {
        classes[i].obj_size = (usize)1 << (MIN_CLASS_SH + i);
        list_init(&classes[i].partial);
        list_init(&classes[i].full);
        classes[i].slabs = 0;
    }
    initialized = true;
}

static int class_for(usize size)
{
    for (int i = 0; i < NUM_CLASSES; i++) {
        if (size <= classes[i].obj_size)
            return i;
    }
    return -1;
}

#ifdef CONFIG_DEBUG
/* Byte 0..7 hold the free-list link; the rest must still be poison. */
static void check_poison(const struct slab *s, const void *obj, usize size, irqflags_t flags)
{
    const u8 *bytes = obj;

    for (usize i = sizeof(struct free_obj); i < size; i++) {
        if (bytes[i] != POISON_FREE) {
            spin_unlock_irqrestore(&heap_lock, flags);
            panic("kmalloc: use after free: %zu-byte object %p modified at offset %zu "
                  "(slab %p)",
                  size, obj, i, (const void *)s);
        }
    }
}
#endif

static struct slab *new_slab(int idx)
{
    u64 phys = pmm_alloc_page();

    if (phys == PMM_ALLOC_FAILED)
        return NULL;
    pmm_set_owner(phys, PMM_OWNER_SLAB); /* kfree trusts a slab header only on such pages */
    struct slab *s = phys_to_virt(phys);
    usize size = classes[idx].obj_size;
    u8 *first = (u8 *)s + SLAB_HDR_SZ;
    u16 cap = (u16)((PAGE_SIZE_4K - SLAB_HDR_SZ) / size);

    *s = (struct slab){.magic = SLAB_MAGIC, .class_idx = (u16)idx, .capacity = cap};
    /* Thread the free list in address order. */
    for (u16 i = cap; i-- > 0;) {
        struct free_obj *obj = (void *)(first + (usize)i * size);

#ifdef CONFIG_DEBUG
        memset(obj, POISON_FREE, size);
#endif
        obj->next = s->free_list;
        s->free_list = obj;
    }
    s->free_count = cap;
    classes[idx].slabs++;
    list_push_front(&classes[idx].partial, &s->link);
    return s;
}

/* `flags` is what heap_lock was taken with: debug checks unlock before panicking. */
static void *alloc_small(int idx, __maybe_unused irqflags_t flags)
{
    struct size_class *c = &classes[idx];
    struct slab *s;

    if (list_empty(&c->partial)) {
        s = new_slab(idx);
        if (!s)
            return NULL;
    } else {
        s = list_first_entry(&c->partial, struct slab, link);
    }

    struct free_obj *obj = s->free_list;

    /* Invariant: slabs on the partial list have a non-empty free list. */
    BUG_ON(obj == NULL);
#ifdef CONFIG_DEBUG
    /* Before unlinking, so a detected corruption leaves the slab intact. */
    check_poison(s, obj, c->obj_size, flags);
#endif
    s->free_list = obj->next;
    s->free_count--;
    if (s->free_count == 0) {
        list_remove(&s->link);
        list_push_front(&c->full, &s->link);
    }
    small_live++;
    return obj;
}

static unsigned order_for(usize size)
{
    unsigned order = 0;

    while ((PAGE_SIZE_4K << order) < size)
        order++;
    return order;
}

static long test_fail_in = -1; /* kmalloc_test_fail_one(): allocations before the one that fails */

long kmalloc_test_fail_one(long n)
{
    long previous = test_fail_in;

    test_fail_in = n;
    return previous;
}

void *kmalloc(usize size)
{
    if (!initialized)
        init_classes();
    /*
     * The largest buddy block is the limit. Checking first also keeps the
     * arithmetic below honest: size + DEBUG_OVERHEAD would wrap near
     * SIZE_MAX (a 16-byte object), and order_for() would never finish.
     */
    if (size == 0 || size > KMALLOC_MAX)
        return NULL;
    if (test_fail_in >= 0) {
        bool fail = test_fail_in == 0;

        test_fail_in--; /* -1 after the one that fails: off */
        if (fail)
            return NULL;
    }

    int idx = class_for(size + DEBUG_OVERHEAD);

    if (idx >= 0) {
        irqflags_t flags = spin_lock_irqsave(&heap_lock);
        u8 *p = alloc_small(idx, flags);

        spin_unlock_irqrestore(&heap_lock, flags);
#ifdef CONFIG_DEBUG
        if (p) {
            usize cls = classes[idx].obj_size;
            u16 requested = (u16)size;

            memset(p + size, REDZONE_BYTE, cls - sizeof(u16) - size);
            memcpy(p + cls - sizeof(u16), &requested, sizeof(requested));
        }
#endif
        return p;
    }

    unsigned order = order_for(size);

    if (order > BUDDY_MAX_ORDER)
        return NULL;
    u64 phys = pmm_alloc_pages(order);

    if (phys == PMM_ALLOC_FAILED)
        return NULL;
    pmm_set_owner(phys, PMM_OWNER_KMALLOC);
    __atomic_add_fetch(&large_live, 1, __ATOMIC_RELAXED);
    return phys_to_virt(phys);
}

void *kzalloc(usize size)
{
    void *p = kmalloc(size);

    if (p)
        memset(p, 0, size);
    return p;
}

void *kcalloc(usize n, usize size)
{
    usize total;

    if (__builtin_mul_overflow(n, size, &total))
        return NULL;
    return kzalloc(total);
}

static void free_small(struct slab *s, void *ptr, irqflags_t flags)
{
    struct size_class *c = &classes[s->class_idx];
    usize offset = (usize)((u8 *)ptr - ((u8 *)s + SLAB_HDR_SZ));
    struct free_obj *obj = ptr;

    if (offset % c->obj_size != 0 || offset / c->obj_size >= s->capacity) {
        spin_unlock_irqrestore(&heap_lock, flags);
        panic("kfree: %p is not an object boundary in its slab", ptr);
    }
#ifdef CONFIG_DEBUG
    for (struct free_obj *f = s->free_list; f; f = f->next) {
        if (f == obj) {
            spin_unlock_irqrestore(&heap_lock, flags);
            panic("kfree: double free of %p (%zu-byte class)", ptr, c->obj_size);
        }
    }

    /* Redzone: everything between the requested size and the size tail. */
    u16 requested;
    const u8 *bytes = ptr;

    memcpy(&requested, bytes + c->obj_size - sizeof(u16), sizeof(requested));
    if (requested > c->obj_size - DEBUG_OVERHEAD) {
        spin_unlock_irqrestore(&heap_lock, flags);
        panic("kfree: heap overflow: size tail of %p overwritten (%zu-byte class)", ptr,
              c->obj_size);
    }
    for (usize i = requested; i < c->obj_size - sizeof(u16); i++) {
        if (bytes[i] != REDZONE_BYTE) {
            spin_unlock_irqrestore(&heap_lock, flags);
            panic("kfree: heap overflow: %u-byte object %p written past its end at offset %zu",
                  requested, ptr, i);
        }
    }
    memset(obj, POISON_FREE, c->obj_size);
#endif
    bool was_full = s->free_count == 0;

    obj->next = s->free_list;
    s->free_list = obj;
    s->free_count++;
    small_live--;

    if (was_full) {
        list_remove(&s->link);
        list_push_front(&c->partial, &s->link);
    }
    /* Return empty slabs, but keep one per class to avoid thrashing. */
    if (s->free_count == s->capacity && c->slabs > 1) {
        list_remove(&s->link);
        s->magic = 0;
        c->slabs--;
        pmm_free_page(virt_to_phys_hhdm(s));
    }
}

void kfree(void *ptr)
{
    uptr va = (uptr)ptr;

    if (ptr == NULL)
        return;
    if (va < HHDM_BASE || va >= HHDM_BASE + HHDM_SIZE)
        panic("kfree: %p is not a heap pointer", ptr);

    if (IS_ALIGNED(va, PAGE_SIZE_4K)) {
        /* Large allocation: the PMM knows the block's order, and who allocated it. */
        int order = pmm_block_order(virt_to_phys_hhdm(ptr));

        if (order < 0)
            panic("kfree: %p is not a live allocation", ptr);
        if (pmm_block_owner(virt_to_phys_hhdm(ptr)) != PMM_OWNER_KMALLOC)
            panic("kfree: %p is a PMM block kmalloc did not allocate", ptr);
        __atomic_sub_fetch(&large_live, 1, __ATOMIC_RELAXED);
        pmm_free_pages(virt_to_phys_hhdm(ptr), (unsigned)order);
        return;
    }

    struct slab *s = (struct slab *)ALIGN_DOWN(va, PAGE_SIZE_4K);

    /*
     * The header is only data on the page: a user frame or a page table
     * can hold the same bytes. Only a page new_slab() tagged is a slab.
     * (This also turns a pointer into a hole in the direct map into a
     * clean panic, not a page fault.)
     */
    if (pmm_block_owner(virt_to_phys_hhdm(s)) != PMM_OWNER_SLAB)
        panic("kfree: %p is not in a slab page", ptr);

    irqflags_t flags = spin_lock_irqsave(&heap_lock);

    if (s->magic != SLAB_MAGIC || s->class_idx >= NUM_CLASSES) {
        spin_unlock_irqrestore(&heap_lock, flags);
        panic("kfree: %p is not in a slab (bad magic)", ptr);
    }
    free_small(s, ptr, flags);
    spin_unlock_irqrestore(&heap_lock, flags);
}

void kmalloc_get_stats(struct kmalloc_stats *out)
{
    irqflags_t flags = spin_lock_irqsave(&heap_lock);

    out->small_live = small_live;
    out->large_live = large_live;
    out->slabs = 0;
    for (int i = 0; i < NUM_CLASSES; i++)
        out->slabs += classes[i].slabs;
    spin_unlock_irqrestore(&heap_lock, flags);
}
