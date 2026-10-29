# mini-operating-system

A small x86_64 kernel written from scratch in C17 and NASM. It boots from
its own two-stage BIOS loader or its own UEFI loader, runs on its own 4-level page tables with
W^X enforced everywhere, handles every CPU exception and hardware
interrupt, preemptively schedules kernel threads and ring-3 processes,
and isolates those processes from the kernel and from each other. A small
shell, typed at on the PS/2 keyboard or over the serial line, runs those
programs by name.

Nearly every feature ships with a test that boots in CI; the main paths no
test reaches are listed under [Known limitations](#known-limitations).

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
minios 1.1.0 (x86_64, debug build)
...
init: pid 3, starting 11 programs
hello: hello from ring 3, pid 4
faulter: writing to address 0
WARNING: proc: faulter[11]: killed by #PF page fault at 0x400020 (address 0, error 0x6)
privileged: trying cli
WARNING: proc: privileged[12]: killed by #GP general protection at 0x400020 (error 0)
kread: reading the kernel image at 0xffffffff80100000
WARNING: proc: kread[13]: killed by #PF page fault at 0x400020 (address 0xffffffff80100000, error 0x5)
badptr: 0 failure(s)
spinner[5]: done after 152 ms of busy work
sleeper: slept 3 x 40 ms, 142 ms elapsed
fpu[8]: XMM state intact across 300 yields
init: all children reaped, 0 unexpected
```

`1234567` is the bootloader's progress trail (MBR, stage 2, A20, memory
map, kernel loaded, long mode, ELF loaded). After that come the kernel and
then the ring-3 programs, some of them deliberately hostile.

## Quick start

Requirements:
- to build (`make`): `clang`, `lld`, `llvm`, `nasm`, `make`, `python3`, and `mtools` for
  the UEFI images;
- to run and test: `qemu-system-x86`, `ovmf` (the UEFI boots), `gcc` (the host unit
  tests) and `gdb` (`make test` drives the gdb helpers against a live kernel);
- for `make check`, `make lint` and `make format`: `clang-format` and `clang-tidy`;
- for `make test-x2apic`: QEMU 9.0 or later, named with `QEMU=`. Ubuntu 24.04 ships
  QEMU 8.2, which cannot emulate x2APIC under TCG.

On Ubuntu 24.04 (everything but the newer QEMU):

```sh
apt install clang lld llvm nasm gcc make python3 qemu-system-x86 ovmf mtools gdb clang-format clang-tidy
```

```sh
make            # bootloader, kernel, user programs, disk images  -> build/debug/
make run        # boot in QEMU; this terminal is COM1 (Ctrl-A x quits)
make run-uefi   # the same kernel through OVMF and the UEFI loader
make run-shell  # the shell (init=sh), typed at from this terminal; Ctrl-A x quits
make test       # every test suite below but the x2APIC run, for one CONFIG
make check      # check-format + lint + test, for one CONFIG; run before committing
```

A plain `make check` (`CONFIG=debug`) runs CI's lint job and the tests of its
debug job. CI (`.github/workflows/ci.yml`) also builds (`make`) and tests (`make
test`) `CONFIG=release`, and its `x2apic` job builds QEMU 9.2.0 and runs
`make test-x2apic` with it (debug build). So everything CI runs is `make all
check`, then `make CONFIG=release all test`, then `make test-x2apic
QEMU=<a QEMU 9.x qemu-system-x86_64>`. CI lints with the debug flags only.

| Option | Effect |
|--------|--------|
| `CONFIG=release` | the same `-O2 -g`, without kernel UBSan and without the `CONFIG_DEBUG` checks: kmalloc poison, redzones and double-free scan, `buddy_verify()` at boot, `assert()`, and the panic when the screen is drawn with interrupts off or preemption on |
| `V=1` | show full commands |
| `CMDLINE="..."` | kernel options for `make run` and `make run-uefi` (see below) |
| `QEMU_DISPLAY=gtk` | also open a window showing the screen console (`none` by default); with `make run-shell`, typing in it reaches the PS/2 keyboard |
| `make run-shell` / `make run-shell-uefi` | boot the shell image; as with `make run`, this terminal is COM1 (Ctrl-C goes to the guest, Ctrl-A x quits QEMU) |
| `make bench` | microbenchmarks |
| `make test-x2apic QEMU=...` | the in-kernel suite in x2APIC mode; needs QEMU 9.0 or later (see [Testing](#testing)) |
| `make debug` | QEMU waits for gdb on `:1234` (`gdb -x tools/gdb/gdbinit build/debug/kernel.elf`); adds `minios-tasks` and `minios-ptwalk ADDR` |
| `make lint` / `make format` | clang-tidy, clang-format |

Kernel command line (`CMDLINE=` for `make run` and `make run-uefi`; on the ESP,
`/minios/cmdline`). Options are separated by spaces; the whole line is at most 127
bytes (`mkimage.py` refuses a longer one, and the UEFI loader stops at 127 bytes or
the first line break). Unknown options are ignored.

| Option | Effect |
|--------|--------|
| `loglevel=err\|warn\|info\|debug` | console verbosity (default `info`); any other value is ignored with a warning |
| `console=serial` | no screen console, COM1 only; any other value is ignored with a warning |
| `noapic` | 8259 PIC and PIT tick instead of the I/O APIC and LAPIC timer |
| `x2apic` | switch the local APIC from xAPIC to x2APIC mode, if the CPU has x2APIC; no effect on the 8259 path |
| `nohpet` | calibrate against PIT channel 2 instead of the HPET |
| `ktest` | run the in-kernel test suite, report TAP, exit QEMU with its result |
| `bench` | run the microbenchmarks, exit QEMU |
| `init=NAME` | start the program `NAME` (1 to 15 characters) as the first process instead of `init`; a bad name panics |
| `exit-after-init` | exit QEMU when the first process exits: success if its status is 0, failure otherwise |
| `selftest=panic` | a deliberate kernel page fault, to test the panic output |
| `selftest=hang`, `selftest=triplefault` | halt forever with interrupts off, or triple-fault: kernels the test runner must reject. Any other `selftest=` value panics |

`ktest`, any `selftest=` and `exit-after-init` also make a panic exit QEMU with
a failure status instead of halting, so a crashed test run ends at once. The test images are the
same kernel with a fixed command line: `disk-ktest.img` and
`disk-uefi-ktest.img` (`ktest`), `disk-ktest-noapic.img` (`ktest noapic`),
`disk-ktest-nohpet.img` (`ktest nohpet`), `disk-ktest-quiet.img` (`ktest
console=serial loglevel=warn`), `disk-ktest-x2apic.img` (`ktest x2apic`),
`disk-panic.img` (`selftest=panic`), `disk-demo.img`, `disk-uefi-demo.img` and
the three `disk-a20-*.img` (`exit-after-init`), `disk-shell.img` and
`disk-uefi-shell.img` (`init=sh exit-after-init`), `disk-badinit.img`
(`init=no-such-prog exit-after-init`), `disk-bench.img` (`bench`), and the two
`disk-selftest-*.img`.

## What is in it

| Area | Highlights |
|------|------------|
| **Boot** | Own UEFI loader (PE32+, no SDK) and own BIOS loader, with the same hand-off: one kernel binary, one test suite, both firmwares. BIOS path: own MBR + stage 2. A20 by three methods (BIOS call, 8042 keyboard controller, port `0x92`), each followed by a wraparound check; since SeaBIOS starts with A20 on, three test builds of stage 2 turn it off and let only one method turn it back on. E820 map. Unreal-mode kernel load. Real mode straight to long mode. ELF64 loading with full bounds checks. A versioned `boot_info` ABI defined once in C, with NASM offsets generated from it, passed by address: the BIOS loader uses page `0x1000`, the UEFI loader any page below 1 GiB the firmware has free, and the kernel copies it at entry. The UEFI loader runs wherever the firmware loads it, above 4 GiB included. |
| **Traps** | GDT/TSS with IST stacks for `#DF`, NMI and `#MC`. 256 generated entry stubs, one trap-frame layout. Decoded panics with symbolized backtraces (two-pass symbol-table link). Exception-table fixups. |
| **Memory** | E820 sanitizer. Buddy allocator (pure, fuzzed on the host). Own page tables bootstrapped without an allocator. Direct map of RAM only: non-executable, with the kernel image read-only through it. NX, WP, SMEP, SMAP. Guard-paged `vmalloc` that reuses a freed range, for the same size, only after 16 newer frees. Slab `kmalloc`, with use-after-free, double-free and overflow detection in debug builds. |
| **Console** | COM1, plus a screen: UEFI GOP framebuffer (write-combining) or BIOS VGA text, so a machine without a serial port can still show the boot and later panics, if its firmware leaves VGA text mode or a linear 32-bit GOP framebuffer. A log ring replays the boot onto the screen. Outside a panic, drawing never runs with interrupts off once they are on, because a slow scroll would cost the clock ticks. |
| **Interrupts** | ACPI discovery (RSDP, RSDT/XSDT, MADT, HPET; parser fuzzed on the host). Local APIC (xAPIC, or x2APIC when the firmware chose it or the `x2apic` option asks) + I/O APIC with interrupt source overrides honored. LAPIC timer and TSC calibrated against the HPET (or PIT channel 2), with each endpoint bracketed so a preemption cannot skew it, every wait bounded and implausible rates rejected: a broken HPET falls back to the PIT, and no working clock at all is a clear panic, not a hang. The 8259 + PIT path kept as a tested fallback (`noapic`). IRQ-safe spinlocks that report recursive locking instead of deadlocking. |
| **Scheduling** | Preemptive round robin. Preemption on interrupt exit, only when no lock is held. Wait queues that cannot lose wakeups, even when preempted mid-wait. Sleeping mutexes, sleep, exit codes, orphan adoption. |
| **Processes** | Per-process address spaces, ELF loader, syscall/sysret. SYSRET avoided for non-canonical returns (CVE-2012-0217), with recovery when `iretq` itself faults. Validated user copies. Eager FPU switching, from a zeroed initial state. Faults kill only the faulting process (a machine check panics). RFLAGS.NT and AC set in ring 3 can neither crash another task nor switch SMAP off. |
| **Input** | PS/2 keyboard through the 8042 (translation to scan code set 1, US layout, every controller wait bounded, the A20 and reset lines never touched) on IRQ 1, and COM1 receive on IRQ 4, into one queue that `read()` drains: raw bytes, no echo. `read()` blocks without losing a wakeup and rejects a bad buffer before taking a byte. The shell edits the line itself (Backspace, Ctrl-U, Ctrl-C, Ctrl-D; escape sequences swallowed) and runs programs by name. |
| **Hardening** | W^X in kernel and user space, kernel UBSan, stack protector, guard pages, poisoned heap (debug builds), and build-time checks for the flags whose absence corrupts silently. |

## Architecture

```
 ┌───────────────────────────────────────────────────────────────────────┐
 │  ring 3   init · sh · hello · spinner · sleeper · fpu · badptr · …     │
 │           user/lib: crt0, syscall wrappers, printf, line editor        │
 ├───────────────── syscall / sysret · interrupts · exceptions ──────────┤
 │  ring 0                                                               │
 │   proc/    process spawn, ELF validation, syscalls, user copies       │
 │   sched/   round robin, wait queues, sleep, mutex   ── switch.asm     │
 │   mm/      vmalloc · kmalloc · VMM (page tables) · PMM (buddy)        │
 │   core/    panic + ksyms, klog, irq, time, input, UBSan, ktest, bench │
 │   arch/    GDT/TSS, IDT + isr.asm, syscall.asm, FPU, per-CPU, LAPIC   │
 │   acpi/    RSDP/RSDT/XSDT discovery, MADT + HPET parsing (pure)       │
 │   drivers/ serial, 8042, screen, I/O APIC, HPET, PIT, PIC, QEMU exit  │
 ├───────────────────────────────────────────────────────────────────────┤
 │  boot/   stage1 (MBR) → stage2: A20, E820, unreal mode, long mode, ELF │
 │          efi/loader.c: UEFI → same boot_info, same page tables          │
 └───────────────────────────────────────────────────────────────────────┘
```

Virtual memory:

| Range | Contents |
|-------|----------|
| `0x0000000000010000 – 0x00007FFFFFFFFFFF` | user space: program, stack at `0x7FFFFFFF0000` |
| `0xFFFF800000000000` + 512 GiB | direct map of physical RAM (no MMIO holes; RAM above 512 GiB is not used) |
| `0xFFFFC00000000000` + 512 GiB | vmalloc: guard-paged kernel stacks, `iomap()` device and firmware mappings |
| `0xFFFFFFFF80000000` – top | kernel image (`.text` R-X, `.rodata` R--, `.data/.bss` RW-) |

## Testing

`make test` runs all of these but the x2APIC run, for one CONFIG; CI runs them for
both debug and release, and the x2APIC run in a job of its own.

| Layer | What runs | Where |
|-------|-----------|-------|
| Host unit tests | Kernel code built for Linux under ASan + UBSan. printf against glibc (20k random formats); memory-map sanitizer against brute force; buddy allocator fuzz with full verification; ELF validator and ACPI table parser against hostile and randomly corrupted input; the console's text grid against a reference terminal model on random output, and its pixel drawing exact to the byte. | `tests/host/` |
| Binary checks | No SSE/AVX in kernel code; W^X segment layout; entry point. Build-time guard for `-mno-red-zone`, `-mgeneral-regs-only`, `-mcmodel=kernel`. | `tools/check_kernel.py`, `Makefile` |
| In-kernel tests | 141 `KTEST()`s (134 in release builds, which compile out the 7 that test debug-only checks), TAP over serial, booted with **128 MiB and 4 GiB** of RAM, on the APIC path with an HPET, without one, with `nohpet` and without a PIT, and on the legacy 8259 path: chosen by `noapic`, or automatically on a CPU without a local APIC. A machine with neither clock, and the 8259 path without a PIT, must panic with a clear message. Each run states its exact number of skipped tests (from 1 on the default path to 11 on the 8259 path without an HPET) and, except under `loglevel=warn`, which hides the line, the clock the calibration must report. Every protection is checked by attempting the forbidden access. | `tests/kernel/` |
| Crash path | A deliberate kernel page fault; the panic decode and backtrace are matched against patterns. | `disk-panic.img` |
| Whole system | Boots the real `init`, which runs 11 programs and passes only if each ends as expected; again on a machine without ACPI, where the kernel must choose the 8259 and the PIT by itself. | `disk-demo.img` |
| Shell | The shell image, typed at for real: on the PS/2 keyboard through QMP `send-key` (on the APIC path, the 8259 path and UEFI) and on COM1 with no 8042. Each keystroke goes through the 8042 or the UART, IRQ 1 or 4, the scan code decoder, `read()` and the line editor; the runs check the echo, the editing keys, each program's `[exit N]`, and the not-found, name-too-long and no-arguments messages. A key pressed during boot must still leave a keyboard, and bytes typed during boot must arrive in order. A bad `init=` must panic with its name. | `disk-shell.img`, `disk-uefi-shell.img`, `disk-badinit.img` |
| UEFI | The whole in-kernel suite and the `init` demo again, booted through OVMF and the UEFI loader. The demo boots a second time with 8 GiB of RAM, where OVMF loads the loader above 4 GiB; the run requires the loader's logged address to be above 4 GiB. Without a GOP (`-vga none`) and with a Blt-only one, the console must stay on serial; a panic drawn on the framebuffer must complete with no nested panic. | `disk-uefi-*.img` |
| A20 | Three test builds of stage 2, one per method (`bios`, `kbc`, `fast`). Each turns A20 off, checks that addresses wrap, lets only its own method turn A20 back on, prints `A20 test: off, then on by <method>`, and boots through to the end of `init`. | `disk-a20-*.img` |
| x2APIC | The whole in-kernel suite with the `x2apic` option, which switches the local APIC to x2APIC mode; a test requires that mode. Needs QEMU 9.0 or later, so it is a separate target: `make test-x2apic QEMU=...`. CI builds QEMU 9.2.0 for it. | `disk-ktest-x2apic.img` |
| Benchmarks | Smoke run of `make bench`. | `disk-bench.img` |
| gdb helpers | `minios-tasks` and `minios-ptwalk` run by gdb against a live kernel, output matched against patterns (`make gdb-test`). | `tests/gdb/` |

Some of what the in-kernel tests prove:
- a real double fault, and a real kernel stack overflow, each caught on
  its IST stack;
- four never-yielding threads all preempted, each within one 10-tick slice of the others' CPU time;
- a 3000-item producer/consumer with no lost wakeup;
- processes that dereference NULL, run `cli`, read kernel memory or patch
  their own code are killed, and only they are;
- hostile syscall arguments all fail with the right errno;
- the PIT, registered as ISA IRQ 0, arrives through the firmware's
  override onto GSI 2, and masking it at the I/O APIC stops it;
- the SYSRET hazard is avoided, and a faulting `iretq` kills only the task;
- FPU state never leaks between processes, and a new process starts with
  zeroed FPU and SSE registers;
- a process that keeps RFLAGS.NT and AC set across syscalls and timer
  interrupts crashes neither the kernel nor another process, and an
  unmasked x87 error raises `#MF` and kills only the process that caused it;
- the calibrated TSC and LAPIC timer rates agree with the test's own
  PIT channel 2 window (the LAPIC timer measured through the TSC), within
  2% plus the measured uncertainty of the endpoints;
- memory is reclaimed to the frame after hundreds of threads and dozens
  of processes, a zombie holds no user memory, and task creation stops
  cleanly at its limit.

The QEMU runner fails on timeout, triple fault, kernel-reported failure or
a TAP count mismatch, and `make test` proves it: `tests/qemu/check_runner.py`
boots kernels that hang and that triple-fault, and requires the runner to
reject each for the right reason. It also fails a run whose number of
skipped tests differs from the one the Makefile gives it, whose output
matches a line the run must not print (`--reject`), or whose prompt never
appears before it types, all of which `check_runner.py` checks too. Timing-sensitive
tests were run repeatedly with up to 10 QEMU instances competing for the
host's cores.

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

These are deliberate scope cuts and known gaps (see [`docs/PLAN.md`](docs/PLAN.md)):

- **Never booted on real hardware.** Every test that boots the kernel,
  locally and in CI, runs under QEMU's TCG emulator (never KVM), on one CPU,
  with SeaBIOS or OVMF as the firmware; the host unit tests run natively.
  The code written for real machines (the UEFI loader's placement rules, the
  screen console, bounded clock calibration, x2APIC support) runs there only
  where QEMU can set the case up. Paths no test reaches include the
  firmware's x2APIC hand-off; the automatic switch to the 8259 for an I/O
  APIC that does not answer or an APIC ID above 255; an HPET that is
  rejected or broken, falling back to the PIT; the calibration timeouts and
  the implausible-rate check; a PIT that answers but does not count; the
  UEFI loader's stack mapping above 4 GiB, its failure when the firmware
  owns 1 MiB, its LA57 refusal, its rejection of a GOP it cannot draw on for
  any reason but Blt-only (24-bit pixels, unaligned channel masks, a
  framebuffer smaller than its mode, no mode information), and its choice of
  a second GOP when the console's is unusable; `loglevel=` values other than
  `warn`, and unknown `console=` and `loglevel=` values. No test checks what
  the screen shows after a panic. No real PC has run any of it, and the
  first one is likely to find something.
- **One CPU.** Locks use atomic exchange and are written for SMP, but
  there is no AP bring-up, no per-CPU run queues and no TLB shootdown.
- **ISA interrupts only.** No PCI interrupt routing, no MSI, and no ACPI AML
  interpreter (so no ACPI power-off or sleep states).
- **No filesystem.** User programs are embedded in the kernel image and
  started by name; there is `spawn`, but no `fork`/`exec`.
- **No signals, pipes or IPC.** Processes talk only through exit statuses.
- **Input from a PS/2 keyboard or COM1 only.** There is no USB stack: a USB
  keyboard can only reach it through the firmware's PS/2 emulation, and
  UEFI-booted PCs that drop that emulation, or have no 8042, have no keyboard
  (use serial). The keyboard tests type through QEMU's 8042 command 0xD2 and
  the serial tests through the UART's loopback; both are QEMU-only and would
  fail, not skip, on hardware without them. US layout, scan code set 1
  through the controller's translation only; no lock keys or LEDs (the keypad
  always types digits), no arrows, Home, End, Delete, F-keys or Alt, no mouse.
