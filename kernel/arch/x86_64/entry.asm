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
global loader_rflags
extern kmain
extern __stack_chk_guard

_start:
    lea     rsp, [boot_stack_top]   ; lea leaves RFLAGS alone
    pushfq
    pop     qword [loader_rflags]   ; the hand-off state, before cli/cld hide it (tests)
    cli
    cld
    xor     ebp, ebp                ; terminates frame-pointer backtraces

    ; Seed the stack-protector canary before any protected C function runs.
    ; RDI (boot_info physical address) must survive: rdtsc clobbers only
    ; RAX/RDX, and RCX is scratch.
    rdtsc
    shl     rdx, 32
    or      rax, rdx
    mov     rcx, 0x9E3779B97F4A7C15     ; golden-ratio multiplier spreads TSC bits
    imul    rax, rcx
    xor     [__stack_chk_guard], rax
    and     byte [__stack_chk_guard], 0 ; keep the low byte zero

    call    kmain
.halt:                              ; kmain never returns; belt and braces
    cli
    hlt
    jmp     .halt

section .data
align 8
loader_rflags: dq 0

section .bss
align 16
boot_stack:
    resb    16384
boot_stack_top:
