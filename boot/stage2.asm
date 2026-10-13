; SPDX-License-Identifier: MIT
;
; Stage 2: from real mode to a loaded 64-bit kernel.
;
;   1. initialize COM1, clear struct boot_info
;   2. enable A20 (BIOS, then keyboard controller, then fast A20)
;   3. collect the E820 memory map into boot_info
;   4. verify the CPU supports long mode
;   5. load the raw kernel ELF to KERNEL_STAGING through unreal mode
;   6. build identity + higher-half page tables for the first 1 GiB
;   7. switch straight from real mode to long mode
;   8. copy PT_LOAD segments into place, zero their .bss tails
;   9. publish boot_info (magic last) and jump to the ELF entry point
;
; Progress codes on COM1 / screen: '2' after each step number above
; ('2' '3' ... '7'); errors print '!' and a letter, then halt:
;
;   !A  A20 could not be enabled       !C  CPU lacks long mode
;   !M  E820 failed or map is empty    !S  staging area not usable RAM
;   !K  bad stage-2 header             !D  disk read failed
;   !F  kernel is not a valid ELF64    !P  bad program header
;   !a  (A20 test builds) A20 could not be turned off
;
; A20 test builds (-DA20_SELFTEST=bios|kbc|fast) first turn A20 off, then
; may turn it back on by that one method only, and say so on COM1.
; SeaBIOS hands over with A20 already on, so without them none of the
; three methods would ever run under test.

%include "layout.inc"
%include "asm-offsets.inc"

%ifdef A20_SELFTEST
  %assign A20_TRY_BIOS 0
  %assign A20_TRY_KBC  0
  %assign A20_TRY_FAST 0
  %ifidn A20_SELFTEST, bios
    %assign A20_TRY_BIOS 1
  %elifidn A20_SELFTEST, kbc
    %assign A20_TRY_KBC 1
  %elifidn A20_SELFTEST, fast
    %assign A20_TRY_FAST 1
  %else
    %error "A20_SELFTEST must be bios, kbc or fast"
  %endif
%else
  %assign A20_TRY_BIOS 1
  %assign A20_TRY_KBC  1
  %assign A20_TRY_FAST 1
%endif

bits 16
org STAGE2_ADDR

stage2_entry:
    jmp     near stage2_main        ; 3 bytes
    db      0                       ; pad: header starts at STAGE2_HEADER_OFF

; Patched by tools/mkimage.py. Keep in sync with its STAGE2_HEADER struct.
stage2_header:
.magic:             dd STAGE2_MAGIC
.kernel_lba:        dd 0
.kernel_sectors:    dd 0
.cmdline:           times BOOT_CMDLINE_MAX db 0

stage2_main:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, STACK_TOP
    sti
    cld
    mov     [boot_drive], dl

    call    serial_init
    mov     al, '2'
    call    putc

    cmp     dword [stage2_header.magic], STAGE2_MAGIC
    jne     err_header
    cmp     dword [stage2_header.kernel_sectors], 0
    je      err_header

    ; Clear boot_info so every field has a defined value.
    mov     di, BOOT_INFO_PHYS
    mov     cx, BOOT_INFO_SIZE
    xor     al, al
    rep     stosb

    call    enable_a20
    mov     al, '3'
    call    putc

    call    detect_memory
    call    check_staging_area
    mov     al, '4'
    call    putc

    call    check_long_mode
    call    load_kernel
    mov     al, '5'
    call    putc

    call    fill_boot_info_real
    call    build_page_tables
    jmp     enter_long_mode

; --------------------------------------------------------------------------
; Errors (real mode)
; --------------------------------------------------------------------------

err_a20:    mov al, 'A'
            jmp fail16
err_cpu:    mov al, 'C'
            jmp fail16
err_mmap:   mov al, 'M'
            jmp fail16
err_stage:  mov al, 'S'
            jmp fail16
err_header: mov al, 'K'
            jmp fail16
err_disk:   mov al, 'D'
            jmp fail16
%ifdef A20_SELFTEST
err_a20_off: mov al, 'a'
            jmp fail16
%endif

