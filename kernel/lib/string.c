// SPDX-License-Identifier: MIT
/*
 * Memory and string routines.
 *
 * The bulk copy/fill paths use `rep movsb`/`rep stosb`, which are fast on any
 * CPU with ERMSB and, unlike C loops, cannot be pattern-matched back into a
 * call to the very function being defined. They also run unchanged in host
 * unit tests, since the host is x86_64 as well.
 */
#include <lib/string.h>

void *memcpy(void *restrict dst, const void *restrict src, usize n)
{
    void *ret = dst;

#ifdef __x86_64__
    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
#else
    for (usize i = 0; i < n; i++)
        ((u8 *)dst)[i] = ((const u8 *)src)[i];
#endif
    return ret;
}

void *memmove(void *dst, const void *src, usize n)
{
    u8 *d = dst;
    const u8 *s = src;

    if (d == s || n == 0)
        return dst;

    if (d < s || d >= s + n) {
        /* No harmful overlap: forward copy is safe. */
#ifdef __x86_64__
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
#else
        for (usize i = 0; i < n; i++)
            d[i] = s[i];
#endif
    } else {
        /* dst overlaps the tail of src: copy backwards. */
#ifdef __x86_64__
        d += n - 1;
        s += n - 1;
        __asm__ volatile("std\n\t"
                         "rep movsb\n\t"
                         "cld"
                         : "+D"(d), "+S"(s), "+c"(n)
                         :
                         : "memory");
#else
        while (n) {
            n--;
            d[n] = s[n];
        }
#endif
    }
    return dst;
}

void *memset(void *dst, int c, usize n)
{
    void *ret = dst;

#ifdef __x86_64__
    __asm__ volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"((u8)c) : "memory");
#else
    for (usize i = 0; i < n; i++)
        ((u8 *)dst)[i] = (u8)c;
#endif
    return ret;
}

int memcmp(const void *a, const void *b, usize n)
{
    const u8 *pa = a;
    const u8 *pb = b;

    for (usize i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return pa[i] < pb[i] ? -1 : 1;
    }
    return 0;
}

usize strlen(const char *s)
{
    const char *p = s;

    while (*p)
        p++;
    return (usize)(p - s);
}

usize strnlen(const char *s, usize max)
{
    usize n = 0;

    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, usize n)
{
    for (; n > 0; n--, a++, b++) {
        if (*a != *b)
            return (int)(u8)*a - (int)(u8)*b;
        if (*a == '\0')
            break;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (*s == '\0')
            return NULL;
    }
}

usize strlcpy(char *restrict dst, const char *restrict src, usize size)
{
    usize len = strlen(src);

    if (size > 0) {
        usize n = len < size - 1 ? len : size - 1;

        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}
