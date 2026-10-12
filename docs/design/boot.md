# Boot

How a powered-on PC gets from the BIOS to `kmain()`. Sources: `boot/stage1.asm`,
`boot/stage2.asm`, `tools/mkimage.py`, `kernel/include/boot/boot_info.h`.

## Disk image

```
LBA 0        stage 1: MBR (446 B code, 1 active partition entry, 0x55AA)
LBA 1..63    stage 2: exactly 63 sectors; header at byte 4
LBA 64..     kernel ELF (stripped), padded to a whole sector
```

`mkimage.py` patches the stage-2 header with the kernel's start LBA, its
sector count and the kernel command line. The loader has no hard-coded sizes.
The run image and the test image contain the same kernel binary; only the
command line differs (`""` vs `"ktest"`).

## Physical memory during boot

| Range | Use |
|-------|-----|
| `0x00000-0x004FF` | IVT + BIOS data area |
| `0x01000-0x01FFF` | `struct boot_info` (`BOOT_INFO_PHYS`) |
| `0x02000-0x05FFF` | boot page tables: PML4, PDPT lo, PDPT hi, PD |
| `0x06000-0x07BFF` | stack (grows down from `0x7C00`) |
| `0x07C00-0x07DFF` | stage 1 |
| `0x07E00-0x0FBFF` | stage 2 |
| `0x10000-0x17FFF` | disk bounce buffer (64 KiB-aligned, never crosses a DMA boundary) |
| `0x100000-` | kernel segments, at their ELF `p_paddr` |
| `0x1000000-` | raw kernel ELF staging area (checked against E820 before use) |

## Sequence

| Code | Step |
|------|------|
| `1` | Stage 1: normalize `CS:IP`, check INT 13h extensions, read stage 2 (LBA mode), jump. |
| `2` | Stage 2 running; COM1 initialized; `boot_info` page cleared. |
| `3` | A20 enabled: BIOS `2401h` → 8042 output port → port `0x92`, each verified by a wraparound test. |
| `4` | E820 map collected (20- and 24-byte replies, ACPI "ignore" bit honored, zero-length entries dropped); staging area verified as usable RAM. |
| `5` | CPU checked for long mode; kernel read in 32 KiB chunks and copied above 1 MiB through **unreal mode** (re-entered after every BIOS call, since a BIOS may reload segment registers). |
| `6` | Page tables built; PICs masked, NMI disabled, null IDT loaded; **real mode → long mode in one step** (`CR4.PAE`, `EFER.LME`, then `CR0.PE`+`CR0.PG` together). |
| `7` | ELF validated (class, endianness, type, machine, header sizes, entry in the higher half). Each `PT_LOAD` is bounds-checked against the file and the staging area, copied, and its `.bss` tail zeroed. `boot_info` is published with **magic written last**, then the loader jumps to `e_entry`. |

Errors print `!` and a letter, then halt. Each letter is listed at the top of
`stage2.asm`. The null IDT is deliberate: any interrupt before the kernel
installs its own IDT triple-faults immediately, instead of jumping through a
real-mode IVT that makes no sense in long mode.

## The kernel's entry contract

- Long mode, paging on, interrupts off, NMI masked, PICs masked.
- Physical `0–1 GiB` mapped twice with 2 MiB pages: identity, and at
  `KERNEL_VMA = 0xFFFFFFFF80000000`.
- `RDI` = physical address of `struct boot_info`. No valid stack: `_start`
  loads its own before touching memory through `RSP`.

Each promise is checked at boot by `tests/kernel/test_boot.c`: the CPU is in
long mode, A20 is on, `.bss` is zero, the kernel sits at its link address,
the sections are page-aligned, the memory map is sane, and IF/DF are clear.

## Single source of truth

`struct boot_info` is defined only in C. `kernel/asm-offsets.c` is compiled
to assembly and its `->NAME value` markers become `asm-offsets.inc`, which
stage 2 includes. Changing a field moves the offsets in both languages at
once, and `static_assert`s pin the layout.

## Debugging a boot failure

1. Read the progress string. The last code printed shows how far the loader got.
2. `make debug`, then `gdb` with `target remote :1234`. Break at `*0x7c00`
   (stage 1) or `*0x7e00` (stage 2), and use `set architecture i8086` for
   real-mode code.
3. `build/<config>/qemu-debug.log` records every interrupt and CPU reset
   (`-d int,cpu_reset`). A triple fault appears as a `CPU Reset` right after
   the faulting exception.

## UEFI (`boot/efi/loader.c`)

A second loader, `BOOTX64.EFI`, gives the kernel **exactly the same
hand-off**: `boot_info` at `BOOT_INFO_PHYS`, the kernel at its link
address, the same identity plus higher-half windows, interrupts off, PICs
masked. One kernel binary therefore runs under both firmwares, and
`make test` boots the full in-kernel suite and the `init` demo both ways.
`boot_info.loader` records which loader ran (it is `boot_info` version 2).

```
ESP (FAT32, built by tools/mkesp.py with mtools)
  /EFI/BOOT/BOOTX64.EFI   the loader
  /minios/kernel.elf      the same stripped kernel the BIOS image carries
  /minios/cmdline         the command line (one line)
```

| Step | Detail |
|------|--------|
| Claim memory | `AllocatePages(AllocateAddress)` for the kernel's physical range and the `boot_info` page. If the firmware owns either, the loader stops with a message instead of overwriting firmware memory. |
| Load | Files are read through the volume the loader itself came from (LoadedImage → SimpleFileSystem). The kernel ELF gets the same checks as in stage 2, then is copied and its `.bss` zeroed **while boot services still run**. |
| Page tables | 0–4 GiB identity (the loader keeps running after the CR3 switch, so its image and stack must lie below 4 GiB, which is checked) and 0–1 GiB at `-2 GiB`, with 2 MiB pages. |
| Exit | The final `GetMemoryMap` is followed immediately by `ExitBootServices`, with nothing allocating or printing in between, retried if the map key went stale. |
| Memory map | EFI descriptors become E820 entries. Loader and boot-services memory and conventional memory become usable; ACPI types are kept; runtime services, MMIO and everything unknown become reserved. Contiguous same-type runs are merged. |
| Jump | `cli`, PICs masked, CR3 switched, `RDI = &boot_info`, jump to the ELF entry. |

The loader is a PE32+ built with clang (`--target=x86_64-unknown-windows`)
and `lld-link`, with no UEFI SDK: `boot/efi/efi.h` declares the slice of
the specification it uses, with `static_assert`ed table offsets. The
Microsoft x64 ABI is LLP64 (`long` is 32 bits), so every 64-bit value is
printed with `%ll`.

**Real-hardware caveats.** Firmware may occupy physical 1 MiB or
`0x1000`; the loader then fails cleanly, because the kernel is not
physically relocatable. Firmware using 5-level paging (LA57) is refused.
Some recent platforms gate off the legacy PIT, the kernel's only timer.
