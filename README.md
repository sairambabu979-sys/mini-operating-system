# mini-operating-system

A small x86_64 kernel written from scratch in C17 and NASM. It boots from
its own two-stage BIOS loader or its own UEFI loader, runs on its own 4-level page tables with
W^X enforced everywhere, handles every CPU exception and hardware
interrupt, preemptively schedules kernel threads and ring-3 processes,
and isolates those processes from the kernel and from each other.

Every feature ships with a test that boots in CI.

```
$ make run
1234567
cpu: AuthenticAMD, 40-bit physical / 57-bit virtual; nx=1 pge=1 1g=1 smep=1 smap=1 apic=1 x2apic=0
vmm: direct map: 127 MiB at 0xffff800000000000
pmm: 125 MiB managed, 768 KiB metadata at 0x1ac000, highest frame 0x7fdf000
screen: VGA text mode 3, 80x25 at 0xb8000
acpi: RSDP at 0xf5290: revision 0, OEM "BOCHS ", RSDT at 0x7fe1c52
acpi: 4 tables: FACP APIC HPET WAET
acpi: MADT: 1 CPU(s), 1 I/O APIC(s), local APIC at 0xfee00000, 8259 present
acpi:   ISA IRQ 0 -> GSI 2, edge, active high
...
ioapic: I/O APIC 0 at 0xfec00000: version 0x20, GSIs 0-23
lapic: id 0, version 0x14, 6 LVT entries, at 0xfee00000
irq: using the local APIC and 24 I/O APIC pin(s)
hpet: 100.000000 MHz, 64-bit counter, 3 comparators, at 0xfed00000
time: tick: local APIC timer at 1000 Hz (1000004 ns/tick); TSC 2100.897 MHz; calibrated against the HPET
sched: round robin, 10 ms slices, 16 KiB kernel stacks
minios 0.1.0 (x86_64, debug build)
...
init: pid 2, starting 11 programs
hello: hello from ring 3, pid 3
faulter: writing to address 0
WARNING: proc: faulter[10]: killed by #PF page fault at 0x400020 (address 0, error 0x6)
privileged: trying cli
WARNING: proc: privileged[11]: killed by #GP general protection at 0x400020 (error 0)
kread: reading the kernel image at 0xffffffff80100000
WARNING: proc: kread[13]: killed by #PF page fault at 0x400020 (address 0xffffffff80100000, error 0x5)
badptr: 0 failure(s)
spinner[4]: done after 152 ms of busy work
sleeper: slept 3 x 40 ms, 142 ms elapsed
fpu[7]: XMM state intact across 300 yields
init: all children reaped, 0 unexpected
```

`1234567` is the bootloader's progress trail (MBR, stage 2, A20, memory
map, kernel loaded, long mode, ELF loaded). After that come the kernel and
then the ring-3 programs, some of them deliberately hostile.

## Quick start

Requirements: `clang`, `lld`, `llvm`, `nasm`, `gcc`, `make`, `python3`, `qemu-system-x86`,
and for UEFI `ovmf` and `mtools`. On Ubuntu 24.04:
`apt install clang lld llvm nasm gcc make python3 qemu-system-x86 ovmf mtools`.

```sh
make            # bootloader, kernel, user programs, disk images  -> build/debug/
make run        # boot in QEMU, console on stdio (Ctrl-C to quit)
make run-uefi   # the same kernel through OVMF and the UEFI loader
make test       # every test suite below
make check      # format + lint + test: exactly what CI checks; run before committing
```

| Option | Effect |
|--------|--------|
| `CONFIG=release` | optimized build, no kernel UBSan |
| `V=1` | show full commands |
| `CMDLINE="..."` | kernel options for `make run` (see below) |
| `QEMU_DISPLAY=gtk` | also open a window showing the screen console (`none` by default) |
| `make bench` | microbenchmarks |
| `make debug` | QEMU waits for gdb on `:1234` (`gdb -x tools/gdb/gdbinit build/debug/kernel.elf`); adds `minios-tasks` and `minios-ptwalk ADDR` |
| `make lint` / `make format` | clang-tidy, clang-format |

