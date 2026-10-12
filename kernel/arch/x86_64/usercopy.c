// SPDX-License-Identifier: MIT
/*
 * User copies. See <proc/uaccess.h>.
 *
 * rep movsb either completes or faults with RCX holding the bytes left;
 * the exception-table fixup lands just past it, so "bytes not copied" is
 * simply RCX afterwards. STAC/CLAC exist only on SMAP-capable CPUs, so
 * there are two variants.
 */
#include <arch/cpuid.h>
#include <arch/extable.h>
#include <lib/errno.h>
#include <proc/uaccess.h>

static usize raw_copy(void *dst, const void *src, usize n)
{
    if (cpu_features()->smap) {
        __asm__ volatile("stac\n"
                         "1: rep movsb\n"
                         "2: clac\n" EXTABLE_ENTRY(1b, 2b)
                         : "+D"(dst), "+S"(src), "+c"(n)
                         :
                         : "memory");
    } else {
        __asm__ volatile("1: rep movsb\n"
                         "2:\n" EXTABLE_ENTRY(1b, 2b)
                         : "+D"(dst), "+S"(src), "+c"(n)
                         :
                         : "memory");
    }
    return n;
}

int copy_from_user(void *dst, const void *user_src, usize len)
{
    if (!user_range_ok((uptr)user_src, len))
        return -EFAULT;
    return raw_copy(dst, user_src, len) == 0 ? 0 : -EFAULT;
}

int copy_to_user(void *user_dst, const void *src, usize len)
{
    if (!user_range_ok((uptr)user_dst, len))
        return -EFAULT;
    return raw_copy(user_dst, src, len) == 0 ? 0 : -EFAULT;
}

int strncpy_from_user(char *dst, const char *user_src, usize max)
{
    /* Byte at a time: the string may end right before an unmapped page. */
    for (usize i = 0; i < max; i++) {
        if (copy_from_user(&dst[i], user_src + i, 1) != 0)
            return -EFAULT;
        if (dst[i] == '\0')
            return (int)i;
    }
    return -ENAMETOOLONG;
}
