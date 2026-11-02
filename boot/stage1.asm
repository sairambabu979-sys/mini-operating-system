; SPDX-License-Identifier: MIT
;
; Stage 1: Master Boot Record.
;
; The BIOS loads this sector at 0x7C00 and jumps to it in real mode with
; DL = boot drive. Its only job is to load stage 2 (63 sectors starting at
; LBA 1) with INT 13h extensions and jump to it.
;
; Progress and errors are printed to both the BIOS console and COM1:
;   '1'   stage 1 running
;   '!E'  INT 13h extensions missing
;   '!R'  disk read failed
;
; Layout: 446 bytes of code, a 64-byte partition table (filled in by
; tools/mkimage.py) and the 0x55AA signature.

%include "layout.inc"

bits 16
org STAGE1_ADDR

start:
    ; Some BIOSes enter at 07C0:0000 rather than 0000:7C00; normalize CS.
    jmp     0x0000:.canonical
.canonical:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, STACK_TOP
    sti
    cld
    mov     [boot_drive], dl

    mov     al, '1'
    call    putc

    ; INT 13h AH=41h: are the LBA extensions present?
    mov     ah, 0x41
    mov     bx, 0x55AA
    mov     dl, [boot_drive]
    int     0x13
    jc      .no_extensions
    cmp     bx, 0xAA55
    jne     .no_extensions
    test    cx, 1                   ; bit 0: AH=42h/43h packet calls supported
    jz      .no_extensions

    ; INT 13h AH=42h: extended read of stage 2 into 0000:7E00.
    mov     si, dap
    mov     ah, 0x42
    mov     dl, [boot_drive]
    int     0x13
    jc      .read_failed

    mov     dl, [boot_drive]        ; stage 2 expects DL = boot drive
    jmp     0x0000:STAGE2_ADDR

.no_extensions:
    mov     al, 'E'
    jmp     fail
.read_failed:
    mov     al, 'R'
    ; fall through

; Prints "!<AL>" and halts forever.
fail:
    push    ax
    mov     al, '!'
    call    putc
    pop     ax
    call    putc
.halt:
    cli
    hlt
    jmp     .halt

; putc: prints AL on the BIOS console and COM1. Preserves all registers.
; COM1 is polled for "transmit holding register empty"; with no UART the
; status port reads 0xFF, so the loop cannot hang.
putc:
    pusha
    mov     ah, 0x0E                ; BIOS teletype
    xor     bx, bx
    int     0x10
    popa
    pusha
    mov     ah, al
    mov     dx, COM1 + 5            ; line status register
    mov     cx, 0xFFFF              ; a broken UART must not prevent disk boot
.wait:
    in      al, dx
    test    al, 0x20
    jnz     .send
    loop    .wait
    popa
    ret
.send:
    mov     al, ah
    mov     dx, COM1
    out     dx, al
    popa
    ret

align 4
dap:                                ; INT 13h disk address packet
    db      0x10                    ; packet size
    db      0
    dw      STAGE2_SECTORS
    dw      STAGE2_ADDR, 0x0000     ; buffer offset, segment
    dq      STAGE2_LBA

boot_drive:
    db      0

    times   446 - ($ - $$) db 0     ; assembly fails here if code overflows
partition_table:
    times   64 db 0
    dw      0xAA55
