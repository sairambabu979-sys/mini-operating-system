/* SPDX-License-Identifier: MIT */
/*
 * ELF64 executable validation for user programs.
 *
 * Pure code (no allocation, no hardware), host-tested against malformed
 * and hostile files. The loader only ever sees an image that passed
 * elf_parse(), so it never has to re-check a bound.
 */
#ifndef MINIOS_PROC_ELF_H
#define MINIOS_PROC_ELF_H

#include <compiler.h>
#include <types.h>

#define ELF_MAX_SEGMENTS 8
#define ELF_MAX_PHDRS    32

struct elf_segment {
    u64 vaddr;
    u64 memsz;
    u64 offset; /* file offset of the initialized part */
    u64 filesz;
    bool writable;
    bool executable;
};

struct elf_image {
    u64 entry;
    u32 nsegments;
    struct elf_segment segments[ELF_MAX_SEGMENTS];
};

/*
 * Validates a static x86_64 executable whose loadable segments must lie
 * in [user_min, user_top). Rejects, among others: wrong class, machine or
 * type; dynamic linking; truncated or overlapping headers and segments;
 * writable+executable segments; segments sharing a page; an entry point
 * outside executable memory. Returns 0, -ENOEXEC or -EINVAL.
 */
__must_check int elf_parse(const void *data, usize len, u64 user_min, u64 user_top,
                           struct elf_image *out);

#endif /* MINIOS_PROC_ELF_H */
