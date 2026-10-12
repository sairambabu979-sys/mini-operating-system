/* SPDX-License-Identifier: MIT */
/*
 * Prototypes of kernel routines as renamed by kernel_shim.h, so tests can
 * use the host libc and the kernel implementation side by side.
 */
#ifndef MINIOS_TESTS_HOST_KERNEL_API_H
#define MINIOS_TESTS_HOST_KERNEL_API_H

#include <stddef.h>

void *kmemcpy(void *restrict dst, const void *restrict src, size_t n);
void *kmemmove(void *dst, const void *src, size_t n);
void *kmemset(void *dst, int c, size_t n);
int kmemcmp(const void *a, const void *b, size_t n);
size_t kstrlen(const char *s);
size_t kstrnlen(const char *s, size_t max);
int kstrcmp(const char *a, const char *b);
int kstrncmp(const char *a, const char *b, size_t n);
char *kstrchr(const char *s, int c);
size_t kstrlcpy(char *restrict dst, const char *restrict src, size_t size);

#endif /* MINIOS_TESTS_HOST_KERNEL_API_H */