; Prints "!<AL>" and halts forever.
fail16:
    push    ax
    mov     al, '!'
    call    putc
    pop     ax
    call    putc
.halt:
    cli
    hlt
    jmp     .halt

; --------------------------------------------------------------------------
; Console (real mode)
; --------------------------------------------------------------------------

; 115200 8N1, FIFOs enabled. The kernel re-initializes the UART itself.
serial_init:
    mov     dx, COM1 + 1
    xor     al, al
    out     dx, al                  ; IER: no interrupts
    mov     dx, COM1 + 3
    mov     al, 0x80
    out     dx, al                  ; LCR: DLAB on
    mov     dx, COM1 + 0
    mov     al, 1
    out     dx, al                  ; divisor low: 115200 baud
    mov     dx, COM1 + 1
    xor     al, al
    out     dx, al                  ; divisor high
    mov     dx, COM1 + 3
    mov     al, 0x03
    out     dx, al                  ; LCR: 8N1, DLAB off
    mov     dx, COM1 + 2
    mov     al, 0xC7
    out     dx, al                  ; FCR: enable + clear FIFOs, 14-byte threshold
    mov     dx, COM1 + 4
    mov     al, 0x03
    out     dx, al                  ; MCR: DTR + RTS
    ret

; putc: prints AL on the BIOS console and COM1. Preserves all registers.
putc:
    pusha
    mov     ah, 0x0E
    xor     bx, bx
    int     0x10
    popa
    pusha
    mov     ah, al
    mov     dx, COM1 + 5
.wait:
    in      al, dx
    test    al, 0x20
    jz      .wait
    mov     al, ah
    mov     dx, COM1
    out     dx, al
    popa
    ret

; --------------------------------------------------------------------------
; A20
; --------------------------------------------------------------------------

enable_a20:
%ifdef A20_SELFTEST
    call    a20_force_off
    call    a20_check
    jnz     err_a20_off             ; still on: the test would prove nothing
%endif
    call    a20_check
    jnz     .done
%if A20_TRY_BIOS
    mov     ax, 0x2401              ; BIOS: enable A20 gate
    int     0x15
    call    a20_check
    jnz     .done
%endif
%if A20_TRY_KBC
    mov     ah, 0x02                ; gate on
    call    a20_keyboard_controller
    call    a20_check
    jnz     .done
%endif
%if A20_TRY_FAST
    in      al, 0x92                ; "fast A20" via system control port A
    test    al, 0x02
    jnz     .fast_done
    or      al, 0x02
    and     al, 0xFE                ; never set bit 0: it resets the machine
    out     0x92, al
.fast_done:
    call    a20_check
    jnz     .done
%endif
    jmp     err_a20
.done:
%ifdef A20_SELFTEST
    mov     si, a20_selftest_msg
    call    puts
%endif
    ret

%ifdef A20_SELFTEST
; Turns A20 off by every method, so that only the one under test can
; have turned it back on.
a20_force_off:
    mov     ax, 0x2400              ; BIOS: disable A20 gate
    int     0x15
    xor     ah, ah                  ; gate off
    call    a20_keyboard_controller
    in      al, 0x92
    and     al, 0xFC                ; gate off; never set bit 0: it resets the machine
    out     0x92, al
    ret

; Prints the NUL-terminated string at DS:SI.
puts:
    lodsb
    test    al, al
    jz      .done
    call    putc
    jmp     puts
.done:
    ret

%defstr A20_METHOD A20_SELFTEST
a20_selftest_msg: db 13, 10, "A20 test: off, then on by ", A20_METHOD, 13, 10, 0
%endif

; a20_check: ZF=0 if A20 is enabled, ZF=1 if addresses wrap at 1 MiB.
; Writes different bytes to 0000:0500 and FFFF:0510 (= 0x100500); if they
; alias, A20 is off. Both bytes are restored. Preserves all registers.
a20_check:
    pusha
    push    ds
    push    es
    xor     ax, ax
    mov     ds, ax
    not     ax
    mov     es, ax
    mov     si, 0x0500
    mov     di, 0x0510
    mov     bl, [ds:si]
    mov     bh, [es:di]
    mov     byte [ds:si], 0x00
    mov     byte [es:di], 0xFF
    cmp     byte [ds:si], 0xFF      ; equal => wrapped => A20 disabled
    mov     [es:di], bh             ; mov does not affect flags
    mov     [ds:si], bl
    pop     es
    pop     ds
    popa                            ; popa does not affect flags
    ret