Kernel command line (`CMDLINE=` for `make run`; on the ESP, `/minios/cmdline`):

| Option | Effect |
|--------|--------|
| `loglevel=debug\|info\|warn\|err` | console verbosity |
| `console=serial` | no screen console, COM1 only |
| `noapic` | 8259 PIC and PIT tick instead of the I/O APIC and LAPIC timer |
| `nohpet` | calibrate against PIT channel 2 instead of the HPET |
| `ktest` | run the in-kernel test suite, report TAP, exit QEMU |
| `bench` | run the microbenchmarks, exit QEMU |
| `exit-after-init` | exit QEMU with `init`'s status |
| `selftest=panic\|hang\|triplefault` | deliberately fail, to test the crash path and the test runner |

## What is in it

| Area | Highlights |
|------|------------|
| **Boot** | Own UEFI loader (PE32+, no SDK) and own BIOS loader, with an identical hand-off: one kernel binary, one test suite, both firmwares. BIOS path: own MBR + stage 2. A20 by three methods, each verified. E820 map. Unreal-mode kernel load. Real mode straight to long mode. ELF64 loading with full bounds checks. A versioned `boot_info` ABI defined once in C, with NASM offsets generated from it. |
| **Traps** | GDT/TSS with IST stacks for `#DF`, NMI and `#MC`. 256 generated entry stubs, one trap-frame layout. Decoded panics with symbolized backtraces (two-pass symbol-table link). Exception-table fixups. |
| **Memory** | E820 sanitizer. Buddy allocator (pure, fuzzed on the host). Own page tables bootstrapped without an allocator. Direct map of RAM only: non-executable, with the kernel image read-only through it. NX, WP, SMEP, SMAP. Guard-paged `vmalloc`. Slab `kmalloc` with use-after-free and double-free detection. |
| **Console** | COM1, plus a screen: UEFI GOP framebuffer (write-combining) or BIOS VGA text, so a real machine without a serial port shows the boot and any panic. A log ring replays the boot onto the screen. Drawing never runs with interrupts off once they are on, because a slow scroll would cost the clock ticks. |
| **Interrupts** | ACPI discovery (RSDP, RSDT/XSDT, MADT, HPET; parser fuzzed on the host). Local APIC + I/O APIC with interrupt source overrides honored. LAPIC timer and TSC calibrated against the HPET (or PIT channel 2), with each endpoint bracketed so a preemption cannot skew it. The 8259 + PIT path kept as a tested fallback (`noapic`). IRQ-safe spinlocks that report recursive locking instead of deadlocking. |
| **Scheduling** | Preemptive round robin. Preemption on interrupt exit, only when no lock is held. Wait queues that cannot lose wakeups, even when preempted mid-wait. Sleeping mutexes, sleep, exit codes, orphan adoption. |
| **Processes** | Per-process address spaces, ELF loader, syscall/sysret. SYSRET avoided for non-canonical returns (CVE-2012-0217), with recovery when `iretq` itself faults. Validated user copies. Eager FPU switching. Faults kill only the faulting process. |
| **Hardening** | W^X in kernel and user space, kernel UBSan, stack protector, guard pages, poisoned heap, and build-time checks for the flags whose absence corrupts silently. |

## Architecture

```
 ┌───────────────────────────────────────────────────────────────────────┐
 │  ring 3   init · hello · spinner · sleeper · fpu · badptr · hostile …  │
 │           user/lib: crt0, syscall wrappers, printf                     │
 ├───────────────── syscall / sysret · interrupts · exceptions ──────────┤
 │  ring 0                                                               │
 │   proc/    process spawn, ELF validation, syscalls, user copies       │
 │   sched/   round robin, wait queues, sleep, mutex   ── switch.asm     │
 │   mm/      vmalloc · kmalloc · VMM (page tables) · PMM (buddy)        │
 │   core/    panic + ksyms, klog, irq, time, UBSan, ktest, bench        │
 │   arch/    GDT/TSS, IDT + isr.asm, syscall.asm, FPU, per-CPU, LAPIC   │
 │   acpi/    RSDP/RSDT/XSDT discovery, MADT + HPET parsing (pure)       │
 │   drivers/ serial, screen, I/O APIC, HPET, PIT, PIC, QEMU debug exit  │
 ├───────────────────────────────────────────────────────────────────────┤
 │  boot/   stage1 (MBR) → stage2: A20, E820, unreal mode, long mode, ELF │
 │          efi/loader.c: UEFI → same boot_info, same page tables          │
 └───────────────────────────────────────────────────────────────────────┘
```

