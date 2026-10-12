/* SPDX-License-Identifier: MIT */
/*
 * UBSan test hook: lets a ktest provoke undefined behaviour on purpose and
 * observe the report instead of panicking.
 */
#ifndef MINIOS_CORE_UBSAN_H
#define MINIOS_CORE_UBSAN_H

#include <types.h>

struct ubsan_expectation {
    bool armed;
    int hits;
    const char *kind;
    u32 line;
};

void ubsan_expect(struct ubsan_expectation *e);
void ubsan_expect_clear(void);

#endif /* MINIOS_CORE_UBSAN_H */