; Classic 8042 method: read the output port, replace bit 1 (the A20 gate)
; with AH (0x02 on, 0 off), write it back. Nothing below writes AH: the
; waits use AL and CX.
a20_keyboard_controller:
    cli
    call    kbc_wait_input
    mov     al, 0xAD                ; disable keyboard
    out     0x64, al
    call    kbc_wait_input
    mov     al, 0xD0                ; read output port
    out     0x64, al
    call    kbc_wait_output
    in      al, 0x60
    mov     bl, al
    call    kbc_wait_input
    mov     al, 0xD1                ; write output port
    out     0x64, al
    call    kbc_wait_input
    mov     al, bl
    and     al, 0xFD
    or      al, ah                  ; A20 gate
    out     0x60, al
    call    kbc_wait_input
    mov     al, 0xAE                ; re-enable keyboard
    out     0x64, al
    call    kbc_wait_input
    sti
    ret

; Bounded waits: a missing controller reads 0xFF, which must not hang us.
kbc_wait_input:
    mov     cx, 0xFFFF
.loop:
    in      al, 0x64
    test    al, 0x02
    jz      .ok
    loop    .loop
.ok:
    ret

kbc_wait_output:
    mov     cx, 0xFFFF
.loop:
    in      al, 0x64
    test    al, 0x01
    jnz     .ok
    loop    .loop
.ok:
    ret

; --------------------------------------------------------------------------
; Memory map (INT 15h, EAX=E820h)
; --------------------------------------------------------------------------

%define SMAP 0x534D4150             ; "SMAP"

detect_memory:
    mov     di, BOOT_INFO_PHYS + BI_MMAP
    xor     ebx, ebx                ; continuation value
    xor     bp, bp                  ; entries stored
.next:
    ; Pre-set the ACPI attribute to "valid" so 20-byte replies are accepted.
    mov     dword [es:di + E820_ACPI], 1
    mov     eax, 0xE820
    mov     edx, SMAP
    mov     ecx, E820_ENTRY_SIZE
    int     0x15
    jc      .carry                  ; carry: unsupported (first call) or end of list
    cmp     eax, SMAP
    jne     err_mmap
    jcxz    .skip                   ; ECX = bytes written
    ; ACPI 3.0's "enabled" attribute (bit 0) was deprecated in ACPI 6.0, and
    ; some BIOSes leave it clear on every entry: honoring it lost all of RAM
    ; on those. Like Linux, ignore it (and record every entry as valid).
    mov     dword [es:di + E820_ACPI], 1
    mov     ecx, [es:di + E820_LENGTH]
    or      ecx, [es:di + E820_LENGTH + 4]
    jz      .skip                   ; zero-length entry
    inc     bp
    add     di, E820_ENTRY_SIZE
    cmp     bp, BOOT_MMAP_MAX
    jae     .done
.skip:
    test    ebx, ebx
    jnz     .next
    jmp     .done
.carry:
    test    bp, bp
    jz      err_mmap
.done:
    test    bp, bp
    jz      err_mmap
    mov     [BOOT_INFO_PHYS + BI_MMAP_COUNT], bp
    ret

; The ELF is staged at KERNEL_STAGING; make sure that range is usable RAM
; before writing to it, rather than corrupting whatever lives there.
check_staging_area:
    mov     edx, [stage2_header.kernel_sectors]
    shl     edx, 9                  ; bytes
    add     edx, KERNEL_STAGING     ; EDX = staging end (exclusive)
    jc      err_stage
    mov     si, BOOT_INFO_PHYS + BI_MMAP
    mov     cx, [BOOT_INFO_PHYS + BI_MMAP_COUNT]