- **One shared console input.** Keyboard and COM1 bytes merge into one
  256-byte queue (the rest is dropped and counted), and the shell echoes to
  both the screen and COM1. There is no foreground process: with two readers,
  which one gets a byte is unspecified. `read()` blocks uninterruptibly, with
  no timeout; there are no signals, so Ctrl-C only cancels the line being
  typed, and a program that never gets input waits forever.
- **A minimal shell.** Programs run by name, without arguments; lines hold
  31 characters; no history. Echo comes from the shell, so typeahead shows
  only when it reads, and kernel messages printed mid-line are not redrawn.
  A serial terminal must be at least 36 columns wide for erasing to be exact.
  Outside `exit-after-init`, leaving the shell leaves the system idle. On a
  real UART a paste can overrun the 16-byte receive FIFO while the console
  transmits with interrupts off; overruns are counted, and QEMU cannot show
  them.
- **Serial output runs with interrupts off.** Every console write (a kernel
  message, or one 256-byte chunk of a `write()` call) holds the console lock,
  with interrupts off, while COM1 is polled byte by byte. On a real UART at
  115200 baud a 256-byte write takes about 22 ms (twice that if every byte
  is a newline, which goes out as `\r\n`), and the timer ticks missed
  meanwhile are lost: the clock runs slow while a process writes.