Virtual memory:

| Range | Contents |
|-------|----------|
| `0x0000000000010000 – 0x00007FFFFFFFFFFF` | user space: program, stack at `0x7FFFFFFF0000` |
| `0xFFFF800000000000` + 512 GiB | direct map of physical RAM (no MMIO holes) |
| `0xFFFFC00000000000` + 512 GiB | vmalloc: guard-paged kernel stacks, `iomap()` device and firmware mappings |
| `0xFFFFFFFF80000000` – top | kernel image (`.text` R-X, `.rodata` R--, `.data/.bss` RW-) |

## Testing

`make test` runs all of these; CI runs them for both debug and release.

| Layer | What runs | Where |
|-------|-----------|-------|
| Host unit tests | Kernel code built for Linux under ASan + UBSan. printf against glibc (20k random formats); memory-map sanitizer against brute force; buddy allocator fuzz with full verification; ELF validator and ACPI table parser against hostile and randomly corrupted input; the console's text grid against a reference terminal model on random output, and its pixel drawing exact to the byte. | `tests/host/` |
| Binary checks | No SSE/AVX in kernel code; W^X segment layout; entry point. Build-time guard for `-mno-red-zone`, `-mgeneral-regs-only`, `-mcmodel=kernel`. | `tools/check_kernel.py`, `Makefile` |
| In-kernel tests | 98 `KTEST()`s, TAP over serial, booted with **128 MiB and 4 GiB** of RAM, on the APIC path with and without an HPET, and on the legacy 8259 path. Every protection is checked by attempting the forbidden access. | `tests/kernel/` |
| Crash path | A deliberate kernel page fault; the panic decode and backtrace are matched against patterns. | `disk-panic.img` |
| Whole system | Boots the real `init`, which runs 11 programs and passes only if each ends as expected. | `disk-demo.img` |
| UEFI | The whole in-kernel suite and the `init` demo again, booted through OVMF and the UEFI loader. | `disk-uefi-*.img` |
| Benchmarks | Smoke run of `make bench`. | `disk-bench.img` |

Some of what the in-kernel tests prove:
- a real double fault, and a real kernel stack overflow, each caught on
  its IST stack;
- four never-yielding threads all preempted, and fair to within 25%;
- a 3000-item producer/consumer with no lost wakeup;
- processes that dereference NULL, run `cli`, read kernel memory or patch
  their own code are killed, and only they are;
- hostile syscall arguments all fail with the right errno;
- the PIT, registered as ISA IRQ 0, arrives through the firmware's
  override onto GSI 2, and masking it at the I/O APIC stops it;
- the SYSRET hazard is avoided, and a faulting `iretq` kills only the task;
- FPU state never leaks between processes;
- memory is fully reclaimed after hundreds of threads and processes.

The QEMU runner fails on timeout, triple fault, kernel-reported failure or
a TAP count mismatch, and `make test` proves it: `tests/qemu/check_runner.py`
boots kernels that hang and that triple-fault, and requires the runner to
reject each for the right reason. Timing-sensitive tests were run repeatedly with up to 10
QEMU instances competing for the host's cores.

## Benchmarks

Release build, QEMU TCG (software emulation) on a 2.1 GHz host, median of
9 batches. Under TCG these figures measure the emulator as much as the
kernel; run `make bench QEMU_EXTRA="-enable-kvm -cpu host"` on a KVM host
for hardware numbers.

