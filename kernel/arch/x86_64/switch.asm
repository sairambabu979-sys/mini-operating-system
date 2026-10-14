; SPDX-License-Identifier: MIT
;
; Context switch.
;
; struct task *switch_context(struct task *prev, struct task *next)
;
;   in:   RDI = prev, RSI = next; interrupts off; scheduler lock held
;   does: pushes prev's callee-saved registers, stores RSP in prev,
;         loads next's RSP, pops its registers, returns into next
;   out:  RAX = prev, as seen by *next*: whoever resumes learns which task
;         ran before it, so it can release the lock and reap a dead
;         predecessor (a task cannot free the stack it is running on).
;
; Caller-saved registers need no saving: switch_context is a normal call
; from C, so the compiler already assumes they are clobbered. FPU/SSE
; state is handled in C, only for user tasks.

bits 64
default rel

%include "asm-offsets.inc"

section .text

global switch_context
switch_context:
    push    rbp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    mov     [rdi + TASK_SAVED_RSP], rsp
    mov     rsp, [rsi + TASK_SAVED_RSP]
    mov     rax, rdi
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp
    ret

; First instructions of every new task. Reached by switch_context's `ret`
; with R12 = entry function, R13 = its argument, RAX = previous task, and
; RSP 16-byte aligned (see task_setup_stack in sched.c).
extern sched_task_start
extern task_exit
global task_trampoline
task_trampoline:
    mov     rdi, rax
    call    sched_task_start            ; finish the switch, enable interrupts
    mov     rdi, r13
    call    r12
    mov     edi, eax
    call    task_exit                   ; does not return
    ud2
