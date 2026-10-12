# mini-operating-system

A small x86_64 kernel written from scratch in C17 and NASM. It boots from
its own two-stage BIOS loader, runs on its own 4-level page tables with
W^X enforced everywhere, handles every CPU exception and hardware
interrupt, preemptively schedules kernel threads and ring-3 processes,
and isolates those processes from the kernel and from each other.

Every feature ships with a test that boots in CI.

```
$ make run
1234567
cpu: AuthenticAMD, 40-bit physical / 57-bit virtual; nx=1 pge=1 1g=1 smep=1 smap=1
vmm: direct map: 127 MiB at 0xffff800000000000
pmm: 125 MiB managed, 768 KiB metadata at 0x18c000, highest frame 0x7fdf000
time: PIT at 1000 Hz (999847 ns/tick), TSC 2097.859 MHz
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

Requirements: `clang`, `lld`, `llvm`, `nasm`, `gcc`, `make`, `python3`, `qemu-system-x86`.
On Ubuntu 24.04: `apt install clang lld llvm nasm gcc make python3 qemu-system-x86`.

```sh
make            # bootloader, kernel, user programs, disk images  -> build/debug/
make run        # boot in QEMU, console on stdio (Ctrl-C to quit)
make test       # everything below, as CI runs it
```

| Option | Effect |
|--------|--------|
| `CONFIG=release` | optimized build, no kernel UBSan |
| `V=1` | show full commands |
| `CMDLINE="..."` | kernel options for `make run`, e.g. `loglevel=debug` |
| `make bench` | microbenchmarks |
| `make debug` | QEMU waits for gdb on `:1234` (`gdb -x tools/gdb/gdbinit build/debug/kernel.elf`) |
| `make lint` / `make format` | clang-tidy, clang-format |

## What is in it

| Area | Highlights |
|------|------------|
| **Boot** | Own MBR + stage 2. A20 by three methods, each verified. E820 map. Unreal-mode kernel load. Real mode straight to long mode. ELF64 loading with full bounds checks. A versioned `boot_info` ABI defined once in C, with NASM offsets generated from it. |
| **Traps** | GDT/TSS with IST stacks for `#DF`, NMI and `#MC`. 256 generated entry stubs, one trap-frame layout. Decoded panics with symbolized backtraces (two-pass symbol-table link). Exception-table fixups. |
| **Memory** | E820 sanitizer. Buddy allocator (pure, fuzzed on the host). Own page tables bootstrapped without an allocator. Direct map of RAM only: non-executable, with the kernel image read-only through it. NX, WP, SMEP, SMAP. Guard-paged `vmalloc`. Slab `kmalloc` with use-after-free and double-free detection. |
| **Interrupts** | 8259 PIC with spurious-IRQ handling, PIT at 1 kHz, TSC calibration. IRQ-safe spinlocks that report recursive locking instead of deadlocking. |
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
 │   arch/    GDT/TSS, IDT + isr.asm, syscall.asm, FPU, per-CPU          │
 │   drivers/ serial (16550), PIC (8259), QEMU debug exit                │
 ├───────────────────────────────────────────────────────────────────────┤
 │  boot/   stage1 (MBR) → stage2: A20, E820, unreal mode, long mode, ELF │
 └───────────────────────────────────────────────────────────────────────┘
