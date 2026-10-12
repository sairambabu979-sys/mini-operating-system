/* SPDX-License-Identifier: MIT */
/*
 * Kernel linker script.
 *
 * The kernel is linked in the top 2 GiB of the address space (so
 * -mcmodel=kernel can use sign-extended 32-bit addresses) and loaded at
 * physical 1 MiB. Each permission class gets its own page-aligned PT_LOAD
 * segment, which lets the VMM map them W^X:
 *
 *     .text            R-X
 *     .rodata          R--
 *     .data, .bss      RW-
 */
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

KERNEL_PHYS = 0x100000;
KERNEL_VMA  = 0xFFFFFFFF80000000;

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    rodata PT_LOAD FLAGS(4);   /* PF_R */
    data   PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}

SECTIONS
{
    . = KERNEL_VMA + KERNEL_PHYS;
    __kernel_start = .;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA)
    {
        __text_start = .;
        *(.text.entry)
        *(.text .text.*)
        . = ALIGN(4K);
        __text_end = .;
    } :text

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        __rodata_start = .;
        *(.rodata .rodata.*)

        /* KTEST() descriptors, see <core/ktest.h> */
        . = ALIGN(8);
        __ktest_start = .;
        KEEP(*(.ktest))
        __ktest_end = .;

        /* Exception fixups, see <arch/extable.h> */
        . = ALIGN(8);
        __extable_start = .;
        KEEP(*(__ex_table))
        __extable_end = .;

        . = ALIGN(4K);
        __rodata_end = .;
    } :rodata

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA)
    {
        __data_start = .;
        *(.data .data.*)
    } :data

    .bss ALIGN(16) : AT(ADDR(.bss) - KERNEL_VMA)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
        . = ALIGN(4K);
        __data_end = .;
    } :data

    __kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.interp .dynamic .dynsym .dynstr .hash .gnu.hash)
    }
}
