// SPDX-License-Identifier: MIT
/*
 * In-kernel test runner. See <core/ktest.h>.
 */
#include <core/ktest.h>
#include <core/printk.h>

extern const struct ktest __ktest_start[], __ktest_end[];

static int current_failures;
static const char *current_skip;

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

void ktest_skip(const char *reason)
{
    current_skip = reason;
}

int ktest_run_all(void)
{
    int total = (int)(__ktest_end - __ktest_start);
    int failed = 0, skipped = 0;

    kprintf("TAP version 13\n1..%d\n", total);
    for (int i = 0; i < total; i++) {
        const struct ktest *t = &__ktest_start[i];

        current_failures = 0;
        current_skip = NULL;
        t->fn();
        if (current_skip && !current_failures) {
            kprintf("ok %d - %s # SKIP %s\n", i + 1, t->name, current_skip);
            skipped++;
            continue;
        }
        kprintf("%s %d - %s\n", current_failures ? "not ok" : "ok", i + 1, t->name);
        if (current_failures)
            failed++;
    }
    kprintf("# ktest: %d/%d passed, %d skipped\n", total - failed - skipped, total, skipped);
    return failed;
}
