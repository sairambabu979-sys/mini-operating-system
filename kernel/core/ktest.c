// SPDX-License-Identifier: MIT
/*
 * In-kernel test runner. See <core/ktest.h>.
 */
#include <core/ktest.h>
#include <core/printk.h>

extern const struct ktest __ktest_start[], __ktest_end[];

static int current_failures;

void ktest_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    current_failures++;
    kprintf("    # %s:%d: ", file, line);
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");
}

int ktest_run_all(void)
{
    int total = (int)(__ktest_end - __ktest_start);
    int failed = 0;

    kprintf("TAP version 13\n1..%d\n", total);
    for (int i = 0; i < total; i++) {
        const struct ktest *t = &__ktest_start[i];

        current_failures = 0;
        t->fn();
        kprintf("%s %d - %s\n", current_failures ? "not ok" : "ok", i + 1, t->name);
        if (current_failures)
            failed++;
    }
    kprintf("# ktest: %d/%d passed\n", total - failed, total);
    return failed;
}
