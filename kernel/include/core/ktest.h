/* SPDX-License-Identifier: MIT */
/*
 * In-kernel test framework.
 *
 *     KTEST(pmm_alloc_free) {
 *         KEXPECT(page != NULL);
 *         KEXPECT_EQ(stats.free, before);
 *     }
 *
 * Each KTEST() places a descriptor in the .ktest section; the linker script
 * collects them between __ktest_start and __ktest_end. Booting with
 * "ktest" on the command line runs every test and reports TAP on the
 * console, which tests/qemu/run_tests.py parses.
 */
#ifndef MINIOS_CORE_KTEST_H
#define MINIOS_CORE_KTEST_H

#include <compiler.h>
#include <types.h>

struct ktest {
    const char *name;
    const char *file;
    void (*fn)(void);
};

#define KTEST(test_name)                                                                           \
    static void ktest_fn_##test_name(void);                                                        \
    static const struct ktest ktest_desc_##test_name __used __section(".ktest")                    \
        __aligned(8) = {.name = #test_name, .file = __FILE__, .fn = ktest_fn_##test_name};         \
    static void ktest_fn_##test_name(void)

void ktest_fail(const char *file, int line, const char *fmt, ...) __printf(3, 4);

#define KEXPECT(cond)                                                                              \
    do {                                                                                           \
        if (!(cond))                                                                               \
            ktest_fail(__FILE__, __LINE__, "expected: %s", #cond);                                 \
    } while (0)

#define KEXPECT_EQ(a, b)                                                                           \
    do {                                                                                           \
        u64 ka_ = (u64)(a), kb_ = (u64)(b);                                                        \
        if (ka_ != kb_)                                                                            \
            ktest_fail(__FILE__, __LINE__, "%s == %s: %#llx != %#llx", #a, #b,                     \
                       (unsigned long long)ka_, (unsigned long long)kb_);                          \
    } while (0)

/* Runs every registered test. Returns the number of failed tests. */
int ktest_run_all(void);

#endif /* MINIOS_CORE_KTEST_H */
