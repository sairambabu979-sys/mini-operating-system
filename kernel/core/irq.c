// SPDX-License-Identifier: MIT
#define pr_fmt(fmt) "irq: " fmt

#include <core/irq.h>
#include <core/klog.h>
#include <drivers/pic8259.h>
#include <lib/errno.h>

struct irq_slot {
    irq_handler_t handler;
    void *ctx;
    u64 count;
};

static struct irq_slot slots[PIC_IRQ_LINES];
static u64 spurious;
static u32 irq_depth;

void irq_init(void)
{
    pic_init(VEC_FIRST_IRQ);
}

int irq_register(unsigned line, irq_handler_t handler, void *ctx)
{
    if (line >= PIC_IRQ_LINES || line == PIC_CASCADE)
        return -EINVAL;
    if (slots[line].handler)
        return -EBUSY;
    slots[line].ctx = ctx;
    slots[line].handler = handler;
    pic_unmask(line);
    return 0;
}

bool in_irq(void)
{
    return irq_depth > 0;
}

/* Weak default: replaced by the scheduler's preemption point. */
__attribute__((weak)) void irq_exit_hook(void)
{
}

void irq_dispatch(struct trap_frame *frame)
{
    unsigned line = (unsigned)(frame->vector - VEC_FIRST_IRQ);

    if (line >= PIC_IRQ_LINES) {
        pr_warn("vector %llu outside the PIC range\n", (unsigned long long)frame->vector);
        return;
    }
    if (pic_handle_spurious(line)) {
        spurious++;
        return;
    }

    struct irq_slot *s = &slots[line];

    s->count++;
    irq_depth++;
    if (s->handler)
        s->handler(frame, s->ctx);
    else
        pr_warn("unhandled IRQ %u\n", line);
    irq_depth--;
    pic_eoi(line);

    /* Only after EOI: a context switch here may not return for a while. */
    irq_exit_hook();
}

u64 irq_count(unsigned line)
{
    return line < PIC_IRQ_LINES ? slots[line].count : 0;
}

u64 irq_spurious_count(void)
{
    return spurious;
}
