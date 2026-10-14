# Interrupt controllers and time

How hardware interrupts reach a handler, and where the tick comes from.
Sources: `kernel/acpi/`, `kernel/arch/x86_64/lapic.c`,
`kernel/drivers/{ioapic,hpet,pit,pic8259}.c`, `kernel/core/{irq,time}.c`.

## Two paths, one interface

| | APIC path (default) | Legacy path (`noapic`) |
|---|---|---|
| ISA IRQs | I/O APIC, routed per the MADT | 8259 pair |
| Tick | local APIC timer, periodic | PIT channel 0, IRQ 0 (no PIT: panic) |
| Calibrated against | HPET main counter, else PIT channel 2 | the same |
| Chosen when | CPU has a local APIC, its APIC ID is at most 255, the MADT lists an I/O APIC that answers, no `noapic` | anything else |

Drivers see neither: `irq_register(line, ...)` names an ISA line, and the
interrupt arrives on vector `32 + line` either way. The legacy path is not
dead code. It is the fallback for anything missing at boot, and
`make test` boots it every time through the `noapic` option
(`disk-ktest-noapic.img`); the automatic fallbacks have not run.

Why bother, when the 8259 + PIT worked: some recent platforms gate off the
8254 PIT, and the 8259 cannot be used on SMP. The local APIC timer is
also the tick every future CPU will need. The APIC path runs without any
PIT when there is an HPET to calibrate against (`make test` boots it with
`-machine pc,pit=off`). The 8259 path cannot: it panics with `no PIT: the
8259 path ("noapic" or no usable I/O APIC) has no other tick` instead of
hanging (no test run boots `noapic` without a PIT). `pit_present()`
programs a 1 ms channel 2 one-shot and checks that its output went low
(without a PIT, port `0x61` floats and reads high), then that it rises
within a million polls: a PIT that answers but does not count (clock
gating) would otherwise be the tick and hang the first wait. QEMU cannot
model a gated PIT, so that half has not run.

An APIC ID above 255 also selects the 8259: the I/O APIC's destination
field is 8 bits, and reaching a larger ID would need interrupt remapping.

## ACPI discovery (`kernel/acpi/`)

1. **RSDP.** The UEFI loader copies it from the EFI configuration table
   (ACPI 2.0 GUID preferred, 1.0 accepted) into `boot_info.acpi_rsdp`
   (`boot_info` version 3). Under the BIOS the field is 0 and the kernel
   scans the first KiB of the EBDA and then `0xE0000–0xFFFFF` on 16-byte
   boundaries, as the specification requires.
2. **Root table.** XSDT when the RSDP is revision 2+ and has one, else
   the RSDT. Each entry is mapped, its checksum verified, and its
   signature recorded. Both paths run in the suite: SeaBIOS hands over
   an ACPI 1.0 RSDP (RSDT), OVMF a 2.0 one (XSDT).
3. **MADT and HPET** are parsed into plain structures that the drivers
   use. Nothing reads a firmware table after `acpi_init()`.

Tables are mapped with `iomap()` (read-only, cached, behind a guard page):
firmware may put them in "reserved" memory the direct map leaves out.

**Parsing is pure and fuzzed.** `kernel/acpi/tables.c` takes a buffer and
a length and reads nothing outside them. The host suite builds tables
byte by byte: QEMU's real layout, x2APIC and 64-bit address entries,
conflicting overrides, reserved flag encodings, truncations, and 20,000
randomly corrupted MADTs, each parsed at a random length from an
exactly-sized heap copy under ASan. Two mutations were checked by hand:
dropping the "entry fits in the table" bound is an ASan heap overflow,
and dropping the "length at least 2" check hangs the fuzzer on a
zero-length entry.

**Interrupt source overrides.** ISA IRQs start identity-mapped,
edge-triggered and active high. An override moves one (QEMU, like most
PCs, sends IRQ 0 to GSI 2, and makes the SCI and PCI-routed lines
level-triggered). An ISA line whose identity GSI another override took
(IRQ 2, the old cascade) is marked unwired, and `irq_register` returns
`-ENODEV` for it. Duplicate sources, two sources on one GSI, non-ISA
buses and reserved polarity or trigger encodings are counted and
ignored, never guessed at.

## Local APIC (`arch/x86_64/lapic.c`)

- **The mode the firmware left.** In xAPIC mode the registers are the
  MMIO page at the base from `IA32_APIC_BASE` (the MADT's value is compared
  and a mismatch logged), mapped uncached with `iomap()`. In x2APIC mode
  they are MSRs (`0x800` + offset / 16).
