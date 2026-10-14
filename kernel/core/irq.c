// SPDX-License-Identifier: MIT
/*
 * Interrupt dispatch over two controllers. See <core/irq.h>.
 *
 * The 8259 is always remapped first, even when the APICs take over: a
 * PIC left on its power-on vectors (8-15) would deliver anything that
 * slips through as a CPU exception. In APIC mode every 8259 line is then
 * masked, and the local APIC's LINT0 (where the 8259 is wired) as well.
 */
#define pr_fmt(fmt) "irq: " fmt

#include <acpi/acpi.h>
#include <arch/cpuid.h>
#include <arch/lapic.h>
#include <core/boot.h>
#include <core/irq.h>
#include <core/klog.h>
#include <drivers/ioapic.h>
#include <drivers/pic8259.h>
#include <lib/cmdline.h>
#include <lib/errno.h>

#define LOCAL_VECTORS (VEC_COUNT - VEC_FIRST_LOCAL)

struct irq_slot {
    irq_handler_t handler;
    void *ctx;
    u64 count;
};

struct irq_chip {
    const char *name;
    void (*set_masked)(unsigned line, bool masked);
    void (*eoi)(unsigned line);
    bool (*spurious)(unsigned line); /* true: no handler, EOI already dealt with */
};

static struct irq_slot slots[IRQ_LINES];
static struct irq_slot local_slots[LOCAL_VECTORS];
static u64 spurious;
static u64 apic_errors;
static u32 irq_depth;
static const struct irq_chip *chip;
static const struct acpi_madt_info *madt;

/* --- 8259 ------------------------------------------------------------------ */

static void pic_set_masked(unsigned line, bool masked)
{
    if (masked)
        pic_mask(line);
    else
        pic_unmask(line);
}

static const struct irq_chip pic_chip = {
    .name = "8259 PIC",
    .set_masked = pic_set_masked,
    .eoi = pic_eoi,
    .spurious = pic_handle_spurious,
};

/* --- I/O APIC + local APIC ------------------------------------------------- */

static void apic_set_masked(unsigned line, bool masked)
{
    ioapic_set_masked(madt->isa[line].gsi, masked);
}

static void apic_eoi(unsigned line)
{
    (void)line;
    lapic_eoi();
}

static bool apic_spurious(unsigned line)
{
    (void)line;
    return false; /* the local APIC reports spurious interrupts on their own vector */
}

static const struct irq_chip apic_chip = {
    .name = "I/O APIC",
    .set_masked = apic_set_masked,
    .eoi = apic_eoi,
    .spurious = apic_spurious,
};

/* Returns NULL when the APICs are usable, or why they are not. */
static const char *apic_unusable_reason(void)
{
    if (cmdline_has_flag(boot_info_get()->cmdline, "noapic"))
        return "noapic on the command line";
    if (!cpu_features()->apic)
        return "the CPU has no local APIC";
    if (!acpi_madt())
        return "no MADT";
    if (lapic_boot_id() > 0xFF)
        return "APIC ID above 255: I/O APIC routing would need interrupt remapping";
    if (!ioapic_init(acpi_madt()))
        return "no usable I/O APIC";
    return NULL;
}

static const char *pic_reason; /* why the 8259 was chosen; NULL on the APIC path */

void irq_init(void)
{
    pic_init(VEC_FIRST_IRQ);

    const char *reason = apic_unusable_reason();

    if (reason) {
        chip = &pic_chip;
        pic_reason = reason;
        pr_info("using the 8259 PIC (%s)\n", reason);
        return;
    }
    madt = acpi_madt();
    for (unsigned line = 0; line < IRQ_LINES; line++)
        pic_mask(line); /* the cascade too: nothing comes from the 8259 any more */
    lapic_init(madt);
    chip = &apic_chip;
    pr_info("using the local APIC and %u I/O APIC pin(s)\n", ioapic_pin_count());
}

enum irq_mode irq_mode(void)
{
    return chip == &apic_chip ? IRQ_MODE_APIC : IRQ_MODE_PIC;
}

const char *irq_pic_reason(void)
{
    return pic_reason;
}

const char *irq_controller_name(void)
{
    return chip->name;
}

static int wire(unsigned line)
{
    if (chip == &pic_chip)
        return line == PIC_CASCADE ? -EINVAL : 0;

    const struct acpi_isa_irq *isa = &madt->isa[line];

    if (isa->gsi == ACPI_GSI_NONE)
        return -ENODEV;
    return ioapic_route(isa->gsi, (u8)(VEC_FIRST_IRQ + line), (u8)lapic_id(), isa->active_low,
                        isa->level_triggered);
}