.entry:
    cmp     dword [si + E820_TYPE], E820_USABLE
    jne     .next
    cmp     dword [si + E820_BASE + 4], 0
    jne     .next                   ; starts above 4 GiB
    cmp     dword [si + E820_BASE], KERNEL_STAGING
    ja      .next                   ; starts after the staging area
    cmp     dword [si + E820_LENGTH + 4], 0
    jne     .ok                     ; >= 4 GiB long and starts below: covers it
    mov     eax, [si + E820_BASE]
    add     eax, [si + E820_LENGTH]
    jc      .ok                     ; ends above 4 GiB
    cmp     eax, edx
    jae     .ok
.next:
    add     si, E820_ENTRY_SIZE
    loop    .entry
    jmp     err_stage
.ok:
    ret

; --------------------------------------------------------------------------
; CPU feature check
; --------------------------------------------------------------------------

check_long_mode:
    ; CPUID exists iff EFLAGS.ID (bit 21) can be toggled.
    pushfd
    pop     eax
    mov     ecx, eax
    xor     eax, 1 << 21
    push    eax
    popfd
    pushfd
    pop     eax
    push    ecx
    popfd
    xor     eax, ecx
    jz      err_cpu
    mov     eax, 0x80000000
    cpuid
    cmp     eax, 0x80000001
    jb      err_cpu
    mov     eax, 0x80000001
    cpuid
    test    edx, 1 << 29            ; LM
    jz      err_cpu
    ret

; --------------------------------------------------------------------------
; Kernel load (unreal mode)
; --------------------------------------------------------------------------

; Unreal mode: load DS/ES from a flat 4 GiB descriptor in protected mode,
; then drop back to real mode. The hidden segment limits stay at 4 GiB, so
; 32-bit addresses work while BIOS calls remain available. Re-entered after
; every BIOS call, since a BIOS may reload segment registers.
enter_unreal:
    cli
    push    ds
    push    es
    lgdt    [gdt_descriptor]
    mov     eax, cr0
    or      al, CR0_PE
    mov     cr0, eax
    mov     bx, SEL_DATA
    mov     ds, bx
    mov     es, bx
    and     al, ~CR0_PE
    mov     cr0, eax
    pop     es
    pop     ds
    sti
    ret

load_kernel:
    mov     eax, [stage2_header.kernel_lba]
    mov     [kernel_dap.lba], eax
    mov     eax, [stage2_header.kernel_sectors]
    mov     [sectors_left], eax
    mov     dword [dest_ptr], KERNEL_STAGING
.chunk:
    mov     eax, [sectors_left]
    test    eax, eax
    jz      .done
    cmp     eax, LOAD_CHUNK_SECTORS
    jbe     .size_ok
    mov     eax, LOAD_CHUNK_SECTORS
.size_ok:
    mov     [kernel_dap.count], ax

    mov     si, kernel_dap
    mov     ah, 0x42
    mov     dl, [boot_drive]
    int     0x13
    jc      err_disk

    call    enter_unreal
    movzx   ecx, word [kernel_dap.count]
    shl     ecx, 9
    mov     esi, BOUNCE_BUF
    mov     edi, [dest_ptr]
    add     [dest_ptr], ecx
    a32 rep movsb                   ; DS:ESI -> ES:EDI with 4 GiB limits

    movzx   eax, word [kernel_dap.count]
    add     [kernel_dap.lba], eax
    adc     dword [kernel_dap.lba + 4], 0
    sub     [sectors_left], eax
    jmp     .chunk
.done:
    ret

; Fields of boot_info that are simplest to fill while still in real mode.
fill_boot_info_real:
    mov     si, stage2_header.cmdline
    mov     di, BOOT_INFO_PHYS + BI_CMDLINE
    mov     cx, BOOT_CMDLINE_MAX
    rep     movsb
    mov     byte [BOOT_INFO_PHYS + BI_CMDLINE + BOOT_CMDLINE_MAX - 1], 0
    movzx   eax, byte [boot_drive]
    mov     [BOOT_INFO_PHYS + BI_BOOT_DRIVE], eax
    mov     dword [BOOT_INFO_PHYS + BI_LOADER], BOOT_LOADER_BIOS
    ret

