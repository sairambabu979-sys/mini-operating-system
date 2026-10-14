// SPDX-License-Identifier: MIT
/*
 * Host test runner: runs every registered TEST() and prints TAP.
 */
#include "test.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static struct test_case *head;
static struct test_case **tail = &head;
static int current_failures;

void test_register(struct test_case *tc)
{
    /* Constructors run in link order; append to keep output stable. */
    *tail = tc;
    tail = &tc->next;
}

void test_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    current_failures++;
    printf("    # %s:%d: ", file, line);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

int main(int argc, char **argv)
{
    const char *filter = argc > 1 ? argv[1] : NULL;
    int n = 0, failed = 0;

    for (struct test_case *tc = head; tc; tc = tc->next)
        if (!filter || strstr(tc->name, filter))
            n++;

    printf("TAP version 13\n1..%d\n", n);

    int i = 0;

    for (struct test_case *tc = head; tc; tc = tc->next) {
        if (filter && !strstr(tc->name, filter))
            continue;
        current_failures = 0;
        tc->fn();
        i++;
        printf("%s %d - %s\n", current_failures ? "not ok" : "ok", i, tc->name);
        if (current_failures)
            failed++;
    }

    printf("# %d/%d passed\n", n - failed, n);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
