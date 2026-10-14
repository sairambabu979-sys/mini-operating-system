// SPDX-License-Identifier: MIT
#include <arch/cpu.h>
#include <arch/fpu.h>
#include <arch/paging.h>
#include <core/panic.h>
#include <lib/string.h>
#include <mm/kmalloc.h>

#define MXCSR_DEFAULT 0x1F80u /* all SSE exceptions masked, round to nearest */

static u8 initial_state[FPU_STATE_SIZE] __aligned(16);

void fpu_init(void)
{
    write_cr0((read_cr0() & ~(CR0_EM | CR0_TS)) | CR0_MP);
    write_cr4(read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT);

    u32 mxcsr = MXCSR_DEFAULT;

    __asm__ volatile("fninit\n\t"
                     "ldmxcsr %0"
                     :
                     : "m"(mxcsr));
    fpu_save(initial_state);
}

void *fpu_state_alloc(void)
{
    void *state = kmalloc(FPU_STATE_SIZE);

    if (state) {
        BUG_ON(!IS_ALIGNED((uptr)state, 16)); /* FXSAVE faults on misalignment */
        memcpy(state, initial_state, FPU_STATE_SIZE);
    }
    return state;
}