- **At most 256 tasks** (`TASK_MAX`), zombies and the boot and idle tasks
  included. Beyond that `spawn` fails with `-EAGAIN` and `kthread_create`
  returns NULL.
- **At most 512 GiB of RAM.** The direct map is 512 GiB, and RAM at physical
  addresses above it is left out of the allocator, with a warning that says
  how much.
- **About 24 GiB of RAM without 1 GiB pages.** The direct map's page tables
  come from a 32-page pool in the kernel image, before any allocator exists.
  With 1 GiB pages that covers all 512 GiB; on a CPU without them it runs
  out at about 24 GiB, and the boot panics saying so.
- **Not physically relocatable.** Both loaders place the kernel at 1 MiB. The
  UEFI loader asks the firmware for that range and fails cleanly if it is
  taken, which some real firmware may do.
- **x2APIC tested only partly, and only with QEMU 9.** If the firmware hands
  over in x2APIC mode, the kernel stays in it (firmware may lock it on) and
  drives the local APIC through MSRs. That MSR path, apart from reading the
  boot APIC ID, has run only under QEMU 9 TCG (debug build), switched on by
  the `x2apic` option (`make test-x2apic`), never on hardware; QEMU's
  firmware does not hand over in x2APIC mode on the one-CPU machines the
  tests boot, so the hand-off itself has not run either. A boot CPU whose
  APIC ID is above 255 stays on the 8259 (untested): routing to it through
  the I/O APIC would need interrupt remapping. See
  [interrupts.md](docs/design/interrupts.md).
