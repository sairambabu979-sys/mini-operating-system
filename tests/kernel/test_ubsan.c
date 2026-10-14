// SPDX-License-Identifier: MIT
/*
 * Kernel UBSan is compiled in only for debug builds.
 */
#include <core/ktest.h>
#include <core/ubsan.h>

#ifdef CONFIG_DEBUG

#include <lib/string.h>

KTEST(ubsan_reports_signed_overflow)
{
    struct ubsan_expectation e;
    volatile int x = __INT_MAX__;

    ubsan_expect(&e);
    x = x + 1;
    ubsan_expect_clear();
    KEXPECT_EQ(e.hits, 1);
    KEXPECT(e.kind && strcmp(e.kind, "integer overflow in addition") == 0);
}

KTEST(ubsan_reports_out_of_bounds_index)
{
    struct ubsan_expectation e;
    /*
     * UBSan reports and then lets the access happen, so the out-of-bounds
     * slot must be real memory: `spare` absorbs the write. (An earlier
     * version wrote past a bare stack array and the stack protector caught
     * the smashed canary, which is the other safety net working.)
     */
    struct {
        int array[4];
        int spare;
    } s = {{0}, 0};
    volatile int index = 4;

    ubsan_expect(&e);
    s.array[index] = 1;
    ubsan_expect_clear();
    KEXPECT_EQ(e.hits, 1);
    KEXPECT(e.kind && strcmp(e.kind, "array index out of bounds") == 0);
    KEXPECT_EQ(s.spare, 1);
}

KTEST(ubsan_reports_oversized_shift)
{
    struct ubsan_expectation e;
    volatile unsigned shift = 40;
    volatile unsigned value = 1;

    ubsan_expect(&e);
    value = value << shift; // NOLINT(clang-analyzer-core.BitwiseShift): the point of the test
    ubsan_expect_clear();
    KEXPECT_EQ(e.hits, 1);
    KEXPECT(e.kind && strcmp(e.kind, "shift out of bounds") == 0);
}

#endif /* CONFIG_DEBUG */
