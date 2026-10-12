/* SPDX-License-Identifier: MIT */
/*
 * Freestanding memory and string routines.
 *
 * The compiler may emit calls to memcpy/memmove/memset/memcmp on its own, so
 * these names are part of the freestanding ABI and must exist.
 */
#ifndef MINIOS_LIB_STRING_H
#define MINIOS_LIB_STRING_H

#include <types.h>

void *memcpy(void *restrict dst, const void *restrict src, usize n);
void *memmove(void *dst, const void *src, usize n);
void *memset(void *dst, int c, usize n);
int memcmp(const void *a, const void *b, usize n);

usize strlen(const char *s);
usize strnlen(const char *s, usize max);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, usize n);
char *strchr(const char *s, int c);

/*
 * Copies at most size-1 bytes and always NUL-terminates when size > 0.
 * Returns strlen(src), so truncation is detected by `ret >= size`.
 */
usize strlcpy(char *restrict dst, const char *restrict src, usize size);

#endif /* MINIOS_LIB_STRING_H */