- **The SYSRET hazard's real path runs only under QEMU 9.** On real CPUs a
  non-canonical RIP in `iretq` raises `#GP` in ring 0, the path the
  exception-table recovery handles. QEMU 8.2's TCG skips that check, so
  `make test` exercises the path with a different bad frame (an invalid
  SS); QEMU 9.2 checks it, so `make test-x2apic` takes the real one.

## Documentation

- [`docs/PLAN.md`](docs/PLAN.md): architecture, decisions, milestones, risks, and how the build differed from the plan
- [`docs/releases/`](docs/releases/): release notes, which are also the annotated tag messages
- [`docs/design/boot.md`](docs/design/boot.md): BIOS and UEFI boot paths, memory layout, debugging
- [`docs/design/traps.md`](docs/design/traps.md): GDT/TSS/IDT, exception policy, panic, sanitizers, legacy IRQs, locks
- [`docs/design/interrupts.md`](docs/design/interrupts.md): ACPI discovery, local APIC, I/O APIC, HPET, calibration
- [`docs/design/console.md`](docs/design/console.md): log ring, screen consoles, drawing without losing ticks
- [`docs/design/memory.md`](docs/design/memory.md): memory map, buddy allocator, page tables, heaps
- [`docs/design/scheduler.md`](docs/design/scheduler.md): context switch, preemption, lost-wakeup safety
- [`docs/design/usermode.md`](docs/design/usermode.md): processes, syscall entry/exit, SYSRET hazard, user memory access
- [`docs/design/input.md`](docs/design/input.md): the 8042 and COM1 receive, the input queue, `read()`, the shell

