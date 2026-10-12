; SPDX-License-Identifier: MIT
;
; ksetjmp / klongjmp. Layout must match struct kjmp_buf (arch/setjmp.h);
; offsets come from asm-offsets.inc.

bits 64
default rel

%include "asm-offsets.inc"

section .text

; int ksetjmp(struct kjmp_buf *buf)
;   in:  RDI = buf
;   out: EAX = 0
global ksetjmp
ksetjmp:
    mov     [rdi + KJMP_RBX], rbx
    mov     [rdi + KJMP_RBP], rbp
    mov     [rdi + KJMP_R12], r12
    mov     [rdi + KJMP_R13], r13
    mov     [rdi + KJMP_R14], r14
    mov     [rdi + KJMP_R15], r15
    lea     rdx, [rsp + 8]              ; RSP as it will be after we return
    mov     [rdi + KJMP_RSP], rdx
    mov     rdx, [rsp]                  ; return address
    mov     [rdi + KJMP_RIP], rdx
    xor     eax, eax
    ret

; void klongjmp(struct kjmp_buf *buf, int value)
;   in:  RDI = buf, ESI = value (0 is promoted to 1)
;   Does not return; resumes at ksetjmp's call site with EAX = value.
global klongjmp
klongjmp:
    mov     eax, esi
    test    eax, eax
    jnz     .nonzero
    inc     eax
.nonzero:
    mov     rbx, [rdi + KJMP_RBX]
    mov     rbp, [rdi + KJMP_RBP]
    mov     r12, [rdi + KJMP_R12]
    mov     r13, [rdi + KJMP_R13]
    mov     r14, [rdi + KJMP_R14]
    mov     r15, [rdi + KJMP_R15]
    mov     rsp, [rdi + KJMP_RSP]
    jmp     [rdi + KJMP_RIP]

; void call_on_stack(void (*fn)(void *), void *arg, void *stack_top)
;   Runs fn(arg) on another stack, then returns on the original one.
;   in:  RDI = fn, RSI = arg, RDX = stack_top (16-byte aligned)
global call_on_stack
call_on_stack:
    push    rbp
    mov     rbp, rsp
    mov     rsp, rdx
    mov     rax, rdi
    mov     rdi, rsi
    call    rax
    mov     rsp, rbp
    pop     rbp
    ret
