// SPDX-License-Identifier: MIT
/*
 * Exception and interrupt dispatch.
 *
 * Policy (docs/PLAN.md §5.3):
 *   - kernel-mode fault at an exception-table address: resume at the fixup
 *   - #BP: report and continue (a breakpoint is not an error)
 *   - NMI: report and continue
 *   - anything else in kernel mode: panic with a full decode
 *   - user mode: arrives with processes (M7); until then, panic
 */
#define pr_fmt(fmt) "traps: " fmt

#include <arch/cpu.h>
#include <arch/extable.h>
#include <arch/io.h>
#include <arch/setjmp.h>
#include <arch/trap.h>
#include <core/irq.h>
#include <core/klog.h>
#include <core/panic.h>
#include <proc/process.h>

static const char *const exception_names[VEC_FIRST_IRQ] = {
    [VEC_DIVIDE_ERROR] = "#DE divide error",
    [VEC_DEBUG] = "#DB debug",
    [VEC_NMI] = "NMI",
    [VEC_BREAKPOINT] = "#BP breakpoint",
    [VEC_OVERFLOW] = "#OF overflow",
    [VEC_BOUND_RANGE] = "#BR bound range exceeded",
    [VEC_INVALID_OPCODE] = "#UD invalid opcode",
    [VEC_DEVICE_NOT_AVAILABLE] = "#NM device not available",
    [VEC_DOUBLE_FAULT] = "#DF double fault",
    [VEC_INVALID_TSS] = "#TS invalid TSS",
    [VEC_SEGMENT_NOT_PRESENT] = "#NP segment not present",
    [VEC_STACK_FAULT] = "#SS stack-segment fault",
    [VEC_GENERAL_PROTECTION] = "#GP general protection",
    [VEC_PAGE_FAULT] = "#PF page fault",
    [VEC_X87_FP] = "#MF x87 floating-point",
    [VEC_ALIGNMENT_CHECK] = "#AC alignment check",
    [VEC_MACHINE_CHECK] = "#MC machine check",
    [VEC_SIMD_FP] = "#XM SIMD floating-point",
    [VEC_VIRTUALIZATION] = "#VE virtualization",
    [VEC_CONTROL_PROTECTION] = "#CP control protection",
    [VEC_HYPERVISOR_INJECTION] = "#HV hypervisor injection",
    [VEC_VMM_COMMUNICATION] = "#VC VMM communication",
    [VEC_SECURITY] = "#SX security",
};

const char *trap_name(u64 vector)
{
    if (vector < VEC_FIRST_IRQ)
        return exception_names[vector] ? exception_names[vector] : "reserved exception";
    return "external interrupt";
}

#define CMOS_INDEX_PORT  0x70
#define CMOS_NMI_DISABLE 0x80

static struct trap_expectation *expectation;
static u64 breakpoint_count;

void trap_expect(struct trap_expectation *e, u64 vector, struct kjmp_buf *resume)
{
    *e = (struct trap_expectation){.armed = true, .vector = vector, .resume = resume};
    expectation = e;
}

void trap_expect_clear(void)
{
    expectation = NULL;
}

/* Vectors that are traps: the saved RIP already points past the instruction. */
static bool vector_is_trap(u64 vector)
{
    return vector == VEC_BREAKPOINT || vector == VEC_OVERFLOW || vector == VEC_DEBUG;
}

/* Returns true if a test was expecting this trap and execution can resume. */
static bool handle_expected(struct trap_frame *f)
{
    struct trap_expectation *e = expectation;

    if (!e || !e->armed || e->vector != f->vector)
        return false;

    e->armed = false;
    e->hit = true;
    e->error_code = f->error_code;
    e->cr2 = read_cr2();
    e->handler_rsp = read_rsp();

    uptr fixup = extable_find(f->rip);

    if (fixup) {
        f->rip = fixup;
        return true;
    }
    if (e->resume)
        klongjmp(e->resume, 1);
    if (vector_is_trap(f->vector))
        return true;
    panic_trap(f, "expected trap fired with no way to resume");
}

static void handle_exception(struct trap_frame *f)
{
    if (handle_expected(f))
        return;

    /* A process's fault is the process's problem: it dies, the kernel does not. */
    if (trap_from_user(f) && f->vector != VEC_NMI && f->vector != VEC_MACHINE_CHECK)
        process_kill_on_fault(f);

    switch (f->vector) {
    case VEC_BREAKPOINT:
        breakpoint_count++;
        pr_info("breakpoint at %#llx\n", (unsigned long long)(f->rip - 1));
        return;
    case VEC_NMI:
        pr_warn("NMI received at %#llx\n", (unsigned long long)f->rip);
        return;
    default:
        break;
    }

    if (!trap_from_user(f)) {
        uptr fixup = extable_find(f->rip);

        if (fixup) {
            f->rip = fixup;
            return;
        }
    }
    panic_trap(f, "unhandled exception");
}

void trap_dispatch(struct trap_frame *f)
{
    if (f->vector < VEC_FIRST_IRQ) {
        handle_exception(f);
        return;
    }
    irq_dispatch(f);
}

u64 trap_breakpoint_count(void)
{
    return breakpoint_count;
}

void nmi_enable(void)
{
    /* Bit 7 of the CMOS index port gates NMI; stage 2 set it before long mode. */
    outb(CMOS_INDEX_PORT, inb(CMOS_INDEX_PORT) & (u8)~CMOS_NMI_DISABLE);
}