## Repository layout

```
boot/            stage1.asm (MBR), stage2.asm, layout.inc; efi/ (UEFI loader)
kernel/arch/     x86_64: entry, GDT/TSS/IDT, isr/syscall/switch asm, FPU, user copies, LAPIC
kernel/acpi/     table discovery and (pure) parsing
kernel/core/     kmain, panic, klog, irq, time, input, ksyms, UBSan, ktest, bench
kernel/mm/       memmap, buddy, pmm, vmm, vmalloc, kmalloc
kernel/sched/    scheduler, mutex
kernel/proc/     processes, ELF validation, syscalls
kernel/drivers/  serial, 8042, screen (framebuffer, VGA text), I/O APIC, HPET, PIT, PIC, QEMU exit
kernel/lib/      printf, strings, cmdline, scan code decoder, text grid, framebuffer drawing, font
kernel/include/  headers (uapi/ is shared with user space)
user/            crt0, libsys, line editor, programs/ (init, sh, and what they run)
tests/           host/ (Linux), kernel/ (KTEST), qemu/ (runner, its self-test, typing), gdb/
tools/           mkimage, mkesp, gen_ksyms, gen_user_images, check_kernel, gdb/
```

## License

MIT, see [`LICENSE`](LICENSE). The console font (`kernel/lib/font8x8.c`) is Daniel
Hepper's public-domain font8x8, after the IBM VGA fonts.
