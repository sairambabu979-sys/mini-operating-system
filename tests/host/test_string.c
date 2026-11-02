// SPDX-License-Identifier: MIT
#include "kernel_api.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

static int sign(int v)
{
    return (v > 0) - (v < 0);
}

TEST(memcpy_copies_all_lengths)
{
    unsigned char src[300], dst[300];

    for (size_t i = 0; i < sizeof(src); i++)
        src[i] = (unsigned char)(i * 7 + 3);
    for (size_t n = 0; n <= sizeof(src); n++) {
        memset(dst, 0xAA, sizeof(dst));
        EXPECT_TRUE(kmemcpy(dst, src, n) == dst);
        EXPECT_EQ(memcmp(dst, src, n), 0);
        if (n < sizeof(dst))
            EXPECT_EQ(dst[n], 0xAA); /* no overrun */
    }
}

TEST(memmove_handles_overlap_both_directions)
{
    for (int shift = -16; shift <= 16; shift++) {
        for (size_t n = 0; n < 64; n++) {
            unsigned char expect[128], got[128];

            for (size_t i = 0; i < sizeof(got); i++)
                expect[i] = got[i] = (unsigned char)i;
            size_t base = 32;

            memmove(expect + base + shift, expect + base, n);
            EXPECT_TRUE(kmemmove(got + base + shift, got + base, n) == got + base + shift);
            EXPECT_EQ(memcmp(expect, got, sizeof(got)), 0);
        }
    }
}

TEST(memmove_leaves_direction_flag_clear)
{
    char buf[16] = "abcdefgh";
#ifdef __x86_64__
    unsigned long flags;
#endif

    kmemmove(buf + 1, buf, 8); /* backward path uses std/cld */
#ifdef __x86_64__
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    EXPECT_EQ(flags & (1ul << 10), 0);
#endif
    EXPECT_STREQ(buf, "aabcdefgh");
}

TEST(memset_fills_exact_range)
{
    unsigned char buf[64];

    memset(buf, 0, sizeof(buf));
    EXPECT_TRUE(kmemset(buf + 8, 0x1FF, 16) == buf + 8); /* value truncated to u8 */
    for (size_t i = 0; i < sizeof(buf); i++)
        EXPECT_EQ(buf[i], (i >= 8 && i < 24) ? 0xFF : 0);
}

TEST(memcmp_orders_as_unsigned_bytes)
{
    EXPECT_EQ(kmemcmp("abc", "abc", 3), 0);
    EXPECT_EQ(sign(kmemcmp("abc", "abd", 3)), -1);
    EXPECT_EQ(sign(kmemcmp("\x80", "\x01", 1)), 1);
    EXPECT_EQ(kmemcmp("x", "y", 0), 0);
}

TEST(strlen_and_strnlen)
{
    EXPECT_EQ(kstrlen(""), 0);
    EXPECT_EQ(kstrlen("kernel"), 6);
    EXPECT_EQ(kstrnlen("kernel", 3), 3);
    EXPECT_EQ(kstrnlen("kernel", 100), 6);
}

TEST(strcmp_family_matches_libc_sign)
{
    const char *cases[][2] = {
        {"", ""}, {"a", ""}, {"", "a"}, {"abc", "abd"}, {"abc", "ab"}, {"\xff", "a"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        EXPECT_EQ(sign(kstrcmp(cases[i][0], cases[i][1])), sign(strcmp(cases[i][0], cases[i][1])));
        for (size_t n = 0; n < 4; n++)
            EXPECT_EQ(sign(kstrncmp(cases[i][0], cases[i][1], n)),
                      sign(strncmp(cases[i][0], cases[i][1], n)));
    }
}

TEST(strchr_finds_terminator)
{
    const char *s = "a=b";

    EXPECT_TRUE(kstrchr(s, '=') == s + 1);
    EXPECT_TRUE(kstrchr(s, '\0') == s + 3);
    EXPECT_TRUE(kstrchr(s, 'z') == NULL);
}

TEST(strlcpy_truncates_and_terminates)
{
    char buf[4];

    EXPECT_EQ(kstrlcpy(buf, "ab", sizeof(buf)), 2);
    EXPECT_STREQ(buf, "ab");
    EXPECT_EQ(kstrlcpy(buf, "abcdef", sizeof(buf)), 6);
    EXPECT_STREQ(buf, "abc");
    buf[0] = 'X';
    EXPECT_EQ(kstrlcpy(buf, "abc", 0), 3);
    EXPECT_EQ(buf[0], 'X'); /* size 0 writes nothing */
}
