# mini-operating-system

A small x86_64 kernel written from scratch in C and NASM: its own two-stage
BIOS bootloader, 4-level paging, interrupt handling and a preemptive
scheduler. Every feature ships with a test that boots in CI.

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

`1234567` is the bootloader's progress trail (MBR, stage 2, A20, memory map,
kernel loaded, long mode, ELF loaded). Everything after it is the kernel and
then ring-3 programs, some of them deliberately hostile.

## Quick start

Requirements: `clang`, `lld`, `llvm`, `nasm`, `gcc`, `make`, `python3`, `qemu-system-x86`
(Ubuntu 24.04: `apt install clang lld llvm nasm gcc make python3 qemu-system-x86`).

```sh
make            # bootloader, kernel, disk images  -> build/debug/
make run        # boot in QEMU, console on stdio (Ctrl-A X to quit)
make test       # host unit tests + binary checks + in-kernel tests under QEMU
```

`CONFIG=release` selects the optimized build. `V=1` shows full commands.
`CMDLINE="..."` passes kernel options to `make run`.

## Status

| Milestone | State |
|-----------|-------|
| M0 Scaffolding: build, CI, host tests | ✅ |
| M1 Bootloader: MBR → long mode → ELF load | ✅ |
| M2 Kernel foundation: GDT/TSS/IDT, exceptions, panic, UBSan | ✅ |
| M3 Physical memory: E820 sanitizer, buddy allocator | ✅ |
| M4 Virtual memory: own page tables, direct map, W^X, vmalloc, kmalloc | ✅ |
| M5 Interrupts and time: PIC, PIT, TSC calibration, IRQ-safe locks | ✅ |
| M6 Scheduler: preemptive round robin, wait queues, sleep, mutexes | ✅ |
| M7 User mode: ring 3, syscall/sysret, ELF loader, isolation, FPU state | ✅ |

## Testing

| Layer | Runs | Where |
|-------|------|-------|
| Host unit tests | Kernel library code under ASan + UBSan; printf differentially tested against glibc | `tests/host/` |
| Binary checks | No SSE/AVX in kernel code; W^X segment layout; entry point | `tools/check_kernel.py` |
| In-kernel tests | `KTEST()` cases, TAP over serial, judged by exit code and TAP count; run at 128 MiB and 4 GiB | `tests/kernel/`, `tests/qemu/run_tests.py` |
| Crash path | Deliberate kernel page fault; panic decode and symbolized backtrace matched against patterns | `disk-panic.img` |
| Whole system | Boots the real `init` and its 11 programs; passes only if every one ends as expected | `disk-demo.img` |

## Documentation

- [`docs/PLAN.md`](docs/PLAN.md): architecture, decisions, milestones, risks
- [`docs/design/boot.md`](docs/design/boot.md): boot sequence, memory layout, debugging
- [`docs/design/memory.md`](docs/design/memory.md): memory map, buddy allocator, page tables, heaps
- [`docs/design/traps.md`](docs/design/traps.md): GDT/TSS/IDT, exception policy, panic, sanitizers, IRQs, locks
- [`docs/design/scheduler.md`](docs/design/scheduler.md): context switch, preemption, lost-wakeup safety
- [`docs/design/usermode.md`](docs/design/usermode.md): processes, syscall entry/exit, SYSRET hazard, user memory access
