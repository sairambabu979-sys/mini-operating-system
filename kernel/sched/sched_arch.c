// SPDX-License-Identifier: MIT
/*
 * Architecture side of a context switch.
 */
#include <arch/fpu.h>
#include <arch/irqflags.h>
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

void sched_arch_task_exit(struct task *t)
{
    struct address_space *as = t->as;
    void *fpu_state = t->fpu_state;
    irqflags_t flags = arch_irq_save();

    /*
     * Detach first, then leave the tables: a preemption from here on
     * switches this task back in on the kernel's tables, never on `as`.
     */
    t->as = NULL;
    t->fpu_state = NULL;
    if (as)
        write_cr3(vmm_kernel_space()->pml4_phys);
    arch_irq_restore(flags);

    if (as)
        vmm_destroy_user_space(as);
    kfree(fpu_state);
}

void sched_arch_task_free(struct task *t)
{
    if (t->as)
        vmm_destroy_user_space(t->as);
    kfree(t->fpu_state);
}
