// SPDX-License-Identifier: MIT
/*
 * The kernel printf engine is checked against glibc's snprintf, which is the
 * reference implementation of C17 7.21.6.1 for every supported conversion.
 */
#include "test.h"

#include <lib/printf.h>

/*
 * These tests deliberately use flag combinations the C standard defines but
 * -Wformat flags as suspicious (e.g. '0' with a precision), plus invalid
 * conversions to check how errors are echoed.
 */
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-extra-args"
#pragma GCC diagnostic ignored "-Wformat-overflow"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Formats with both implementations and compares output and return value. */
#define CHECK(fmt, ...)                                                                            \
    do {                                                                                           \
        char want_[256], got_[256];                                                                \
        int wn_ = snprintf(want_, sizeof(want_), fmt, __VA_ARGS__);                                \
        int gn_ = k_snprintf(got_, sizeof(got_), fmt, __VA_ARGS__);                                \
        EXPECT_STREQ(got_, want_);                                                                 \
        EXPECT_EQ(gn_, wn_);                                                                       \
    } while (0)

TEST(printf_signed_integers)
{
    const long long values[] = {0, 1, -1, 42, -42, INT_MAX, INT_MIN, LLONG_MAX, LLONG_MIN};

    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        long long v = values[i];

        CHECK("%lld|%+lld|% lld|%20lld|%-20lld|%020lld|%.25lld|%25.20lld", v, v, v, v, v, v, v, v);
        CHECK("%d|%+5d|%-5d|%05d|%.0d|%5.3d", (int)v, (int)v, (int)v, (int)v, (int)v, (int)v);
        CHECK("%hhd|%hd|%ld|%zd|%td|%jd", (signed char)v, (short)v, (long)v, (ssize_t)v,
              (ptrdiff_t)v, (intmax_t)v);
    }
}

TEST(printf_unsigned_bases)
{
    const unsigned long long values[] = {0, 1, 7, 8, 255, 0xdeadbeef, ULLONG_MAX};

    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        unsigned long long v = values[i];

        CHECK("%llu|%llo|%llx|%llX", v, v, v, v);
        CHECK("%#llo|%#llx|%#llX|%#.0llo|%#.0llx", v, v, v, v, v);
        CHECK("%#20llx|%#-20llx|%#020llx|%020.5llx", v, v, v, v);
        CHECK("%hhu|%hu|%u|%lu|%zu|%ju", (unsigned char)v, (unsigned short)v, (unsigned)v,
              (unsigned long)v, (size_t)v, (uintmax_t)v);
    }
}

TEST(printf_strings_and_chars)
{
    CHECK("[%s]|[%10s]|[%-10s]|[%.2s]|[%10.3s]", "kernel", "kernel", "kernel", "kernel", "kernel");
    CHECK("[%s]|[%.0s]", "", "abc");
    CHECK("[%c]|[%3c]|[%-3c]", 'x', 'y', 'z');
    CHECK("%%|%s%%", "done");
}

TEST(printf_star_width_and_precision)
{
    CHECK("[%*d]|[%-*d]|[%*d]", 6, 42, 6, 42, -6, 42);
    CHECK("[%.*d]|[%.*s]|[%.*d]", 4, 7, 3, "abcdef", -1, 9);
    CHECK("[%*.*x]", 10, 4, 0xab);
}

TEST(printf_pointer_is_fixed_width)
{
    char buf[64];

    k_snprintf(buf, sizeof(buf), "%p", (void *)0x1234);
    EXPECT_STREQ(buf, "0x0000000000001234");
    k_snprintf(buf, sizeof(buf), "%p", (void *)0xffffffff80100000ull);
    EXPECT_STREQ(buf, "0xffffffff80100000");
}

TEST(printf_truncation_semantics)
{
    char buf[8];

    memset(buf, 'X', sizeof(buf));
    EXPECT_EQ(k_snprintf(buf, sizeof(buf), "%s", "0123456789"), 10);
    EXPECT_STREQ(buf, "0123456");

    memset(buf, 'X', sizeof(buf));
    EXPECT_EQ(k_snprintf(buf, 0, "%d", 12345), 5);
    EXPECT_EQ(buf[0], 'X'); /* size 0 never writes */

    EXPECT_EQ(k_snprintf(buf, 1, "abc"), 3);
    EXPECT_STREQ(buf, "");
}

TEST(printf_null_string_and_unknown_conversion)
{
    char buf[64];
    const char *null_str = NULL;

    k_snprintf(buf, sizeof(buf), "%s", null_str);
    EXPECT_STREQ(buf, "(null)");

    /* Unknown conversions are echoed so formatting bugs are visible. */
    EXPECT_EQ(k_snprintf(buf, sizeof(buf), "a%qb"), 4);
    EXPECT_STREQ(buf, "a%qb");
    EXPECT_EQ(k_snprintf(buf, sizeof(buf), "end%"), 4);
    EXPECT_STREQ(buf, "end%");
}

/* Randomized differential test: many format/value pairs against glibc. */
TEST(printf_randomized_against_libc)
{
    static const char *const flags[] = {"", "-", "+", " ", "#", "0", "-+", "#0", "+0", " -"};
    static const char *const convs[] = {"d", "i", "u", "x", "X", "o"};
    unsigned seed = 0x5eed;

    for (int iter = 0; iter < 20000; iter++) {
        char fmt[32];
        unsigned r = (unsigned)rand_r(&seed);
        int width = (int)(r % 24) - 1;
        int prec = (int)((r >> 5) % 24) - 1;
        const char *conv = convs[(r >> 10) % 6];
        const char *flag = flags[(r >> 13) % 10];
        long long v = (long long)(((unsigned long long)rand_r(&seed) << 33) ^
                                  ((unsigned long long)rand_r(&seed) << 2) ^ r);
        char wbuf[8] = "", pbuf[8] = "";

        if (width >= 0)
            snprintf(wbuf, sizeof(wbuf), "%d", width);
        if (prec >= 0)
            snprintf(pbuf, sizeof(pbuf), ".%d", prec);
        snprintf(fmt, sizeof(fmt), "%%%s%s%sll%s", flag, wbuf, pbuf, conv);

        char want[128], got[128];
        int wn = snprintf(want, sizeof(want), fmt, v);
        int gn = k_snprintf(got, sizeof(got), fmt, v);

        if (wn != gn || strcmp(want, got) != 0) {
            test_fail(__FILE__, __LINE__, "fmt \"%s\" v=%lld: want \"%s\" got \"%s\"", fmt, v, want,
                      got);
            return;
        }
    }
}
