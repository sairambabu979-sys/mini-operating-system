/* SPDX-License-Identifier: MIT */
/*
 * Non-local jumps within the kernel (callee-saved registers, RSP, RIP).
 *
 * Used to recover from faults that cannot be resumed in place, such as a
 * double fault deliberately provoked by a test. Never jump across a
 * context switch: the saved stack must still be live.
 */
#ifndef MINIOS_ARCH_SETJMP_H
#define MINIOS_ARCH_SETJMP_H

#include <compiler.h>
#include <types.h>

struct kjmp_buf {
    u64 rbx, rbp, r12, r13, r14, r15, rsp, rip;
};

/* Returns 0 when called, or `value` (never 0) when resumed by klongjmp. */
__attribute__((returns_twice)) int ksetjmp(struct kjmp_buf *buf);
__noreturn void klongjmp(struct kjmp_buf *buf, int value);

/* Runs fn(arg) with RSP = stack_top, then switches back. */
void call_on_stack(void (*fn)(void *), void *arg, void *stack_top);

#endif /* MINIOS_ARCH_SETJMP_H */
