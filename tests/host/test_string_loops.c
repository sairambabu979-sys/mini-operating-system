// SPDX-License-Identifier: MIT
/*
 * kernel/lib/string.c's plain loops, which hosts other than x86_64 (an
 * Apple Silicon Mac) build instead of rep movsb/stosb. Compiled here a
 * second time with the loops forced on and every name renamed (l...), so
 * an x86_64 host, CI's, tests them too, against the C library.
 */
#define HOST_LOOPS 1
#define memcpy     lmemcpy
#define memmove    lmemmove
#define memset     lmemset
#define memcmp     lmemcmp
#define strlen     lstrlen
#define strnlen    lstrnlen
#define strcmp     lstrcmp
#define strncmp    lstrncmp
#define strchr     lstrchr
#define strlcpy    lstrlcpy
#include "../../kernel/lib/string.c"
#undef memcpy
#undef memmove
#undef memset
#undef memcmp
#undef strlen
#undef strnlen
#undef strcmp
#undef strncmp
#undef strchr
#undef strlcpy

#include "test.h"

#include <string.h>

TEST(string_loops_copy_fill_and_move_like_libc)
{
    unsigned char src[200], want[264], got[264];

    for (size_t i = 0; i < sizeof(src); i++)
        src[i] = (unsigned char)(i * 13 + 5);
    for (size_t n = 0; n <= sizeof(src); n += 7) {
        memset(got, 0xAA, sizeof(got));
        EXPECT_TRUE(lmemcpy(got, src, n) == got);
        EXPECT_EQ(memcmp(got, src, n), 0);
        EXPECT_EQ(got[n], 0xAA); /* no overrun */
        EXPECT_TRUE(lmemset(got, 0x5C, n) == got);
        for (size_t i = 0; i < n; i++)
            EXPECT_EQ(got[i], 0x5C);
        EXPECT_EQ(got[n], 0xAA);
    }
    for (int shift = -16; shift <= 16; shift++) {
        for (size_t n = 0; n < 64; n += 3) {
            for (size_t i = 0; i < sizeof(got); i++)
                want[i] = got[i] = (unsigned char)i;
            memmove(want + 100 + shift, want + 100, n);
            EXPECT_TRUE(lmemmove(got + 100 + shift, got + 100, n) == got + 100 + shift);
            EXPECT_EQ(memcmp(want, got, sizeof(got)), 0);
        }
    }
}
