/* SPDX-License-Identifier: MIT */
/*
 * Minimal unit-test harness for host-side tests of pure kernel modules.
 *
 * Tests self-register through a constructor, so adding a test is just:
 *
 *     TEST(printf_basic) { EXPECT_STREQ(...); }
 *
 * Results are reported as TAP (https://testanything.org), the same format
 * the in-kernel test framework uses, so one parser handles both.
 */
#ifndef MINIOS_TESTS_HOST_TEST_H
#define MINIOS_TESTS_HOST_TEST_H

#include <stdio.h>

struct test_case {
    const char *name;
    void (*fn)(void);
    struct test_case *next;
};

void test_register(struct test_case *tc);
void test_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define TEST(name)                                                                                 \
    static void test_fn_##name(void);                                                              \
    static struct test_case test_case_##name = {#name, test_fn_##name, NULL};                      \
    __attribute__((constructor)) static void test_reg_##name(void)                                 \
    {                                                                                              \
        test_register(&test_case_##name);                                                          \
    }                                                                                              \
    static void test_fn_##name(void)

#define EXPECT_TRUE(cond)                                                                          \
    do {                                                                                           \
        if (!(cond))                                                                               \
            test_fail(__FILE__, __LINE__, "expected true: %s", #cond);                             \
    } while (0)

#define EXPECT_EQ(a, b)                                                                            \
    do {                                                                                           \
        long long va_ = (long long)(a), vb_ = (long long)(b);                                      \
        if (va_ != vb_)                                                                            \
            test_fail(__FILE__, __LINE__, "%s == %s: %lld != %lld", #a, #b, va_, vb_);             \
    } while (0)

#define EXPECT_STREQ(a, b)                                                                         \
    do {                                                                                           \
        const char *sa_ = (a), *sb_ = (b);                                                         \
        if (strcmp(sa_, sb_) != 0)                                                                 \
            test_fail(__FILE__, __LINE__, "%s == %s: \"%s\" != \"%s\"", #a, #b, sa_, sb_);         \
    } while (0)

#endif /* MINIOS_TESTS_HOST_TEST_H */
