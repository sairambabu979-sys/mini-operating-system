/* SPDX-License-Identifier: MIT */
/* User programs: static, non-PIE, loaded at 4 MiB; one PT_LOAD per permission class. */
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x400000;
    .text : { *(.text.start) *(.text .text.*) } :text
    . = ALIGN(4K);
    .rodata : { *(.rodata .rodata.*) } :rodata
    . = ALIGN(4K);
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame*) }
}
