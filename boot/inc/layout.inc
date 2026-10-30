; SPDX-License-Identifier: MIT
;
; Physical memory layout used by the bootloader. See docs/design/boot.md.
;
;   0x00000 - 0x004FF   IVT + BIOS data area (do not touch)
;   0x00500             scratch byte for the A20 wraparound test
;   0x01000 - 0x01FFF   struct boot_info            (BOOT_INFO_PHYS)
;   0x02000 - 0x05FFF   boot page tables: PML4, PDPT lo, PDPT hi, PD
;   0x06000 - 0x07BFF   real-mode stack (grows down from 0x7C00)
;   0x07C00 - 0x07DFF   stage 1 (MBR)
;   0x07E00 - 0x0FBFF   stage 2 (63 sectors)
;   0x10000 - 0x17FFF   disk bounce buffer (64 KiB aligned: never crosses a DMA boundary)
;   0x100000 -          kernel PT_LOAD segments (KERNEL_PHYS_MIN)
;   0x1000000 -         raw kernel ELF staging area (KERNEL_STAGING), then the
;                       initrd, which stays there for the kernel

%ifndef BOOT_LAYOUT_INC
%define BOOT_LAYOUT_INC

%define COM1                0x3F8

%define STAGE1_ADDR         0x7C00
%define STACK_TOP           0x7C00
%define STAGE2_ADDR         0x7E00
%define STAGE2_LBA          1
%define STAGE2_SECTORS      63          ; fills the conventional post-MBR gap
%define STAGE2_MAGIC        0x44483253  ; "S2HD" little-endian
%define STAGE2_HEADER_OFF   4           ; mkimage.py patches the header here

%define BOOT_PML4           0x2000
%define BOOT_PDPT_LO        0x3000
%define BOOT_PDPT_HI        0x4000
%define BOOT_PD             0x5000

%define BOUNCE_SEG          0x1000      ; 0x1000:0000 = 0x10000
%define BOUNCE_BUF          0x10000
%define LOAD_CHUNK_SECTORS  64          ; 32 KiB per INT 13h call (well under the 127 cap)

%define KERNEL_PHYS_MIN     0x00100000
%define KERNEL_STAGING      0x01000000
%define KERNEL_VMA          0xFFFFFFFF80000000

; GDT selectors (stage 2's own GDT; the kernel installs a new one)
%define SEL_CODE64          0x08
%define SEL_DATA            0x10

; Architectural constants
%define CR0_PE              (1 << 0)
%define CR0_PG              (1 << 31)
%define CR4_PAE             (1 << 5)
%define MSR_EFER            0xC0000080
%define EFER_LME            (1 << 8)
%define PTE_P               (1 << 0)
%define PTE_W               (1 << 1)
%define PTE_PS              (1 << 7)

%endif
