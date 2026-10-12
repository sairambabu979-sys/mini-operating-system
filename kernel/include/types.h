/* SPDX-License-Identifier: MIT */
/*
 * Fixed-width integer types used throughout the kernel.
 *
 * Only the compiler-provided freestanding headers are used (-nostdinc keeps
 * libc headers out); everything else is defined by the kernel itself.
 */
#ifndef MINIOS_TYPES_H
#define MINIOS_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef uintptr_t uptr;
typedef size_t usize;
typedef ptrdiff_t sptr;

#endif /* MINIOS_TYPES_H */
