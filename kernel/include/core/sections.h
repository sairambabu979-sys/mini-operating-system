/* SPDX-License-Identifier: MIT */
/*
 * Linker-script symbols (kernel/linker.ld). Declared as arrays so they decay
 * to addresses and are never mistaken for objects with contents.
 */
#ifndef MINIOS_CORE_SECTIONS_H
#define MINIOS_CORE_SECTIONS_H

extern char __kernel_start[], __kernel_end[];
extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __data_end[];
extern char __bss_start[], __bss_end[];

#endif /* MINIOS_CORE_SECTIONS_H */