```

Virtual memory:

| Range | Contents |
|-------|----------|
| `0x0000000000010000 – 0x00007FFFFFFFFFFF` | user space: program, stack at `0x7FFFFFFF0000` |
| `0xFFFF800000000000` + 512 GiB | direct map of physical RAM (no MMIO holes) |
| `0xFFFFC00000000000` + 512 GiB | vmalloc: guard-paged kernel stacks |
| `0xFFFFFFFF80000000` – top | kernel image (`.text` R-X, `.rodata` R--, `.data/.bss` RW-) |

## Testing

`make test` runs all of these; CI runs them for both debug and release.

| Layer | What runs | Where |
|-------|-----------|-------|
| Host unit tests | Kernel code built for Linux under ASan + UBSan. printf against glibc (20k random formats); memory-map sanitizer against brute force; buddy allocator fuzz with full verification; ELF validator against hostile and randomly corrupted files. | `tests/host/` |
| Binary checks | No SSE/AVX in kernel code; W^X segment layout; entry point. Build-time guard for `-mno-red-zone`, `-mgeneral-regs-only`, `-mcmodel=kernel`. | `tools/check_kernel.py`, `Makefile` |
| In-kernel tests | 79 `KTEST()`s, TAP over serial, booted with **128 MiB and 4 GiB** of RAM. Every protection is checked by attempting the forbidden access. | `tests/kernel/` |
| Crash path | A deliberate kernel page fault; the panic decode and backtrace are matched against patterns. | `disk-panic.img` |
| Whole system | Boots the real `init`, which runs 11 programs and passes only if each ends as expected. | `disk-demo.img` |
| Benchmarks | Smoke run of `make bench`. | `disk-bench.img` |

Some of what the in-kernel tests prove:
- a real double fault, and a real kernel stack overflow, each caught on
  its IST stack;
- four never-yielding threads all preempted, and fair to within 25%;
- a 3000-item producer/consumer with no lost wakeup;
- processes that dereference NULL, run `cli`, read kernel memory or patch
  their own code are killed, and only they are;
- hostile syscall arguments all fail with the right errno;
- the SYSRET hazard is avoided, and a faulting `iretq` kills only the task;
- FPU state never leaks between processes;
- memory is fully reclaimed after hundreds of threads and processes.

The QEMU runner fails on timeout, triple fault, kernel-reported failure or
a TAP count mismatch. It was itself checked by injecting a failing test
and a triple fault. Timing-sensitive tests were run repeatedly with 6 QEMU
instances competing for 4 host cores.

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
- **Legacy timer and interrupts.** PIT and 8259 PIC; no LAPIC, IOAPIC or ACPI.
- **No filesystem.** User programs are embedded in the kernel image and
  started by name; there is `spawn`, but no `fork`/`exec`.
- **No signals, pipes or IPC.** Processes talk only through exit statuses.
- **Console only:** serial output, no input driver.
- **BIOS only;** no UEFI loader. The `boot_info` ABI is loader-agnostic.
- **Only partly tested under TCG.** On real CPUs a non-canonical RIP
  in `iretq` raises `#GP` in ring 0, the path the exception-table
  recovery handles. TCG skips that check, so that exact path is exercised
  with a different bad frame (an invalid SS).

## Documentation

- [`docs/PLAN.md`](docs/PLAN.md): architecture, decisions, milestones, risks, and how the build differed from the plan
- [`docs/design/boot.md`](docs/design/boot.md): boot sequence, memory layout, debugging
- [`docs/design/traps.md`](docs/design/traps.md): GDT/TSS/IDT, exception policy, panic, sanitizers, IRQs, locks
- [`docs/design/memory.md`](docs/design/memory.md): memory map, buddy allocator, page tables, heaps
- [`docs/design/scheduler.md`](docs/design/scheduler.md): context switch, preemption, lost-wakeup safety
- [`docs/design/usermode.md`](docs/design/usermode.md): processes, syscall entry/exit, SYSRET hazard, user memory access

## Repository layout

```
boot/            stage1.asm (MBR), stage2.asm, layout.inc
kernel/arch/     x86_64: entry, GDT/TSS/IDT, isr/syscall/switch asm, FPU, user copies
kernel/core/     kmain, panic, klog, irq, time, ksyms, UBSan, ktest, bench
kernel/mm/       memmap, buddy, pmm, vmm, vmalloc, kmalloc
kernel/sched/    scheduler, mutex
kernel/proc/     processes, ELF validation, syscalls
kernel/drivers/  serial, PIC, QEMU exit
kernel/include/  headers (uapi/ is shared with user space)
user/            crt0, libsys, programs/
tests/           host/ (Linux), kernel/ (KTEST), qemu/run_tests.py
tools/           mkimage, gen_ksyms, gen_user_images, check_kernel, gdb/
```

## License

MIT, see [`LICENSE`](LICENSE).
