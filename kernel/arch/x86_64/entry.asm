; SPDX-License-Identifier: MIT
;
; Kernel entry point.
;
; Contract with the bootloader (docs/design/boot.md):
;   - 64-bit long mode, paging on, interrupts off, direction flag undefined
;   - physical 0-1 GiB mapped both at 0 and at KERNEL_VMA (-2 GiB)
;   - RDI = physical address of struct boot_info
;   - no valid stack: we must not touch memory through RSP before loading it

bits 64
default rel

section .text.entry
global _start
extern kmain

_start:
    cli
    cld
    lea     rsp, [boot_stack_top]
    xor     ebp, ebp                ; terminates frame-pointer backtraces
    ; RDI (boot_info physical address) is passed through untouched.
    call    kmain
.halt:                              ; kmain never returns; belt and braces
    cli
    hlt
    jmp     .halt

section .bss
align 16
boot_stack:
    resb    16384
boot_stack_top:
