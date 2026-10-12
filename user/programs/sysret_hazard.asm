; SPDX-License-Identifier: MIT
;
; The SYSRET hazard (CVE-2012-0217): the syscall instruction is the very
; last thing in the user address space, so its return address, RCX,
; is 0x0000800000000000, which is non-canonical. On Intel CPUs a SYSRET to
; it faults in ring 0 on the user's stack. The kernel must notice and leave
; through iretq, whose own #GP then kills only this process.

bits 64
section .text
global _start

    times 0xFF9 db 0xCC                 ; pad so the syscall ends at the page end
_start:
    mov     eax, 3                      ; SYS_GETPID (5 bytes)
    syscall                             ; (2 bytes) -> next RIP = 0x800000000000
