/* SPDX-License-Identifier: MIT */
/*
 * Access to user memory from the kernel.
 *
 * The only sanctioned way to touch user memory. Each call checks that the
 * whole range lies in the user half (so a process cannot make the kernel
 * read or write kernel memory on its behalf), lifts SMAP for the copy
 * alone, and turns faults (unmapped or protected pages) into -EFAULT
 * through the exception table instead of a kernel panic.
 */
#ifndef MINIOS_PROC_UACCESS_H
#define MINIOS_PROC_UACCESS_H

#include <compiler.h>
#include <types.h>

#define USER_TOP 0x0000800000000000ull /* exclusive: first non-canonical address */

static inline bool user_range_ok(uptr addr, usize len)
{
    return addr + len >= addr && addr + len <= USER_TOP;
}

__must_check int copy_from_user(void *dst, const void *user_src, usize len);
__must_check int copy_to_user(void *user_dst, const void *src, usize len);

/* Copies a NUL-terminated string of at most max-1 characters.
 * Returns its length, -EFAULT, or -ENAMETOOLONG. */
__must_check int strncpy_from_user(char *dst, const char *user_src, usize max);

#endif /* MINIOS_PROC_UACCESS_H */
