/* SPDX-License-Identifier: MIT */
/*
 * x87/SSE state for user tasks.
 *
 * The kernel itself never touches FPU registers (-mgeneral-regs-only,
 * checked by tools/check_kernel.py), so state is saved and restored only
 * when switching from or to a task that has an FPU area: user processes.
 */
#ifndef MINIOS_ARCH_FPU_H
#define MINIOS_ARCH_FPU_H

#include <compiler.h>
#include <types.h>

#define FPU_STATE_SIZE 512 /* FXSAVE area: 16-byte aligned */

/* Enables SSE (CR0/CR4) and captures the clean initial state. */
void fpu_init(void);

/* A fresh FXSAVE area holding the initial state; NULL on out-of-memory. */
__must_check void *fpu_state_alloc(void);

static inline void fpu_save(void *state)
{
    __asm__ volatile("fxsave64 (%0)" : : "r"(state) : "memory");
}

static inline void fpu_restore(const void *state)
{
    __asm__ volatile("fxrstor64 (%0)" : : "r"(state) : "memory");
}

#endif /* MINIOS_ARCH_FPU_H */