| Operation | Cycles |
|-----------|-------:|
| context switch (kernel thread to kernel thread) | 412 |
| syscall round trip (`getpid`, from ring 3) | 472 |
| `kmalloc` + `kfree`, 64 B | 552 |
| `pmm_alloc_page` + `pmm_free_page` | 1005 |

## Known limitations

These are deliberate scope cuts (see [`docs/PLAN.md`](docs/PLAN.md)):

- **One CPU.** Locks use atomic exchange and are written for SMP, but
  there is no AP bring-up, no per-CPU run queues and no TLB shootdown.
- **ISA interrupts only.** No PCI interrupt routing, no MSI, and no ACPI AML
  interpreter (so no ACPI power-off or sleep states).
- **No filesystem.** User programs are embedded in the kernel image and
  started by name; there is `spawn`, but no `fork`/`exec`.
- **No signals, pipes or IPC.** Processes talk only through exit statuses.
- **Output only:** serial and screen, but no keyboard driver yet.
- **Not physically relocatable.** Both loaders place the kernel at 1 MiB. The
  UEFI loader asks the firmware for that range and fails cleanly if it is
  taken, which some real firmware may do.
- **x2APIC handed back to xAPIC.** If the firmware enables x2APIC mode, the
  kernel returns to xAPIC (an SDM-sanctioned transition). QEMU's TCG cannot
  emulate x2APIC, so that path has not run.
- **Only partly tested under TCG.** On real CPUs a non-canonical RIP
  in `iretq` raises `#GP` in ring 0, the path the exception-table
  recovery handles. TCG skips that check, so that exact path is exercised
  with a different bad frame (an invalid SS).

## Documentation

- [`docs/PLAN.md`](docs/PLAN.md): architecture, decisions, milestones, risks, and how the build differed from the plan
- [`docs/design/boot.md`](docs/design/boot.md): BIOS and UEFI boot paths, memory layout, debugging
- [`docs/design/traps.md`](docs/design/traps.md): GDT/TSS/IDT, exception policy, panic, sanitizers, legacy IRQs, locks
- [`docs/design/interrupts.md`](docs/design/interrupts.md): ACPI discovery, local APIC, I/O APIC, HPET, calibration
- [`docs/design/console.md`](docs/design/console.md): log ring, screen consoles, drawing without losing ticks
- [`docs/design/memory.md`](docs/design/memory.md): memory map, buddy allocator, page tables, heaps
- [`docs/design/scheduler.md`](docs/design/scheduler.md): context switch, preemption, lost-wakeup safety
- [`docs/design/usermode.md`](docs/design/usermode.md): processes, syscall entry/exit, SYSRET hazard, user memory access

## Repository layout

```
boot/            stage1.asm (MBR), stage2.asm, layout.inc; efi/ (UEFI loader)
kernel/arch/     x86_64: entry, GDT/TSS/IDT, isr/syscall/switch asm, FPU, user copies, LAPIC
kernel/acpi/     table discovery and (pure) parsing
kernel/core/     kmain, panic, klog, irq, time, ksyms, UBSan, ktest, bench
kernel/mm/       memmap, buddy, pmm, vmm, vmalloc, kmalloc
kernel/sched/    scheduler, mutex
kernel/proc/     processes, ELF validation, syscalls
kernel/drivers/  serial, screen (framebuffer, VGA text), I/O APIC, HPET, PIT, PIC, QEMU exit
kernel/lib/      printf, strings, cmdline, text grid, framebuffer drawing, font
kernel/include/  headers (uapi/ is shared with user space)
user/            crt0, libsys, programs/
tests/           host/ (Linux), kernel/ (KTEST), qemu/run_tests.py
tools/           mkimage, mkesp, gen_ksyms, gen_user_images, check_kernel, gdb/
```

## License

MIT, see [`LICENSE`](LICENSE). The console font (`kernel/lib/font8x8.c`) is Daniel
Hepper's public-domain font8x8, after the IBM VGA fonts.
