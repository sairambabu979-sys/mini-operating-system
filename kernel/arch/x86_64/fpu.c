// SPDX-License-Identifier: MIT
#include <arch/cpu.h>
#include <arch/cpuid.h>
#include <arch/fpu.h>
#include <arch/paging.h>
#include <core/panic.h>
#include <lib/string.h>
#include <mm/kmalloc.h>

#define MXCSR_DEFAULT      0x1F80u /* all SSE exceptions masked, round to nearest */
#define FXSAVE_REGS_OFFSET 32u     /* ST0-7 from here, then XMM0-15, then reserved */

static u8 initial_state[FPU_STATE_SIZE] __aligned(16);

void fpu_init(void)
{
    /*
     * NE, because the BIOS path inherits it clear: x87 errors would go to
     * FERR#/IRQ 13, which nothing handles, instead of #MF, which kills the
     * faulting process. MCE, so a machine check reaches the #MC handler
     * (on its own IST stack) instead of resetting the machine silently.
     */
    write_cr0((read_cr0() & ~(CR0_EM | CR0_TS)) | CR0_MP | CR0_NE);
    write_cr4(read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT | (cpu_features()->mce ? CR4_MCE : 0));

    u32 mxcsr = MXCSR_DEFAULT;

    __asm__ volatile("fninit\n\t"
                     "ldmxcsr %0"
                     :
                     : "m"(mxcsr));
    fpu_save(initial_state);
    /*
     * fninit resets the x87 control words but not the register contents,
     * and nothing resets XMM0-15: they hold whatever the firmware left
     * (OVMF uses SSE). The template every process starts from must not
     * carry that, so clear ST0-7, XMM0-15 and the reserved tail.
     */
    memset((u8 *)initial_state + FXSAVE_REGS_OFFSET, 0, FPU_STATE_SIZE - FXSAVE_REGS_OFFSET);
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
