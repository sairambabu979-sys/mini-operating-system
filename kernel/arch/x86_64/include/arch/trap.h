/* SPDX-License-Identifier: MIT */
/*
 * Trap frames and the exception/interrupt entry interface.
 */
#ifndef MINIOS_ARCH_TRAP_H
#define MINIOS_ARCH_TRAP_H

#include <compiler.h>
#include <types.h>

/*
 * Register state saved on every interrupt, exception and (later) syscall.
 * The field order is the reverse of the push order in isr.asm; asm-offsets
 * exports the layout so the assembly never hard-codes an offset.
 */
struct trap_frame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector;
    u64 error_code; /* CPU-pushed, or 0 for vectors without one */
    /* Pushed by the CPU: */
    u64 rip, cs, rflags, rsp, ss;
};

static_assert(sizeof(struct trap_frame) == 22 * sizeof(u64), "trap_frame layout changed");
/* The CPU aligns RSP to 16 before pushing; 22 qwords keep it aligned for calls. */
static_assert(sizeof(struct trap_frame) % 16 == 0, "trap_frame must preserve 16-byte alignment");

/* Exception vectors (Intel SDM Vol. 3A, table 6-1). */
enum {
    VEC_DIVIDE_ERROR = 0,
    VEC_DEBUG = 1,
    VEC_NMI = 2,
    VEC_BREAKPOINT = 3,
    VEC_OVERFLOW = 4,
    VEC_BOUND_RANGE = 5,
    VEC_INVALID_OPCODE = 6,
    VEC_DEVICE_NOT_AVAILABLE = 7,
    VEC_DOUBLE_FAULT = 8,
    VEC_INVALID_TSS = 10,
    VEC_SEGMENT_NOT_PRESENT = 11,
    VEC_STACK_FAULT = 12,
    VEC_GENERAL_PROTECTION = 13,
    VEC_PAGE_FAULT = 14,
    VEC_X87_FP = 16,
    VEC_ALIGNMENT_CHECK = 17,
    VEC_MACHINE_CHECK = 18,
    VEC_SIMD_FP = 19,
    VEC_VIRTUALIZATION = 20,
    VEC_CONTROL_PROTECTION = 21,
    VEC_HYPERVISOR_INJECTION = 28,
    VEC_VMM_COMMUNICATION = 29,
    VEC_SECURITY = 30,
    VEC_FIRST_IRQ = 32,
    VEC_COUNT = 256,
};

/* trap_frame.vector for frames built by the syscall entry (not an IDT vector). */
#define TRAP_VECTOR_SYSCALL 0x100

/* Page-fault error code bits. */
#define PF_PRESENT (1u << 0) /* 0: not-present page, 1: protection violation */
#define PF_WRITE   (1u << 1)
#define PF_USER    (1u << 2)
#define PF_RSVD    (1u << 3) /* reserved bit set in a paging structure */
#define PF_INSTR   (1u << 4) /* instruction fetch */
#define PF_PK      (1u << 5) /* protection key */
#define PF_SS      (1u << 6) /* shadow stack */

static inline bool trap_from_user(const struct trap_frame *f)
{
    return (f->cs & 3) != 0;
}

void idt_init(void);

/* C entry point for every vector, called from isr.asm. */
void trap_dispatch(struct trap_frame *f);

/* Restores `frame` and returns to it (first entry of a process into ring 3). */
__noreturn void enter_user(struct trap_frame *frame);

/* Called when iretq to ring 3 faults; kills the current process. */
__noreturn void user_bad_iret(const u64 *iret_frame);

const char *trap_name(u64 vector);

/* Re-enables NMI delivery (masked by the bootloader). Call after idt_init(). */
void nmi_enable(void);

/* Number of #BP traps handled; lets tests observe int3 without a debugger. */
u64 trap_breakpoint_count(void);

/*
 * Test hook: while armed, the next exception with this vector is recorded
 * instead of being fatal, then execution resumes at an exception-table
 * fixup or, failing that, via klongjmp to `resume`. Used by ktests only.
 */
struct kjmp_buf;
struct trap_expectation {
    bool armed;
    u64 vector;
    struct kjmp_buf *resume; /* NULL: rely on the exception table */
    /* Filled in when the trap fires: */
    bool hit;
    u64 error_code;
    u64 cr2;
    uptr handler_rsp; /* stack pointer the handler ran on */
};

void trap_expect(struct trap_expectation *e, u64 vector, struct kjmp_buf *resume);
void trap_expect_clear(void);

#endif /* MINIOS_ARCH_TRAP_H */
