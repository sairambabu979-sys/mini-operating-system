# Work after the October 2026 review

The user authorized the complete review backlog. This file tracks implementation
and evidence; an unchecked item is still work, including the larger expansions.

## Correctness and build foundation

- [ ] Bounded UART initialization/transmission; restore loopback on failure.
- [ ] Bound UART receive work including errored bytes; host register-model tests.
- [ ] Strict tar formats and whole-archive lookup validation; GNU/pax/checksum/root regressions.
- [ ] Archive-builder invalid-path, collision, and deterministic-output tests.
- [ ] Recover failed virtual allocations and partial mappings; OOM regressions.
- [ ] Track all compiler, assembler and linker flag changes in incremental builds.
- [ ] Native host tests and BIOS/UEFI development workflow on Apple Silicon.
- [ ] Bound BIOS serial and LAPIC IPI waits.

## Initrd and files

- [ ] Versioned boot module handoff, both loaders and image builders.
- [ ] Reserve initrd before metadata placement and physical allocation.
- [ ] Validate and mount a read-only filesystem with path/collision rules.
- [ ] Load programs from files and list programs from the mounted directory.
- [ ] Per-process descriptors and file/directory syscalls; user programs.
- [ ] Both firmware paths tested, including malformed/missing inputs and OOM.

## Diagnostics and validation

- [ ] IST guard pages, log timestamps, page-fault table walks, might_sleep checks.
- [ ] Architecture decision records and refreshed design/README documentation.
- [ ] Large (>1 MiB) BIOS image and long task/process churn.
- [ ] Log-level/console-option and optional CPU feature matrix.
- [ ] x2APIC release coverage and real IRET-fault recovery.
- [ ] Simulated HPET/PIT/APIC and UEFI failure paths from README's limitations.
- [ ] Verify visible panic output, not just serial completion.
- [ ] Buffered serial output and elapsed-time accounting independent of lost ticks.
- [ ] Physical-machine and KVM validation (requires suitable hardware).

## Expansion milestones

- [ ] Priority/MLFQ scheduling and latency benchmarks.
- [ ] SMP startup, per-CPU state/queues, contended locking, IPIs/TLB shootdowns.
- [ ] Copy-on-write fork and process image replacement.
- [ ] Relocation and KASLR.
- [ ] Persistent filesystem and block devices.
- [ ] Networking.
- [ ] Signals, pipes and IPC.
- [ ] PCI/MSI routing and USB input.
- [ ] ACPI power management.
- [ ] Foreground console ownership and interruptible reads.
- [ ] Shell quoting/editing/history and higher resource limits.

## Release

- [ ] Correct the stale v1.0.0 tagging checkbox.
- [ ] Create the missing v1.3.0 tag at the existing release commit.
- [ ] Run debug/release checks and report any unavailable external validation.