int irq_register(unsigned line, irq_handler_t handler, void *ctx)
{
    if (line >= IRQ_LINES)
        return -EINVAL;
    if (slots[line].handler)
        return -EBUSY;
    int err = wire(line);

    if (err)
        return err;
    slots[line].ctx = ctx;
    slots[line].handler = handler;
    chip->set_masked(line, false);
    return 0;
}

void irq_unregister(unsigned line)
{
    if (line >= IRQ_LINES)
        return;
    if (slots[line].handler)
        chip->set_masked(line, true);
    slots[line].handler = NULL;
    slots[line].ctx = NULL;
}

void irq_mask(unsigned line)
{
    if (line < IRQ_LINES && slots[line].handler)
        chip->set_masked(line, true);
}

void irq_unmask(unsigned line)
{
    if (line < IRQ_LINES && slots[line].handler)
        chip->set_masked(line, false);
}

int irq_register_local(u8 vector, irq_handler_t handler, void *ctx)
{
    if (vector < VEC_FIRST_LOCAL || vector == VEC_APIC_ERROR || vector == VEC_APIC_SPURIOUS)
        return -EINVAL;
    if (irq_mode() != IRQ_MODE_APIC)
        return -ENODEV;
    struct irq_slot *s = &local_slots[vector - VEC_FIRST_LOCAL];

    if (s->handler)
        return -EBUSY;
    s->ctx = ctx;
    s->handler = handler;
    return 0;
}

void irq_unregister_local(u8 vector)
{
    if (vector >= VEC_FIRST_LOCAL)
        local_slots[vector - VEC_FIRST_LOCAL] = (struct irq_slot){0};
}

bool in_irq(void)
{
    return irq_depth > 0;
}

/* Weak default: replaced by the scheduler's preemption point. */
__attribute__((weak)) void irq_exit_hook(void)
{
}

static void run_handler(struct irq_slot *s, struct trap_frame *frame, const char *what, u64 id)
{
    s->count++;
    irq_depth++;
    if (s->handler)
        s->handler(frame, s->ctx);
    else
        pr_warn("unhandled %s %llu\n", what, (unsigned long long)id);
    irq_depth--;
}

static void dispatch_local(struct trap_frame *frame)
{
    u8 vector = (u8)frame->vector;

    switch (vector) {
    case VEC_APIC_SPURIOUS:
        spurious++; /* never in service: no EOI */
        return;
    case VEC_APIC_ERROR:
        apic_errors++;
        pr_warn("local APIC error, ESR %#x\n", lapic_read_esr());
        lapic_eoi();
        return;
    default:
        run_handler(&local_slots[vector - VEC_FIRST_LOCAL], frame, "local vector", vector);
        lapic_eoi();
        trap_report_nmis(); /* safe here: no lock can be held across an enabled interrupt */
        irq_exit_hook();
        return;
    }
}

void irq_dispatch(struct trap_frame *frame)
{
    u64 vector = frame->vector;

    if (vector >= VEC_FIRST_LOCAL && irq_mode() == IRQ_MODE_APIC) {
        dispatch_local(frame);
        return;
    }
    if (vector >= VEC_FIRST_IRQ + IRQ_LINES) {
        pr_warn("unexpected interrupt, vector %llu\n", (unsigned long long)vector);
        /* Unacknowledged, it would block its priority class and every one below. */
        if (irq_mode() == IRQ_MODE_APIC && lapic_in_service((u8)vector))
            lapic_eoi();
        return;
    }

    unsigned line = (unsigned)(vector - VEC_FIRST_IRQ);

    if (chip->spurious(line)) {
        spurious++;
        return;
    }
    run_handler(&slots[line], frame, "IRQ", line);
    chip->eoi(line);
    trap_report_nmis();

    /* Only after EOI: a context switch here may not return for a while. */
    irq_exit_hook();
}

u64 irq_count(unsigned line)
{
    return line < IRQ_LINES ? slots[line].count : 0;
}

u64 irq_local_count(u8 vector)
{
    return vector >= VEC_FIRST_LOCAL ? local_slots[vector - VEC_FIRST_LOCAL].count : 0;
}

u64 irq_spurious_count(void)
{
    return spurious;
}

u64 irq_apic_error_count(void)
{
    return apic_errors;
}
