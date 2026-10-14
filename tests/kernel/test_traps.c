// SPDX-License-Identifier: MIT
/*
 * Exception handling tests. Each test provokes one exception on purpose,
 * lets the trap layer record it, and resumes through an exception-table
 * fixup (or klongjmp, when resuming in place is impossible).
 */
#include <arch/cpu.h>
#include <arch/extable.h>
#include <arch/irqflags.h>
#include <arch/segments.h>
#include <arch/setjmp.h>
#include <arch/trap.h>
#include <core/ksyms.h>
#include <core/ktest.h>
#include <lib/string.h>

/* Canonical, but outside the 1 GiB the bootloader maps. */
#define UNMAPPED_ADDR 0x80000000ull
#define NONCANONICAL  0x8000000000000000ull

KTEST(gdt_and_tss_are_loaded)
{
    u16 cs, ss, tr;

    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    __asm__ volatile("mov %%ss, %0" : "=r"(ss));
    __asm__ volatile("str %0" : "=r"(tr));
    KEXPECT_EQ(cs, GDT_KERNEL_CODE);
    KEXPECT_EQ(ss, GDT_KERNEL_DATA);
    KEXPECT_EQ(tr, GDT_TSS);
}

KTEST(invalid_opcode_is_caught)
{
    struct trap_expectation e;

    trap_expect(&e, VEC_INVALID_OPCODE, NULL);
    __asm__ volatile("1: ud2\n2:\n" EXTABLE_ENTRY(1b, 2b)::: "memory");
    trap_expect_clear();
    KEXPECT(e.hit);
    KEXPECT_EQ(e.error_code, 0);
}

KTEST(divide_error_is_caught)
{
    struct trap_expectation e;
    u64 rax = 1, rdx = 0, zero = 0;

    trap_expect(&e, VEC_DIVIDE_ERROR, NULL);
    __asm__ volatile("1: divq %[d]\n2:\n" EXTABLE_ENTRY(1b, 2b)
                     : "+a"(rax), "+d"(rdx)
                     : [d] "r"(zero)
                     : "memory");
    trap_expect_clear();
    KEXPECT(e.hit);
}

KTEST(breakpoint_resumes_after_int3)
{
    u64 before = trap_breakpoint_count();

    __asm__ volatile("int3" ::: "memory");
    KEXPECT_EQ(trap_breakpoint_count(), before + 1);
}

KTEST(noncanonical_access_raises_general_protection)
{
    struct trap_expectation e;
    u64 value = 0;

    trap_expect(&e, VEC_GENERAL_PROTECTION, NULL);
    __asm__ volatile("1: movq (%[p]), %[v]\n2:\n" EXTABLE_ENTRY(1b, 2b)
                     : [v] "+r"(value)
                     : [p] "r"(NONCANONICAL)
                     : "memory");
    trap_expect_clear();
    KEXPECT(e.hit);
    KEXPECT_EQ(e.error_code, 0);
}

KTEST(page_fault_reports_address_and_cause)
{
    struct trap_expectation e;
    u64 value = 0;

    trap_expect(&e, VEC_PAGE_FAULT, NULL);
    __asm__ volatile("1: movq (%[p]), %[v]\n2:\n" EXTABLE_ENTRY(1b, 2b)
                     : [v] "+r"(value)
                     : [p] "r"(UNMAPPED_ADDR)
                     : "memory");
    trap_expect_clear();
    KEXPECT(e.hit);
    KEXPECT_EQ(e.cr2, UNMAPPED_ADDR);
    KEXPECT_EQ(e.error_code, 0); /* read, not-present, supervisor */

    trap_expect(&e, VEC_PAGE_FAULT, NULL);
    __asm__ volatile("1: movq %[v], (%[p])\n2:\n" EXTABLE_ENTRY(1b, 2b)
                     :
                     : [v] "r"(value), [p] "r"(UNMAPPED_ADDR + 8)
                     : "memory");
    trap_expect_clear();
    KEXPECT(e.hit);
    KEXPECT_EQ(e.cr2, UNMAPPED_ADDR + 8);
    KEXPECT_EQ(e.error_code, PF_WRITE);
}

/*
 * The classic way a kernel dies: a fault with an unusable stack. Pointing
 * RSP at unmapped memory and executing int3 makes delivery of #BP fault
 * (#PF), delivery of that #PF fault again, and the CPU escalates to #DF.
 * Only the IST mechanism can run a handler at that point.
 */
KTEST(double_fault_runs_on_ist_stack)
{
    struct kjmp_buf jb;
    struct trap_expectation e;

    /* No timer IRQ while RSP is bogus: keep the fault sequence deterministic. */
    irqflags_t flags = arch_irq_save();

    trap_expect(&e, VEC_DOUBLE_FAULT, &jb);
    if (ksetjmp(&jb) == 0) {
        __asm__ volatile("movq %0, %%rsp\n\t"
                         "int3"
                         :
                         : "r"(UNMAPPED_ADDR + 0x1000)
                         : "memory");
        __builtin_unreachable();
    }
    trap_expect_clear();
    arch_irq_restore(flags);

    uptr top = ist_stack_top(IST_DOUBLE_FAULT);

    KEXPECT(e.hit);
    KEXPECT_EQ(e.error_code, 0); /* #DF always pushes 0 */
    KEXPECT(e.handler_rsp < top && e.handler_rsp >= top - IST_STACK_SIZE);
}

KTEST(extable_ignores_unlisted_addresses)
{
    KEXPECT_EQ(extable_find((uptr)&extable_find), 0);
    KEXPECT_EQ(extable_find(0), 0);
}

KTEST(ksyms_symbolize_kernel_text)
{
    uptr offset = ~0ul;
    uptr fn = (uptr)&ktest_run_all;
    const char *name = ksym_lookup(fn, &offset);

    KEXPECT(name != NULL && strcmp(name, "ktest_run_all") == 0);
    KEXPECT_EQ(offset, 0);
    name = ksym_lookup(fn + 3, &offset);
    KEXPECT(name != NULL && strcmp(name, "ktest_run_all") == 0);
    KEXPECT_EQ(offset, 3);
    KEXPECT_EQ(ksym_address("ktest_run_all"), fn);
    KEXPECT(ksym_lookup(0x1000, &offset) == NULL);
}

extern u64 __stack_chk_guard;

KTEST(stack_protector_guard_is_seeded)
{
    KEXPECT_EQ(__stack_chk_guard & 0xFF, 0);
    KEXPECT(__stack_chk_guard != 0x5a17e2c0ffee5100ull); /* the link-time default */
}