- **x2APIC is never reverted.** The SDM allows going back to xAPIC through
  the disabled state, but firmware may lock x2APIC on (Intel parts with
  SGX, after the AEPIC Leak mitigation), and then that transition raises
  `#GP`. The register layout is the same in both modes apart from three
  differences, handled in `lapic.c`: the ID is a full 32 bits, the ICR is
  one 64-bit MSR, and there is no delivery-status bit to poll.
- **The `x2apic` option** switches the local APIC to x2APIC mode when
  CPUID reports x2APIC (without it, the option does nothing). It enables
  xAPIC first, because going from disabled straight to x2APIC raises
  `#GP`. The boot log names the mode and the reason: `x2APIC mode (left on
  by the firmware)` or `x2APIC mode (switched on by the x2apic option)`.
  Neither SeaBIOS nor OVMF hands over in x2APIC mode on the one-CPU QEMU
  machines the suite boots, so the option is how the suite runs the MSR
  path (see [Testing](#testing)). From the switch on, the driver runs as
  it would after a firmware hand-off. Only `lapic_boot_id()`'s MSR read
  never runs in a test, because it runs before the switch.
- Every LVT is masked before the APIC is re-enabled. **Stale in-service
  bits** left by the firmware are drained with EOIs: one would silently
  block its priority class and everything below it.
- LINT0 (the 8259's ExtINT) stays masked. LINT1, or whatever the MADT's
  NMI entries say, delivers NMIs.
- Vectors: timer `0xF0`, test IPI `0xFD`, error `0xFE`, spurious `0xFF`.
  These are the top priority class. A spurious interrupt gets no EOI. An
  error interrupt logs the latched ESR.
- An NMI is only counted where it lands (see [traps.md](traps.md)): the
  report is printed after the EOI of the next handled interrupt, local or
  ISA, when no lock can be held.

## I/O APIC (`drivers/ioapic.c`)

Every I/O APIC the MADT lists is mapped, its version register read (an
all-ones read means nothing is there, so it is skipped), and every pin
masked. One whose GSI range overlaps another is skipped. Lines are
routed lazily, when a driver registers: fixed delivery, physical
destination (this CPU), polarity and trigger from the override. A
redirection entry is written high half first, with the low half masked
until last, so it is never live half-written.

The index/data register pair makes every access two steps, so one lock
covers all I/O APICs.

## Calibration

Both rates are measured once at boot, with interrupts off, over a 50 ms
window of the reference clock:

- **Reference: the HPET**, when the ACPI table names one that passes its
  checks (period nonzero and at most 100 ns, as the specification
  requires; 32-bit counters wrap-handled). Its comparators are disabled:
  it is only a clock. `nohpet` skips it (no test image boots with it: the
  no-HPET run removes the HPET from the machine instead).
- **Otherwise PIT channel 2**, a polled one-shot gated through port
  `0x61`. It needs no interrupt and leaves channel 0 alone.

Each endpoint is a reading of the reference **bracketed by two TSC
reads**. The bracket is the endpoint's uncertainty. A window whose two
brackets add up to more than 0.1% of its length is retried, up to five
times. The tightest window wins, and a warning is logged if none
qualifies. Without this, one preemption at an endpoint (the host
descheduling the vCPU under emulation, an SMI on hardware) skews the TSC
rate for the whole run. In one stress run (48 boots, 8 at a time) the
fallback was needed once, at 0.39%.

The local APIC timer counts down from `0xFFFFFFFF` (divide-by-16,
masked) across the same window. The tick is the nearest whole count to
1 ms, and `time_ns_per_tick()` is what that count measures, so rounding
is the only error (under QEMU: 62.5 MHz, a tick of 1,000,004 ns).

**A broken clock must not hang the boot.** Calibration runs early, with
interrupts off, so an unbounded wait would hang the boot with no message
saying why. Every window therefore has a timeout and a sanity check:
- Each wait gives up after 2^32 TSC cycles (about 1 s at 4 GHz, 4 s at
  1 GHz). That catches an HPET counter that is stopped, or so slow that
  its 50 ms take longer, or a PIT channel 2 that never runs out
  (clock-gated).
- PIT channel 2 in mode 0 drives its output low as soon as it is
  programmed. An output already high at the first poll means no counting
  PIT (a missing one reads as `0xFF`), or a stall longer than the window.
- The resulting TSC rate must lie between 100 MHz and 20 GHz.

A window with an implausible rate is discarded and counts as one of the
five tries. A clock that times out, or a PIT whose output is already high,
is not tried again: each timeout is 2^32 cycles with interrupts off and
nothing printed, and five in a row looked like the hang the timeout
exists to prevent. Among the good windows, the retry rule above applies.
If the HPET yields no good window, the kernel logs `the HPET did not
time a 50 ms window; calibrating against the PIT` and measures again on
PIT channel 2. If that fails too, or the HPET was never usable (none
listed, `nohpet`, or rejected by its checks), it panics:
`no working reference clock: the PIT (no HPET) did not time a 50 ms
window, so the tick and the TSC cannot be calibrated`. The clause names
what was tried: `the HPET and PIT channel 2 both` after a failed HPET,
`the PIT (HPET not used)` when ACPI lists an HPET that `nohpet` or its
checks ruled out.

The `pit=off,hpet=off` run boots into that panic (see
[Testing](#testing)). No run has an HPET that is listed but broken, so
the fallback from the HPET to the PIT has never run. No run has a clock
that stops or crawls, or an implausible TSC rate, either.

## Testing

| Run | Path exercised | Skips |
|-----|----------------|------:|
| `disk-ktest.img` (BIOS), 128 MiB and 4 GiB | APIC path, HPET reference, RSDP found by scanning, RSDT | 1 |
| `disk-ktest.img` with `-machine pc,hpet=off` | APIC path, PIT channel 2 reference | 3 |
| `disk-ktest.img` with `-machine pc,pit=off` | APIC path, HPET reference, no PIT at all | 4 |
| `disk-ktest.img` with `-machine pc,pit=off,hpet=off` | no reference clock: must panic with `no working reference clock: the PIT (no HPET) did not time a 50 ms window` | – |
| `disk-ktest-noapic.img` | 8259 + PIT tick, HPET reference | 9 |
| `disk-uefi-ktest.img` | APIC path, RSDP from the EFI configuration table, XSDT | 1 |
| `disk-ktest-x2apic.img` (`make test-x2apic`) | APIC path in x2APIC mode, HPET reference | 1 |

The in-kernel tests check each configuration against its command line
and platform. Absent ACPI tables are failures, not skips: under QEMU they
always exist, and a broken RSDP search must not turn the APIC tests into
no-ops. Tests that do not apply to a configuration report TAP `# SKIP`:
- everywhere on the APIC path, the PIT tick-programming test;
- without an HPET, also the two HPET tests;
- without a PIT, also the two independent-measurement tests and the IRQ 0
  routing test;
- on the 8259 path, the nine tests that need the local APIC or the I/O
  APIC: eight in `test_apic.c`, and the scheduler's IRQ-wakeup test, which
  sends a self-IPI. The PIT tick test runs there.

Each ktest run passes `run_tests.py` its exact skip count (`--skips N`)
and the reference clock the boot log must name: `--expect 'calibrated
against the HPET'`, or `'calibrated against the PIT'` for `hpet=off`. A
test that starts skipping because discovery broke, or a boot that quietly
falls back from the HPET to the PIT, fails its run. The
`pit=off,hpet=off` run passes `--expect-panic` and the panic message
instead.

**x2APIC.** `make test-x2apic QEMU=<a QEMU 9.x qemu-system-x86_64>` boots
`disk-ktest-x2apic.img` (command line `ktest x2apic`): the whole suite,
with the local APIC in x2APIC mode. It is not part of `make test`. QEMU
8.2, the version Ubuntu 24.04 ships, cannot emulate x2APIC under TCG;
9.0 and later can. `run_tests.py` takes the emulator from `$QEMU`. The
run also expects `x2APIC mode (switched on by the x2apic option)` in the
log. `lapic_mode_matches_the_command_line` requires x2APIC mode exactly
when the command line asks for it, so an emulator without x2APIC fails
the run instead of quietly testing xAPIC. CI's `x2apic` job builds QEMU
9.2.0 from its git tag, caches it, and runs this target (debug build).

What they prove:
- the controller and reference clock are the ones the configuration
  calls for, and so is the local APIC mode;
- the local APIC registers are exactly as programmed (SVR, TPR, LINT0
  masked, LINT1 NMI, error vector, ESR clear), and this CPU's APIC ID is
  one the MADT lists;
- the LAPIC timer is periodic on its vector with exactly the calibrated
  count;
- the PIT, registered as ISA IRQ 0, really arrives through GSI 2 (the
  override), with the redirection entry read back field by field.
  Masking stops it, and IRQ 2 is refused as unwired;
- a self-IPI sent with interrupts off stays pending, is delivered exactly
  once after `sti`, and leaves nothing in service;
- the HPET and the TSC agree within 2%, measured with bracketed reads;
- the TSC rate, and on the APIC path the LAPIC timer rate, agree with the
  test's own PIT measurement (below);
- the MADT's trigger and polarity reach the redirection entries. IRQ 0's
  entry is edge-triggered, active high, but those are also the defaults,
  so it cannot show that the override's flags are applied. The SCI (ISA
  IRQ 9) can: its override makes it level-triggered, active high, and its
  entry must say so. QEMU's MADT has no active-low ISA line; the host
  suite checks that the parser reads one;
- an NMI sent to this CPU with interrupts off is counted at once, prints
  nothing in NMI context, and is reported once interrupts are back on;
- `iomap()` maps device memory strong-uncached and NX behind a guard
  page, and refuses an uncached alias of RAM (SDM 12.12.4: mixed memory
  types are undefined).

**Checks that count timer interrupts are deliberately loose; register
checks are exact.** A stress run (24 boots, 8 at a time) failed two tests
that asserted emulator timing rather than kernel behavior:
- "100 ticks within ±30% of 100 ms of HPET time": QEMU merges LAPIC
  ticks that fall due while the vCPU is descheduled;
- "10 PIT interrupts in 30 ms of TSC time": the vCPU may not run at all
  during those 30 ms.

Checks that count timer interrupts now only catch factor-sized errors,
such as a wrong divider or a unit mix-up, and waits use deadlines. After
the changes, 72 boots across the three paths, 10 at a time, passed.

**Checks that compare two clocks stay tight.** Each reading is bracketed
by TSC reads, so no interrupt has to arrive on time, and a stop at an
endpoint widens a bracket instead of skewing the result.

Every rate check, loose or tight, and its bound:

| Test | Runs | Asserts |
|------|------|---------|
| `pit_is_programmed_for_the_tick_rate` (`test_irq.c`) | 8259 path | divisor exactly 1193; the counter seen covering more than half its range; over a 100-tick wait, 0.5–4 ms of TSC time per tick (2x fast to 4x slow) |
| `tick_rate_agrees_with_the_hpet` | HPET reference, either path | 100 ticks take 94.05–400 ms of HPET time (99 periods less 5%, up to 4x slow) |
| `lapic_timer_is_periodic_at_one_millisecond` | APIC path | initial count exactly the calibrated one; the period it gives strictly between 999,000 and 1,001,000 ns |
| `hpet_counter_runs_at_its_period` | HPET reference | HPET and TSC within 2% over 20 ms, each HPET read pinned between two TSC reads at most 20 µs apart |
| `tsc_rate_matches_an_independent_measurement` | a PIT present | the TSC rate measured on the PIT within 2% of the calibrated one, plus the measurement's endpoint uncertainty |
| `lapic_timer_rate_matches_an_independent_measurement` | APIC path, a PIT present | LAPIC timer counts over 40 ms of TSC time, converted with the TSC rate from the test's PIT window: within 2% of the calibrated rate, plus the endpoint uncertainty of both measurements |

All but the first are in `test_apic.c`. The scheduler's timing tests
(round-robin CPU time within one slice, sleep lengths counted in ticks)
are in [scheduler.md](scheduler.md).

**The independent PIT measurement.** The two `..._independent_measurement`
tests share no code with `time.c`. Each times a 40 ms one-shot on PIT
channel 2 (count 47,727) in TSC cycles, with interrupts off, and keeps
the tightest of up to eight windows, stopping early at one whose
endpoints are pinned to 0.1%. In the default boot that compares two
clocks, since the kernel calibrated against the HPET; with `hpet=off`,
two implementations on one. An injected 2x LAPIC-rate error passed every
other test, and so did a 2x TSC error without an HPET (with one,
`hpet_counter_runs_at_its_period` also catches a TSC error).

The start of the window is bracketed by TSC reads around the
programming. The end, the output's rising edge, lies between the last
poll that saw the output low and the first that saw it high. The test
bounds it with a TSC read taken *before* the former and one taken
*after* the latter, so a vCPU stop anywhere in that span widens the
bracket and the tolerance with it. The first version read the TSC
*after* the last low poll: a stop between that poll and the read could
put the read past the edge, and nothing measured it. That, and LAPIC
timer reads with no bracket at all, failed 4 of 16 runs under 8-way
parallel load. With the edge bracketed this way, the LAPIC timer read in
pinned pairs, and the tolerance widened by the measured uncertainty, 48
of 48 passed. `time.c` brackets its own PIT edge the same way.

## Limits

- **ISA IRQs only.** No PCI interrupt routing (it needs the AML
  interpreter for `_PRT`) and no MSI.
- **No ACPI namespace.** Only static tables are read: no AML, so no
  power-off, sleep states or thermal management.
- **The LAPIC timer is assumed to run in `hlt`.** True on CPUs with ARAT
  and in C1, which is all `hlt` requests. Some older AMD parts stop it in
  C1E; the kernel does not detect that.
- **One CPU.** The routing targets the boot CPU's APIC ID. Nothing here
  assumes one CPU beyond that.
