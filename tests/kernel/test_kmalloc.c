// SPDX-License-Identifier: MIT
#include <arch/setjmp.h>
#include <core/ktest.h>
#include <core/panic.h>
#include <lib/string.h>
#include <mm/kmalloc.h>
#include <mm/pmm.h>

KTEST(kmalloc_objects_are_aligned_disjoint_and_writable)
{
    enum { N = 64 };
    u8 *ptrs[N];
    usize sizes[N];

    for (int i = 0; i < N; i++) {
        sizes[i] = 1 + (usize)(i * 37 % 3000);
        ptrs[i] = kmalloc(sizes[i]);
        KEXPECT(ptrs[i] != NULL);
        KEXPECT_EQ((uptr)ptrs[i] % 16, 0);
        memset(ptrs[i], i, sizes[i]);
    }
    /* Any overlap would have clobbered another object's pattern. */
    for (int i = 0; i < N; i++) {
        for (usize j = 0; j < sizes[i]; j++) {
            if (ptrs[i][j] != (u8)i) {
                KEXPECT_EQ(ptrs[i][j], (u8)i);
                break;
            }
        }
        kfree(ptrs[i]);
    }
}

KTEST(kmalloc_reuses_freed_objects)
{
    void *a = kmalloc(64);

    kfree(a);
    void *b = kmalloc(64);

    KEXPECT(a == b); /* LIFO free list */
    kfree(b);
}

KTEST(kmalloc_large_allocations_come_from_the_pmm)
{
    struct pmm_stats before, after;

    pmm_get_stats(&before);
    u8 *p = kmalloc(5ul * 4096);

    KEXPECT(p != NULL);
    KEXPECT_EQ((uptr)p % 4096, 0);
    memset(p, 0xAB, 5ul * 4096);
    kfree(p);
    pmm_get_stats(&after);
    KEXPECT_EQ(after.free_frames, before.free_frames);
}

KTEST(kcalloc_detects_overflow_and_zeroes)
{
    KEXPECT(kcalloc((usize)1 << 62, 8) == NULL);

    u32 *z = kcalloc(100, sizeof(u32));

    KEXPECT(z != NULL);
    for (int i = 0; i < 100; i++)
        KEXPECT_EQ(z[i], 0);
    kfree(z);
}

KTEST(kmalloc_stats_return_to_baseline)
{
    struct kmalloc_stats before, after;
    void *p[10];

    kmalloc_get_stats(&before);
    for (int i = 0; i < 10; i++)
        p[i] = kmalloc(100);
    kmalloc_get_stats(&after);
    KEXPECT_EQ(after.small_live, before.small_live + 10);
    for (int i = 0; i < 10; i++)
        kfree(p[i]);
    kmalloc_get_stats(&after);
    KEXPECT_EQ(after.small_live, before.small_live);
}

KTEST(kfree_rejects_non_heap_pointers)
{
    static u64 not_heap;
    struct kjmp_buf jb;
    struct panic_expectation e;

    panic_expect(&e, &jb);
    if (ksetjmp(&jb) == 0)
        kfree(&not_heap); // NOLINT(clang-analyzer-unix.Malloc): misuse is the test
    panic_expect_clear();
    KEXPECT(e.hit);
}

#ifdef CONFIG_DEBUG

KTEST(kmalloc_detects_double_free)
{
    struct kjmp_buf jb;
    struct panic_expectation e;
    void *p = kmalloc(32);

    kfree(p);
    panic_expect(&e, &jb);
    if (ksetjmp(&jb) == 0)
        kfree(p); // NOLINT(clang-analyzer-unix.Malloc): the double free is the test
    panic_expect_clear();
    KEXPECT(e.hit);
    KEXPECT(strncmp(e.message, "kfree: double free", 18) == 0);
}

KTEST(kmalloc_detects_use_after_free)
{
    struct kjmp_buf jb;
    struct panic_expectation e;
    u8 *p = kmalloc(48); /* 64-byte class */

    kfree(p);
    p[20] = 0x42; // NOLINT(clang-analyzer-unix.Malloc): the write after free is the test

    panic_expect(&e, &jb);
    if (ksetjmp(&jb) == 0) {
        void *q = kmalloc(48); /* reuses p: poison check fires */

        kfree(q);
    }
    panic_expect_clear();
    KEXPECT(e.hit);
    KEXPECT(strncmp(e.message, "kmalloc: use after free", 23) == 0);

    p[20] = KMALLOC_POISON; // NOLINT(clang-analyzer-unix.Malloc): repair the poison
    kfree(kmalloc(48));
}

KTEST(kmalloc_detects_heap_overflow)
{
    struct kjmp_buf jb;
    struct panic_expectation e;
    u8 *p = kmalloc(40);

    memset(p, 0x11, 40); /* the whole allocation is fine to use */
    p[40] = 0x22;        // NOLINT(clang-analyzer-security.ArrayBound): one byte too far: the test
    panic_expect(&e, &jb);
    if (ksetjmp(&jb) == 0)
        kfree(p);
    panic_expect_clear();
    KEXPECT(e.hit);
    KEXPECT(strncmp(e.message, "kfree: heap overflow", 20) == 0);

    /* The caught kfree() panicked before freeing; repair, then free for real. */
    p[40] = KMALLOC_REDZONE; // NOLINT(clang-analyzer-unix.Malloc): not freed, see above
    kfree(p);
}

KTEST(kmalloc_exact_size_use_is_not_an_overflow)
{
    /* Every size up to a class boundary, written in full, frees cleanly. */
    for (usize n = 1; n <= 300; n++) {
        u8 *p = kmalloc(n);

        memset(p, 0xEE, n);
        kfree(p);
    }
}

#endif /* CONFIG_DEBUG */
