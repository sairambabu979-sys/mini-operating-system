/* SPDX-License-Identifier: MIT */
/*
 * Fatal errors and invariant checks.
 */
#ifndef MINIOS_CORE_PANIC_H
#define MINIOS_CORE_PANIC_H

#include <compiler.h>
#include <types.h>

struct trap_frame;

/* Prints the reason, registers and a symbolized backtrace, then stops. */
__noreturn void panic(const char *fmt, ...) __printf(1, 2);
__noreturn void panic_trap(const struct trap_frame *f, const char *reason);

/* After this, panic() ends the QEMU run with a failure code (test mode). */
void panic_set_exit_on_panic(bool exit_qemu);

/*
 * Test hook: while armed, the next panic() records its message and resumes
 * at `resume` (via klongjmp) instead of stopping the machine. Only valid
 * for checks that panic *before* mutating state, which is how every
 * "detect misuse" path in the kernel is written.
 */
struct kjmp_buf;
struct panic_expectation {
    bool armed;
    bool hit;
    struct kjmp_buf *resume;
    char message[160];
};

void panic_expect(struct panic_expectation *e, struct kjmp_buf *resume);
void panic_expect_clear(void);

/* BUG_ON: an invariant that must hold in every build. */
#define BUG_ON(cond)                                                                               \
    do {                                                                                           \
        if (unlikely(cond))                                                                        \
            panic("BUG_ON(%s) at %s:%d", #cond, __FILE__, __LINE__);                               \
    } while (0)

/* assert: debug-only invariant; compiled out (but still type-checked) in release. */
#ifdef CONFIG_DEBUG
#define assert(cond) BUG_ON(!(cond))
#else
#define assert(cond) ((void)sizeof(!(cond)))
#endif

#endif /* MINIOS_CORE_PANIC_H */
