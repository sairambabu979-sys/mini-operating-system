; SPDX-License-Identifier: MIT
; User program entry: RSP is 16-byte aligned, RDI holds argc and RSI argv
; (set up by the kernel, see SPAWN_ARGS_MAX in uapi/syscall.h), so main
; gets them as main(argc, argv).

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
