; SPDX-License-Identifier: MIT
; User program entry: RSP is 16-byte aligned (set up by the kernel).

bits 64
section .text.start
global _start
extern main
extern sys_exit

_start:
    xor     ebp, ebp                    ; outermost frame
    call    main
    mov     edi, eax
    call    sys_exit                    ; does not return
    ud2
