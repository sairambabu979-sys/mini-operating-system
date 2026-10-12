; SPDX-License-Identifier: MIT
;
; Interrupt and exception entry stubs.
;
; One stub per vector (generated below) normalizes the stack so that every
; vector produces an identical struct trap_frame:
;
;   - vectors without a CPU error code push a dummy 0,
;   - every stub pushes its vector number,
;   - isr_common pushes the general-purpose registers.
;
; isr_common contract:
;   in:   stack = [vector][error code][CPU frame: RIP CS RFLAGS RSP SS]
;   does: saves GPRs, swapgs if the trap came from ring 3, calls
;         trap_dispatch(frame) with DF clear and RSP 16-byte aligned,
;         restores everything (trap_dispatch may modify the frame) and iretq
;   IF:   cleared by the CPU (all gates are interrupt gates)

bits 64
default rel

%include "asm-offsets.inc"

extern trap_dispatch

section .text

; Vectors for which the CPU pushes an error code (SDM Vol. 3A, 6.13).
%macro ISR_STUB 1
align 16
isr_stub_%1:
%if %1 == 8 || (%1 >= 10 && %1 <= 14) || %1 == 17 || %1 == 21 || %1 == 29 || %1 == 30
%else
    push    qword 0
%endif
    push    qword %1
    jmp     isr_common
%endmacro

%assign vec 0
%rep 256
    ISR_STUB vec
%assign vec vec + 1
%endrep

align 16
isr_common:
    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15

    test    byte [rsp + TF_CS], 3
    jz      .from_kernel
    swapgs
.from_kernel:
    cld
    mov     rdi, rsp
    call    trap_dispatch
    ; fall through

; trap_return: restores a struct trap_frame at RSP and returns into it.
; Shared by interrupts, the syscall slow path and the first entry into a
; new process. Interrupts must be off.
global trap_return
trap_return:
    test    byte [rsp + TF_CS], 3
    jz      .to_kernel
    swapgs
.to_kernel:
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rbx
    pop     rax
    add     rsp, 16                     ; drop vector and error code
global trap_iretq
trap_iretq:
    iretq

; iretq itself can fault when returning to user mode with a bad frame (a
; non-canonical RIP, say): the #GP arrives in ring 0, at trap_iretq, with
; the user GS already swapped in. The exception table sends it here.
; RSP still points at the iret frame. The process caused it, so the
; process dies; the kernel carries on.
extern user_bad_iret
iret_fault:
    test    byte [rsp + 8], 3           ; CS of the rejected frame
    jz      .kernel_frame               ; (impossible; user_bad_iret panics)
    swapgs                              ; back to the kernel GS
.kernel_frame:
    mov     rdi, rsp                    ; the rejected iret frame
    and     rsp, -16
    call    user_bad_iret               ; does not return
    ud2

section __ex_table progbits alloc noexec nowrite align=8
    dq      trap_iretq, iret_fault

section .text

section .rodata
align 8
global isr_stub_table
isr_stub_table:
%assign vec 0
%rep 256
    dq      isr_stub_%+vec
%assign vec vec + 1
%endrep