; --------------------------------------------------------------------------
; Page tables: identity-map and higher-half-map physical 0-1 GiB (2 MiB pages)
;
;   PML4[0]   -> PDPT_LO[0]   -> PD   (virtual 0x0000000000000000)
;   PML4[511] -> PDPT_HI[510] -> PD   (virtual 0xFFFFFFFF80000000)
; --------------------------------------------------------------------------

build_page_tables:
    mov     di, BOOT_PML4
    mov     cx, (4 * 4096) / 4
    xor     eax, eax
    rep     stosd

    mov     dword [BOOT_PML4 + 0 * 8], BOOT_PDPT_LO | PTE_P | PTE_W
    mov     dword [BOOT_PML4 + 511 * 8], BOOT_PDPT_HI | PTE_P | PTE_W
    mov     dword [BOOT_PDPT_LO + 0 * 8], BOOT_PD | PTE_P | PTE_W
    mov     dword [BOOT_PDPT_HI + 510 * 8], BOOT_PD | PTE_P | PTE_W

    mov     di, BOOT_PD
    mov     eax, PTE_P | PTE_W | PTE_PS
    mov     cx, 512
.pde:
    mov     [di], eax               ; high dword already zero
    add     eax, 0x200000
    add     di, 8
    loop    .pde
    ret

; --------------------------------------------------------------------------
; Real mode -> long mode, directly (no intermediate 32-bit stage needed:
; setting CR0.PE and CR0.PG together with EFER.LME activates long mode).
; --------------------------------------------------------------------------

enter_long_mode:
    cli
    mov     al, 0xFF                ; mask both PICs: no IDT exists yet
    out     0xA1, al
    out     0x21, al
    in      al, 0x70                ; disable NMI (kernel re-enables it)
    or      al, 0x80
    out     0x70, al

    lidt    [idt_null]              ; any stray interrupt now triple-faults
                                    ; deterministically instead of jumping
                                    ; through the real-mode IVT
    mov     eax, CR4_PAE
    mov     cr4, eax
    mov     eax, BOOT_PML4
    mov     cr3, eax
    mov     ecx, MSR_EFER
    rdmsr
    or      eax, EFER_LME
    wrmsr
    lgdt    [gdt_descriptor]
    mov     eax, cr0
    or      eax, CR0_PE | CR0_PG
    mov     cr0, eax
    jmp     SEL_CODE64:long_mode_entry

; --------------------------------------------------------------------------
; 64-bit code
; --------------------------------------------------------------------------

bits 64

long_mode_entry:
    mov     ax, SEL_DATA
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     fs, ax
    mov     gs, ax
    mov     rsp, STACK_TOP
    mov     al, '6'
    call    putc64

    call    load_elf                ; RAX = entry point
    mov     rbx, rax

    mov     rax, BOOT_INFO_VERSION
    mov     [BOOT_INFO_PHYS + BI_VERSION], eax
    mov     rax, BOOT_INFO_MAGIC    ; written last: magic implies complete
    mov     [BOOT_INFO_PHYS + BI_MAGIC], rax

    mov     al, '7'
    call    putc64
    mov     al, 13
    call    putc64
    mov     al, 10
    call    putc64

    mov     edi, BOOT_INFO_PHYS     ; SysV: first argument
    xor     ebp, ebp
    jmp     rbx

