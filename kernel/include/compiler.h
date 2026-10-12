/* SPDX-License-Identifier: MIT */
/*
 * Compiler abstraction: attributes, builtins and small generic helpers.
 */
#ifndef MINIOS_COMPILER_H
#define MINIOS_COMPILER_H

#define __packed                    __attribute__((packed))
#define __aligned(x)                __attribute__((aligned(x)))
#define __noreturn                  __attribute__((noreturn))
#define __section(s)                __attribute__((section(s)))
#define __used                      __attribute__((used))
#define __maybe_unused              __attribute__((unused))
#define __must_check                __attribute__((warn_unused_result))
#define __printf(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))
/* glibc defines this too; only matters when kernel code is host-tested. */
#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#ifndef static_assert
#define static_assert _Static_assert
#endif

/* Compiler-only barrier: forbids reordering memory accesses across it. */
#define barrier() __asm__ volatile("" ::: "memory")

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Power-of-two alignment helpers. `a` must be a power of two. */
#define ALIGN_DOWN(x, a) ((x) & ~((__typeof__(x))(a) - 1))
#define ALIGN_UP(x, a)   ALIGN_DOWN((x) + ((__typeof__(x))(a) - 1), (a))
#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* Integer arithmetic: the member's alignment already implies the container's. */
#define container_of(ptr, type, member) ((type *)((uptr)(ptr) - __builtin_offsetof(type, member)))

#endif /* MINIOS_COMPILER_H */
