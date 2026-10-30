# Boot

How a powered-on PC gets from the BIOS to `kmain()`. Sources: `boot/stage1.asm`,
`boot/stage2.asm`, `tools/mkimage.py`, `kernel/include/boot/boot_info.h`.

## Disk image

```
LBA 0        stage 1: MBR (446 B code, 1 active partition entry, 0x55AA)
LBA 1..63    stage 2: exactly 63 sectors; header at byte 4
LBA 64..     kernel ELF (stripped), padded to a whole sector, or to a whole
             page (8 sectors) when an initrd follows
then         the initrd (a ustar archive), padded to a whole sector
```

`mkimage.py` patches the stage-2 header with the kernel's start LBA, its
sector count, the kernel command line, and the initrd's sector count and
size in bytes (both 0 for none). The loader has no hard-coded sizes.
Stage 2 refuses (`!K`) an initrd header whose size does not need exactly
its sector count, or one that follows a kernel of a part page.
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
| `0x1000000-` | staging area: the raw kernel ELF, then the initrd, page-aligned right after it (all checked against E820 before use, and to end below 4 GiB) |

## Sequence

| Code | Step |
|------|------|
| `1` | Stage 1: normalize `CS:IP`, check INT 13h extensions, read stage 2 (LBA mode), jump. |
| `2` | Stage 2 running; COM1 initialized; `boot_info` page cleared. |
| `3` | A20 enabled: BIOS `2401h` → 8042 output port → port `0x92`, each verified by a wraparound test. A first test skips all three when A20 is already on. |
| `4` | E820 map collected (20- and 24-byte replies, the ACPI 3.0 "enabled" attribute ignored, zero-length entries dropped); staging area verified as usable RAM. |
| `5` | CPU checked for long mode; kernel and initrd read in 32 KiB chunks and copied above 1 MiB through **unreal mode** (re-entered after every BIOS call, since a BIOS may reload segment registers). The initrd stays where it was staged, and `boot_info.initrd_phys` and `initrd_size` say where. |
| `6` | Page tables built; PICs masked, NMI disabled, null IDT loaded; **real mode → long mode in one step** (`CR4.PAE`, `EFER.LME`, then `CR0.PE`+`CR0.PG` together). |
| `7` | ELF validated (class, endianness, type, machine, header sizes, entry in the higher half). Each `PT_LOAD` is bounds-checked against the file and the staging area, copied, and its `.bss` tail zeroed. `boot_info` is published with **magic written last**, then the loader jumps to `e_entry`. |

Errors print `!` and a letter, then halt. Each letter is listed at the top of
`stage2.asm`. The null IDT is deliberate: any interrupt before the kernel
installs its own IDT triple-faults immediately, instead of jumping through a
real-mode IVT that makes no sense in long mode.

**The E820 "enabled" bit.** ACPI 3.0 added an extended-attributes word to
E820 replies, whose bit 0 meant "use this entry". Later ACPI versions
deprecated it, and some BIOSes leave it clear on every entry, valid RAM
included. Stage 2 used to drop such entries, which on those machines
dropped all of RAM. It now ignores the bit and stores every entry with
`acpi_attrs = 1`; the UEFI loader does the same, so `acpi_attrs` is
always 1 in `boot_info` (`memory_map_is_sane` checks that bit 0 is set).

**A20 under test.** SeaBIOS hands over with A20 already enabled, so in a
normal QEMU boot the first wraparound test passes and none of the three
methods runs, and `a20_line_is_enabled` (`test_boot.c`) checks only the
final state. Three test builds of stage 2 cover the methods themselves:
`-DA20_SELFTEST=bios`, `kbc` or `fast`. Each one first turns A20 off by
all three methods (BIOS `2400h`, the 8042 output port, port `0x92`) and
halts with `!a` if addresses do not then wrap. It then lets only its own
method turn A20 back on and prints `A20 test: off, then on by <method>`.
The 8042 routine takes the gate bit in `AH`, so the same code turns A20
off and on. `make test` boots `disk-a20-bios.img`, `disk-a20-kbc.img` and
`disk-a20-fast.img` (the `init` demo) and requires that line and
`init exited with status 0`.

## The kernel's entry contract

- Long mode, paging on, interrupts off, NMI masked, PICs masked. Both
  loaders also leave DF clear (`_start` still does `cld`).
- Physical `0–1 GiB` mapped twice with 2 MiB pages: identity, and at
  `KERNEL_VMA = 0xFFFFFFFF80000000`.
- `RDI` = physical address of `struct boot_info`, 8-aligned and inside
  the first 1 GiB (stage 2 uses `0x1000`). No valid stack: `_start`
  loads its own before touching memory through `RSP`.

Each promise is checked at boot by `tests/kernel/test_boot.c`: the CPU is in
long mode, A20 is on, `.bss` is zero, the kernel sits at its link address,
the sections are page-aligned, the memory map is sane, and IF/DF were clear
at the hand-off.

The IF/DF check needs the loader's RFLAGS, which `_start` destroys with its
own `cli; cld`. So `_start` first loads its stack with `lea` (which leaves
RFLAGS alone), then saves RFLAGS with `pushfq` into `loader_rflags`, and only
then runs `cli; cld`. `boot_entry_rflags()` returns that saved value. The
first version read RFLAGS in `kmain`, after `_start` had cleared both flags,
so the check could not fail: a loader that jumped in with interrupts on
still passed.

## The initrd

