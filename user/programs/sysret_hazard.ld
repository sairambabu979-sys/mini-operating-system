/* SPDX-License-Identifier: MIT */
/* Places the program's only page at the very top of the user address space. */
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); }
SECTIONS
{
    . = 0x7FFFFFFFF000;
    .text : { *(.text) } :text
    /DISCARD/ : { *(.comment) *(.note .note.*) }
}
