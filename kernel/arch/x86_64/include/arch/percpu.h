/* SPDX-License-Identifier: MIT */
/*
 * Per-CPU data, reached through GS in kernel mode.
 *
 * The syscall entry needs a kernel stack before it may touch any memory
 * through RSP, and GS (after swapgs) is the only register it can use to
 * find one. C code accesses the struct directly; only assembly uses GS.
 */
#ifndef MINIOS_ARCH_PERCPU_H
#define MINIOS_ARCH_PERCPU_H

#include <types.h>

struct percpu {
    u64 kernel_rsp; /* top of the current task's kernel stack */
    u64 user_rsp;   /* scratch: user RSP during syscall entry */
};

extern struct percpu percpu;

/* Sets GS_BASE to &percpu and KERNEL_GS_BASE to 0 (the initial user GS). */
void percpu_init(void);

#endif /* MINIOS_ARCH_PERCPU_H */
