; SPDX-License-Identifier: MIT
;
; System call entry (SYSCALL instruction).
;
; On entry (SDM Vol. 2B, SYSCALL): RCX = user RIP, R11 = user RFLAGS,
; CS/SS = kernel selectors from STAR, RFLAGS &= ~FMASK (IF, DF, TF, AC are
; clear), RSP = *user* RSP, GS = user GS. Nothing may touch memory through
; RSP until it is replaced.
;
; ABI: RAX = number; arguments RDI, RSI, RDX, R10, R8, R9; result in RAX;
; RCX and R11 are clobbered.
;
; The saved state is a struct trap_frame identical to the interrupt path's,
; so a syscall can always leave through trap_return (iretq) instead.

bits 64
default rel

%include "asm-offsets.inc"

extern syscall_dispatch
extern trap_return

section .text

global syscall_entry
syscall_entry:
    swapgs
    mov     [gs:PERCPU_USER_RSP], rsp
    mov     rsp, [gs:PERCPU_KERNEL_RSP]

    push    qword USER_SS               ; build the same frame the CPU pushes
    push    qword [gs:PERCPU_USER_RSP]  ;   for an interrupt from ring 3
    push    r11                         ; RFLAGS
    push    qword USER_CS
    push    rcx                         ; RIP
    push    qword 0                     ; error code
    push    qword TRAP_VECTOR_SYSCALL
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

    mov     rdi, rsp
    sti                                 ; syscalls are preemptible
    call    syscall_dispatch
    cli

    ; Fast path, SYSRET, only if it is safe. On Intel CPUs, SYSRET to a
    ; non-canonical RIP faults in ring 0 *on the user stack*
    ; (CVE-2012-0217), so such returns, and any frame the kernel rewrote
    ; to something else, leave through iretq instead.
    mov     rcx, [rsp + TF_RIP]
    mov     r11, rcx
    shl     r11, 16
    sar     r11, 16
    cmp     r11, rcx
    jne     .slow_path
    cmp     qword [rsp + TF_CS], USER_CS
    jne     .slow_path
    cmp     qword [rsp + TF_SS], USER_SS
    jne     .slow_path

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
    ; RSP -> [vector][error][rip][cs][rflags][rsp][ss]
    mov     rcx, [rsp + 16]             ; user RIP
    mov     r11, [rsp + 32]             ; user RFLAGS
    mov     rsp, [rsp + 40]             ; user RSP: no memory access through RSP from here
    swapgs
    o64 sysret

.slow_path:
    inc     qword [rel syscall_slow_exits]
    jmp     trap_return

section .bss
align 8
global syscall_slow_exits
syscall_slow_exits:                     ; returns that had to avoid SYSRET
    resq    1
section .text

; void enter_user(struct trap_frame *frame)
;   First return of a new process to ring 3: loads RSP = frame and takes
;   the common exit path. Does not return.
global enter_user
enter_user:
    cli
    mov     rsp, rdi
    jmp     trap_return
