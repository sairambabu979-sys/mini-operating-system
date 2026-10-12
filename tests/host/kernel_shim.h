/* SPDX-License-Identifier: MIT */
/*
 * Force-included (-include) when compiling kernel sources for the host.
 *
 * Renames the kernel's libc-named symbols so they cannot collide with, or be
 * silently replaced by, the host libc. Tests call the k-prefixed names.
 */
#ifndef MINIOS_TESTS_HOST_KERNEL_SHIM_H
#define MINIOS_TESTS_HOST_KERNEL_SHIM_H

#define memcpy  kmemcpy
#define memmove kmemmove
#define memset  kmemset
#define memcmp  kmemcmp
#define strlen  kstrlen
#define strnlen kstrnlen
#define strcmp  kstrcmp
#define strncmp kstrncmp
#define strchr  kstrchr
#define strlcpy kstrlcpy

#endif /* MINIOS_TESTS_HOST_KERNEL_SHIM_H */
