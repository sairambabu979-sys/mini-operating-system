# mini-operating-system

A small x86_64 kernel written from scratch in C and NASM: its own two-stage
BIOS bootloader, 4-level paging, interrupt handling and a preemptive
scheduler. Every feature ships with a test that boots in CI.

```
$ make run
1234567
minios 0.1.0 (x86_64, debug build)
cmdline: ""
kernel: phys [0x100000-0x10a000), virt 0xffffffff80100000
memory map (7 entries, BIOS order):
  [0x0000000000000000-0x000000000009fbff] usable
  [0x0000000000100000-0x0000000007fdffff] usable
  ...
```

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
| M4 Virtual memory: own page tables, W^X, kmalloc | planned |
| M5 Interrupts and time: PIC, PIT | planned |
| M6 Scheduler: preemptive kernel threads | planned |
| M7 User mode: ring 3, syscalls, process isolation | planned |

## Testing

| Layer | Runs | Where |
|-------|------|-------|
| Host unit tests | Kernel library code under ASan + UBSan; printf differentially tested against glibc | `tests/host/` |
| Binary checks | No SSE/AVX in kernel code; W^X segment layout; entry point | `tools/check_kernel.py` |
| In-kernel tests | `KTEST()` cases, TAP over serial, judged by exit code and TAP count | `tests/kernel/`, `tests/qemu/run_tests.py` |
| Crash path | Deliberate kernel page fault; panic decode and symbolized backtrace matched against patterns | `disk-panic.img` |

## Documentation

- [`docs/PLAN.md`](docs/PLAN.md): architecture, decisions, milestones, risks
- [`docs/design/boot.md`](docs/design/boot.md): boot sequence, memory layout, debugging
- [`docs/design/memory.md`](docs/design/memory.md): memory map, buddy allocator, PMM
- [`docs/design/traps.md`](docs/design/traps.md): GDT/TSS/IDT, exception policy, panic, sanitizers