; Validates the staged ELF64 image and copies each PT_LOAD segment to its
; physical address. Returns the entry point in RAX.
;
; Register use: RBX = ELF base, R8 = current phdr, R9 = phdrs left,
;               R10 = lowest paddr, R11 = highest end, R12 = file size
load_elf:
    mov     rbx, KERNEL_STAGING
    mov     r12d, [stage2_header.kernel_sectors]
    shl     r12, 9

    cmp     dword [rbx], 0x464C457F ; "\x7FELF"
    jne     err_elf
    cmp     byte [rbx + 4], 2       ; ELFCLASS64
    jne     err_elf
    cmp     byte [rbx + 5], 1       ; ELFDATA2LSB
    jne     err_elf
    cmp     word [rbx + 16], 2      ; ET_EXEC
    jne     err_elf
    cmp     word [rbx + 18], 0x3E   ; EM_X86_64
    jne     err_elf
    cmp     word [rbx + 54], 56     ; e_phentsize == sizeof(Elf64_Phdr)
    jne     err_elf
    mov     rax, KERNEL_VMA
    cmp     [rbx + 24], rax         ; entry must be in the higher half
    jb      err_elf

    mov     r8, [rbx + 32]          ; e_phoff
    movzx   r9d, word [rbx + 56]    ; e_phnum
    mov     rax, r9
    imul    rax, rax, 56
    add     rax, r8
    jc      err_elf
    cmp     rax, r12
    ja      err_elf                 ; phdr table runs past the file
    add     r8, rbx
    mov     r10, -1
    xor     r11d, r11d

.phdr:
    test    r9, r9
    jz      .done
    cmp     dword [r8], 1           ; PT_LOAD
    jne     .next
    mov     rdx, [r8 + 40]          ; p_memsz
    test    rdx, rdx
    jz      .next

    mov     rdi, [r8 + 24]          ; p_paddr
    mov     rcx, [r8 + 32]          ; p_filesz
    mov     rsi, [r8 + 8]           ; p_offset
    cmp     rcx, rdx
    ja      err_phdr                ; filesz > memsz
    mov     rax, rsi
    add     rax, rcx
    jc      err_phdr
    cmp     rax, r12
    ja      err_phdr                ; segment data runs past the file
    cmp     rdi, KERNEL_PHYS_MIN
    jb      err_phdr                ; would overwrite low memory / the loader
    mov     rax, rdi
    add     rax, rdx
    jc      err_phdr
    cmp     rax, KERNEL_STAGING
    ja      err_phdr                ; would overwrite the staged ELF itself

    cmp     rdi, r10
    cmovb   r10, rdi
    cmp     rax, r11
    cmova   r11, rax

    add     rsi, rbx
    rep     movsb                   ; copy p_filesz bytes
    mov     rcx, rdx
    sub     rcx, [r8 + 32]
    xor     eax, eax
    rep     stosb                   ; zero p_memsz - p_filesz bytes (.bss)
.next:
    add     r8, 56
    dec     r9
    jmp     .phdr
.done:
    test    r11, r11
    jz      err_phdr                ; no loadable segments
    mov     [BOOT_INFO_PHYS + BI_KERNEL_PHYS_START], r10
    mov     [BOOT_INFO_PHYS + BI_KERNEL_PHYS_END], r11
    mov     rax, [rbx + 24]         ; e_entry
    ret

err_elf:    mov al, 'F'
            jmp fail64
err_phdr:   mov al, 'P'
            ; fall through

fail64:
    mov     bl, al
    mov     al, '!'
    call    putc64
    mov     al, bl
    call    putc64
.halt:
    cli
    hlt
    jmp     .halt

; putc64: AL to COM1 (no BIOS in long mode). Clobbers DX, AH.
putc64:
    mov     ah, al
    mov     dx, COM1 + 5
.wait:
    in      al, dx
    test    al, 0x20
    jz      .wait
    mov     al, ah
    mov     dx, COM1
    out     dx, al
    ret

; --------------------------------------------------------------------------
; Data
; --------------------------------------------------------------------------

align 8
gdt:
    dq      0                       ; 0x00 null
    dq      0x00AF9A000000FFFF      ; 0x08 code: 64-bit (L=1), DPL 0, execute/read
    dq      0x00CF92000000FFFF      ; 0x10 data: flat 4 GiB, read/write
gdt_end:

gdt_descriptor:
    dw      gdt_end - gdt - 1
    dd      gdt

idt_null:
    dw      0
    dd      0

align 4
kernel_dap:
    db      0x10, 0
.count:
    dw      0
    dw      0, BOUNCE_SEG           ; buffer offset, segment
.lba:
    dq      0

sectors_left:   dd 0
dest_ptr:       dd 0
boot_drive:     db 0

    times   STAGE2_SECTORS * 512 - ($ - $$) db 0    ; fails if stage 2 overflows
