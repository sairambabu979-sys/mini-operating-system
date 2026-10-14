// SPDX-License-Identifier: MIT
/*
 * Architecture side of a context switch.
 */
#include <arch/fpu.h>
#include <arch/paging.h>
#include <arch/percpu.h>
#include <arch/segments.h>
#include <mm/kmalloc.h>
#include <mm/vmm.h>

#include "sched_internal.h"

void sched_arch_switch(struct task *prev, struct task *next)
{
    /* Ring 3 -> ring 0 transitions (interrupts via the TSS, syscalls via
     * percpu) land on the incoming task's kernel stack. */
    if (next->kstack_top) {
        tss_set_kernel_stack(next->kstack_top);
        percpu.kernel_rsp = next->kstack_top;
    }

    /* Kernel threads run on the kernel's own tables. */
    struct address_space *as = next->as ? next->as : vmm_kernel_space();

    if (read_cr3() != as->pml4_phys)
        write_cr3(as->pml4_phys);

    /* Only user tasks own FPU state; the kernel never touches the FPU. */
    if (prev->fpu_state)
        fpu_save(prev->fpu_state);
    if (next->fpu_state)
        fpu_restore(next->fpu_state);
}

void sched_arch_task_free(struct task *t)
{
    if (t->as)
        vmm_destroy_user_space(t->as);
    kfree(t->fpu_state);
}