Both loaders leave the initrd in RAM where they put it, page-aligned, and
neither reserves it: the BIOS staging area and the UEFI loader's pages are
usable RAM in the map the kernel gets. So the kernel does:

- `validate_boot_info` drops an initrd that is unaligned or does not fit in
  the direct map, with a warning;
- the PMM reserves the rest, page-rounded, whether or not it turns out to
  be a valid archive, and places its page array clear of it. With 4 GiB of
  RAM that array is 30 MiB and would otherwise start below the BIOS
  staging area and run over the initrd;
- `initrd_init` (`kernel/fs/initrd.c`), right after `mm_init`, requires it
  to be all usable RAM and a valid archive (`tar_validate`) with at least
  one entry, and logs
  `initrd: N entries, K KiB at ADDR`. One that fails is logged as an error
  and not used; none at all logs `initrd: none`.

Every user program runs from it (`bin/NAME`; see
[usermode.md](usermode.md)). The kernel tests check that it holds every
program the build made as a loadable ELF, under both loaders and with
4 GiB, and that draining the PMM never hands out one of its frames; every
test that runs a program reads it from there. `disk-noinitrd.img` and
`disk-badinitrd.img` (the kernel ELF given as the initrd) must panic,
saying there is no initrd to start `init` from.

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

A second loader, `BOOTX64.EFI`, gives the kernel **the same hand-off**:
`boot_info` passed in `RDI`, the kernel at its link address, the same
identity plus higher-half windows, interrupts off, PICs masked. Only where
`boot_info` lives differs: stage 2 uses `BOOT_INFO_PHYS` (`0x1000`); this
loader takes any page below 1 GiB the firmware has free, because a fixed
low page may be the firmware's. The kernel accepts any 8-aligned address
inside the 1 GiB boot window (`BOOT_WINDOW_SIZE`), copies `boot_info` into
its own `.data` at entry, and never touches the loader's page again, which
can therefore be ordinary RAM. One kernel binary therefore runs under both firmwares, and
`make test` boots the full in-kernel suite and the `init` demo both ways.
It also boots the UEFI demo with 8 GiB of RAM, where OVMF loads the
loader at 5 GiB: that run requires `loader image at` an address above
4 GiB and a clean end of `init`.
`boot_info.loader` records which loader ran (`boot_info` version 2), and
`boot_info.acpi_rsdp` carries the ACPI RSDP from the EFI configuration
table (version 3; the BIOS loader leaves it 0 and the kernel scans for it),
`boot_info.framebuffer` the GOP framebuffer (version 4; see
[console.md](console.md)), and `boot_info.initrd_phys` and `initrd_size`
the initrd (version 5).

```
ESP (FAT32, built by tools/mkesp.py with mtools)
  /EFI/BOOT/BOOTX64.EFI   the loader
  /minios/kernel.elf      the same stripped kernel the BIOS image carries
  /minios/cmdline         the command line (one line)
  /minios/initrd          the initrd (optional)
```

| Step | Detail |
|------|--------|
| Claim memory | `AllocatePages(AllocateAddress)` for the kernel's physical range: if the firmware owns it, the loader stops with a message instead of overwriting firmware memory. `boot_info` gets any free page below 1 GiB (`AllocateMaxAddress`), logged with the kernel's range. |
| Load | Files are read through the volume the loader itself came from (LoadedImage → SimpleFileSystem). The kernel ELF gets the same checks as in stage 2, then is copied and its `.bss` zeroed **while boot services still run**. Then the initrd, if there is one, is read into pages of its own below 1 GiB (`AllocateMaxAddress`, after the kernel's range is claimed, so it cannot land there) and logged. |
| ACPI | The RSDP from the configuration table: the ACPI 2.0 GUID first, 1.0 accepted. |
| Framebuffer | The Graphics Output Protocol's current mode, unchanged: base, size, pitch, and the channel layout (RGB, BGR, or byte-aligned bit masks). A Blt-only GOP, or none, leaves the console on serial. |
| Page tables | 0–4 GiB identity and 0–1 GiB at `-2 GiB`, with 2 MiB pages. The loader keeps running after the CR3 switch, so it also identity-maps its own image and the stack around its current `RSP`, wherever the firmware put them, and logs `loader image at ..., stack near ...`. The 8 GiB run tests the image mapping; OVMF keeps the stack below 4 GiB, so mapping a stack above it (nothing on the stack is used after the switch) has not run. |
| Exit | The final `GetMemoryMap` is followed immediately by `ExitBootServices`, with nothing allocating or printing in between, retried if the map key went stale. |
| Memory map | EFI descriptors become E820 entries. Loader and boot-services memory and conventional memory become usable; ACPI types are kept; runtime services, MMIO and everything unknown become reserved. Contiguous same-type runs are merged. |
| Jump | `cli`, PICs masked, CR3 switched, `RDI = &boot_info`, jump to the ELF entry. |

The loader is a PE32+ built with clang (`--target=x86_64-unknown-windows`)
and `lld-link`, with no UEFI SDK: `boot/efi/efi.h` declares the slice of
the specification it uses, with `static_assert`ed table offsets. The
Microsoft x64 ABI is LLP64 (`long` is 32 bits), so every 64-bit value is
printed with `%ll`.

**Real-hardware caveats.** Firmware may occupy physical 1 MiB; the
loader then fails cleanly, because the kernel is not physically
relocatable. Firmware using 5-level paging (LA57) is refused. No test
reaches either.
