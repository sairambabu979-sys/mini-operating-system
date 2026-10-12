# Interrupt controllers and time

How hardware interrupts reach a handler, and where the tick comes from.
Sources: `kernel/acpi/`, `kernel/arch/x86_64/lapic.c`,
`kernel/drivers/{ioapic,hpet,pit,pic8259}.c`, `kernel/core/{irq,time}.c`.

## Two paths, one interface

| | APIC path (default) | Legacy path (`noapic`) |
|---|---|---|
| ISA IRQs | I/O APIC, routed per the MADT | 8259 pair |
| Tick | local APIC timer, periodic | PIT channel 0, IRQ 0 |
| Calibrated against | HPET main counter, else PIT channel 2 | the same |
| Chosen when | CPU has a local APIC, the MADT lists an I/O APIC that answers, no `noapic` | anything else |

Drivers see neither: `irq_register(line, ...)` names an ISA line, and the
interrupt arrives on vector `32 + line` either way. The legacy path is not
dead code. It is the fallback for anything missing at boot, and
`make test` boots it every time (`disk-ktest-noapic.img`).

Why bother, when the 8259 + PIT worked: some recent platforms gate off the
8254 PIT, and the 8259 cannot be used on SMP. The local APIC timer is
also the tick every future CPU will need.

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

- Base from `IA32_APIC_BASE` (the MADT's value is compared and a mismatch
  logged). Mapped uncached with `iomap()`.
- **xAPIC only.** If the firmware handed over in x2APIC mode, the driver
  goes back through the disabled state, the transition the SDM allows.
  For one CPU with an 8-bit APIC ID that loses nothing, and it keeps a
  single register path, the one the tests run. (QEMU's TCG cannot
  emulate x2APIC, so that transition is the one untested line here.)
- Every LVT is masked before the APIC is re-enabled. **Stale in-service
  bits** left by the firmware are drained with EOIs: one would silently
  block its priority class and everything below it.
- LINT0 (the 8259's ExtINT) stays masked. LINT1, or whatever the MADT's
  NMI entries say, delivers NMIs.
- Vectors: timer `0xF0`, test IPI `0xFD`, error `0xFE`, spurious `0xFF`.
  These are the top priority class. A spurious interrupt gets no EOI. An
  error interrupt logs the latched ESR.

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
  it is only a clock. `nohpet` skips it.
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

## Testing

| Run | Path exercised |
|-----|----------------|
| `disk-ktest.img` (BIOS) | APIC path, HPET reference, RSDP found by scanning, RSDT |
| `disk-ktest.img` with `-machine pc,hpet=off` | APIC path, PIT channel 2 reference |
| `disk-ktest-noapic.img` | 8259 + PIT tick, HPET reference |
| `disk-uefi-ktest.img` | APIC path, RSDP from the EFI configuration table, XSDT |

The in-kernel tests check each configuration against its command line
and platform. Absent ACPI tables are failures, not skips: under QEMU they
always exist, and a broken RSDP search must not turn the APIC tests into
no-ops. Tests that do not apply to a configuration report TAP `# SKIP`.

What they prove:
- the controller and reference clock are the ones the configuration
  calls for;
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
- `iomap()` maps device memory strong-uncached and NX behind a guard
  page, and refuses an uncached alias of RAM (SDM 12.12.4: mixed memory
  types are undefined).

**Rate checks are deliberately loose; register checks are exact.** A
stress run (24 boots, 8 at a time) failed two tests that asserted
emulator timing rather than kernel behavior:
- "100 ticks within ±30% of 100 ms of HPET time": QEMU merges LAPIC
  ticks that fall due while the vCPU is descheduled;
- "10 PIT interrupts in 30 ms of TSC time": the vCPU may not run at all
  during those 30 ms.

Rate checks now only catch factor-sized errors, such as a wrong divider
or a unit mix-up, and waits use deadlines. After the changes, 72 boots
across the three paths, 10 at a time, passed.

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
