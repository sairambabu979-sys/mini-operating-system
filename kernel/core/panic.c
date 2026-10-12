// SPDX-License-Identifier: MIT
/*
 * panic(): the last thing the kernel does. It must work with the system in
 * any state, so it uses only polled serial output and touches no
 * allocator, lock or interrupt.
 */
#include <arch/cpu.h>
#include <arch/extable.h>
#include <arch/msr.h>
#include <arch/paging.h>
#include <arch/setjmp.h>
#include <arch/trap.h>
#include <core/ksyms.h>
#include <core/panic.h>
#include <core/printk.h>
#include <drivers/qemu.h>
#include <lib/printf.h>

#define BACKTRACE_MAX_DEPTH 32

static bool exit_on_panic;
static volatile bool panicking;
static struct panic_expectation *expectation;

void panic_expect(struct panic_expectation *e, struct kjmp_buf *resume)
{
    *e = (struct panic_expectation){.armed = true, .resume = resume};
    expectation = e;
}

void panic_expect_clear(void)
{
    expectation = NULL;
}

void panic_set_exit_on_panic(bool exit_qemu)
{
    exit_on_panic = exit_qemu;
}

/* Prints addr symbolized via lookup_addr (they differ for return addresses). */
static void print_symbol(uptr addr, uptr lookup_addr)
{
    uptr offset;
    const char *name = ksym_lookup(lookup_addr, &offset);

    if (name) {
        uptr off = offset + (addr - lookup_addr);

        kprintf("0x%016lx %s+%#lx\n", addr, name, off);
    } else {
        kprintf("0x%016lx ?\n", addr);
    }
}

/*
 * Frame-pointer walk. Every frame is [saved RBP][return address]; the chain
 * ends at RBP = 0 (set by _start). Frames are read with probe_read_u64 and
 * must be aligned, in the kernel half, and move up the stack, so a corrupt
 * chain ends the walk instead of faulting inside panic().
 */
static void print_backtrace(uptr rbp)
{
    for (int depth = 0; depth < BACKTRACE_MAX_DEPTH && rbp != 0; depth++) {
        u64 saved_rbp, ret;

        if (!IS_ALIGNED(rbp, 8) || rbp < KERNEL_HALF || rbp > ~(uptr)0 - 16 ||
            !probe_read_u64(rbp, &saved_rbp) || !probe_read_u64(rbp + 8, &ret)) {
            kprintf("  (frame pointer %#lx unreadable, stopping)\n", rbp);
            return;
        }
        if (ret == 0)
            return;
        kprintf("  #%-2d ", depth + 1);
        /* Look up ret-1: a call at the very end of a function returns past it. */
        print_symbol(ret, ret - 1);
        if (saved_rbp != 0 && saved_rbp <= rbp) {
            kprintf("  (frame chain does not ascend, stopping)\n");
            return;
        }
        rbp = saved_rbp;
    }
}

static void print_control_registers(void)
{
    kprintf("CR0 %016llx  CR2 %016llx  CR3 %016llx\n", (unsigned long long)read_cr0(),
            (unsigned long long)read_cr2(), (unsigned long long)read_cr3());
    kprintf("CR4 %016llx  EFER %016llx\n", (unsigned long long)read_cr4(),
            (unsigned long long)rdmsr(MSR_EFER));
}

static void print_page_fault_decode(u64 err)
{
    kprintf("page fault: %s %s in %s mode%s%s\n",
            (err & PF_INSTR)   ? "instruction fetch"
            : (err & PF_WRITE) ? "write"
                               : "read",
            (err & PF_PRESENT) ? "violated page protection" : "hit a non-present page",
            (err & PF_USER) ? "user" : "kernel", (err & PF_RSVD) ? ", reserved bit set" : "",
            (err & PF_PK) ? ", protection key" : "");
}

static void print_frame(const struct trap_frame *f)
{
    kprintf("exception %llu (%s), error code %#llx, from %s mode\n", (unsigned long long)f->vector,
            trap_name(f->vector), (unsigned long long)f->error_code,
            trap_from_user(f) ? "user" : "kernel");
    if (f->vector == VEC_PAGE_FAULT)
        print_page_fault_decode(f->error_code);
    kprintf("RIP %04llx:%016llx  RSP %04llx:%016llx  RFLAGS %016llx\n", (unsigned long long)f->cs,
            (unsigned long long)f->rip, (unsigned long long)f->ss, (unsigned long long)f->rsp,
            (unsigned long long)f->rflags);
    kprintf("RAX %016llx  RBX %016llx  RCX %016llx\n", (unsigned long long)f->rax,
            (unsigned long long)f->rbx, (unsigned long long)f->rcx);
    kprintf("RDX %016llx  RSI %016llx  RDI %016llx\n", (unsigned long long)f->rdx,
            (unsigned long long)f->rsi, (unsigned long long)f->rdi);
    kprintf("RBP %016llx  R8  %016llx  R9  %016llx\n", (unsigned long long)f->rbp,
            (unsigned long long)f->r8, (unsigned long long)f->r9);
    kprintf("R10 %016llx  R11 %016llx  R12 %016llx\n", (unsigned long long)f->r10,
            (unsigned long long)f->r11, (unsigned long long)f->r12);
    kprintf("R13 %016llx  R14 %016llx  R15 %016llx\n", (unsigned long long)f->r13,
            (unsigned long long)f->r14, (unsigned long long)f->r15);
}

static __noreturn void panic_finish(void)
{
    kprintf("======================== end of panic ========================\n");
    if (exit_on_panic)
        qemu_exit(QEMU_EXIT_FAILURE);
    cpu_halt_forever();
}

/* A second panic while printing the first would only recurse: stop now. */
static void enter_panic(void)
{
    cpu_irq_disable();
    if (panicking) {
        kprintf("\nrecursive panic, halting\n");
        if (exit_on_panic)
            qemu_exit(QEMU_EXIT_FAILURE);
        cpu_halt_forever();
    }
    panicking = true;
    kprintf("\n======================== KERNEL PANIC ========================\n");
}

__noreturn void panic(const char *fmt, ...)
{
    va_list ap;

    if (expectation && expectation->armed) {
        struct panic_expectation *e = expectation;

        e->armed = false;
        e->hit = true;
        va_start(ap, fmt);
        k_vsnprintf(e->message, sizeof(e->message), fmt, ap);
        va_end(ap);
        klongjmp(e->resume, 1);
    }

    enter_panic();
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");
    print_control_registers();
    kprintf("backtrace:\n");
    print_backtrace(read_rbp());
    panic_finish();
}

__noreturn void panic_trap(const struct trap_frame *f, const char *reason)
{
    enter_panic();
    kprintf("%s\n", reason);
    print_frame(f);
    print_control_registers();
    kprintf("backtrace:\n  #0  ");
    print_symbol(f->rip, f->rip);
    print_backtrace(f->rbp);
    panic_finish();
}
