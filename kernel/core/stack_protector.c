// SPDX-License-Identifier: MIT
/*
 * Stack-smashing protector (-fstack-protector-strong, global guard).
 *
 * Functions with arrays or address-taken locals store __stack_chk_guard
 * below their return address and verify it before returning. _start
 * seeds the guard from the TSC before kmain runs. That makes it a bug
 * detector, not a security boundary, until a real entropy source exists.
 */
#include <core/panic.h>
#include <types.h>

/* Low byte zero: string overflows (strcpy-like) cannot reproduce it. */
u64 __stack_chk_guard = 0x5a17e2c0ffee5100ull;

__noreturn void __stack_chk_fail(void);

__noreturn void __stack_chk_fail(void)
{
    panic("stack smashing detected: canary overwritten");
}
