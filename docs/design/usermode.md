# User mode and system calls

Sources: `kernel/proc/`, `kernel/arch/x86_64/{syscall.asm,isr.asm,usercopy.c,fpu.c,percpu.c}`,
`kernel/include/uapi/syscall.h`, `user/`.

## Processes

A process is a task with its own address space and FPU state, started
from one of the ELF programs embedded in the kernel image
(`tools/gen_user_images.py`). There is no filesystem; `spawn(name)`
creates a process from an image, as `posix_spawn` would, rather than via
fork/exec.

```
0x0000000000010000 -              program segments (NULL page and below: unmapped)
0x00007FFFFFFE0000 - ...FFF0000   user stack, 64 KiB, unmapped guard below
0x00007FFFFFFF0000 - USER_TOP     free (the sysret test puts its code here)
0xFFFF800000000000 -              kernel half: shared, supervisor-only
```

**Loading.** `elf_parse()` is pure, validating code, host-tested against
hand-built hostile files and 20,000 random corruptions under ASan. It
rejects:
- the wrong class, machine or type, and dynamic linking,
- truncated or overflowing headers and segments,
- segments outside `[USER_MIN, USER_TOP)`, W+X segments, and segments
  sharing a page,
- an entry point outside executable memory.

The loader copies each segment into fresh frames through the direct map,
then maps them with the segment's own permissions. The kernel never writes
through user mappings, so SMAP stays on during loading and read-only user
text really is read-only.

**Address spaces.** A new PML4 copies the kernel half (entries 256–511,
frozen at boot) and starts with an empty user half. Tearing one down frees
every user frame and every user page table. That happens when the process
exits, not when it is reaped: `task_exit()` detaches the space, switches
the task onto the kernel's page tables, and only then frees it, so the
space is never active while it is destroyed. The FPU area goes at the same
time. A zombie keeps only its task struct and kernel stack, so a parent
that never calls `wait` does not pin its children's memory.

## Entering and leaving ring 3

All ways in and out share one frame layout, `struct trap_frame`:

| Path | In | Out |
|------|----|-----|
| Interrupt or exception from ring 3 | CPU pushes the frame, the stub saves registers, `swapgs` | `trap_return`: `swapgs`, `iretq` |
| `syscall` | `syscall_entry` builds the same frame by hand | `sysret` when safe, else `trap_return` |
| First run of a process | `process_start` builds a frame | `enter_user` → `trap_return` |

Two hand-off points tell the CPU where the kernel stack is:
- `TSS.RSP0`, for interrupts arriving from ring 3,
- `percpu.kernel_rsp`, read through GS by the syscall entry, which runs
  with no usable stack.

Both are updated on every context switch.

### The syscall entry

`SYSCALL` leaves RSP pointing at the user stack and interrupts disabled.
The entry does `swapgs`, saves the user RSP in per-CPU scratch space, loads
the kernel stack, builds the frame, and only then re-enables interrupts.
Syscalls are preemptible.

**`FMASK` clears every flag ring 3 can set**: CF, PF, AF, ZF, SF, TF,
IF, DF, OF, IOPL, NT, RF, AC and ID. The way out restores the user's own
flags from the frame (`sysret` through R11, or `iretq`). The ones that
matter:
- IF: off until the stack switch;
- DF and TF: the kernel assumes DF clear, and must not single-step;
- AC: set in ring 0 it switches SMAP off;
- NT: the kernel does not save RFLAGS across context switches, so an NT
  flag the user set would reach the next `iretq` of whatever task ran next,
  and `iretq` with NT set raises `#GP` in 64-bit mode. Until `FMASK`
  cleared it, a process could kill other processes this way, or panic the
  kernel through a kernel thread's `iretq`.

Interrupts from ring 3 need the same care for AC, which interrupt
delivery does not clear: on CPUs with SMAP, `trap_dispatch()` clears it
on entry (see [traps.md](traps.md)).

### SYSRET only when it is safe (CVE-2012-0217)

On Intel CPUs, `SYSRET` to a non-canonical RIP raises `#GP` *in ring 0,
with the user's RSP*: a classic privilege escalation. The exit path checks
the return RIP is canonical, and the frame still describes user CS/SS,
before using `SYSRET`. Otherwise it takes `iretq`.

The `sysret_hazard` program places its `syscall` as the last two bytes of
the user address space, so the return RIP is `0x0000800000000000`. The
tests assert that the kernel took the `iretq` path (`syscall_slow_exits`)
and that only the process died. They also assert the opposite for an
ordinary program: its syscalls must use the fast path.

### When iretq itself faults

`iretq` to ring 3 can fault: a non-canonical RIP on real CPUs, or a bad SS.
The `#GP` is then raised in ring 0, at the `iretq` instruction, *after*
`swapgs` has already installed the user GS. That instruction has an
exception-table entry. The fixup restores the kernel GS and kills the
process (`user_bad_iret`). A test drives this deterministically by
"returning" with an SS no CPU accepts, then checks that a later syscall
still finds its stack through GS.

QEMU 8.2's TCG does not check RIP canonicality in `iretq`. There the
hazard program instead dies one instruction later, with a user-mode
`#PF`. QEMU 9.2's TCG does check, so the x2APIC runs (`make test-x2apic`)
take the real `#GP` path, and the recovery works. The test accepts either
outcome and documents why.

## System calls

| # | Call | Notes |
|---|------|-------|
| 0 | `exit(code)` | |
| 1 | `write(fd, buf, len)` | fd 1/2 only; copied in 256-byte chunks; each chunk is one console message, sent to COM1 with interrupts off (see [console.md](console.md)) |
| 2 | `yield()` | |
| 3 | `getpid()` | |
| 4 | `sleep_ms(ms)` | capped at one hour; sleeps at least `ms` (see [scheduler.md](scheduler.md)) |
| 5 | `spawn(name)` | name copied byte by byte (it may end just before an unmapped page); `-EAGAIN` at `TASK_MAX` tasks |
| 6 | `wait(pid or -1, int *status)` | the status pointer is written (with 0) *before* reaping, so an unmapped, read-only or kernel pointer returns `-EFAULT` and the child stays waitable |
| 7 | `time_ms()` | |

**User memory** is reached only through `copy_from_user`, `copy_to_user`
and `strncpy_from_user`. Each checks that the whole range lies below
`USER_TOP`, which rejects kernel pointers and wrapping lengths. Each also
lifts SMAP for the copy alone (`stac`/`clac`, only on CPUs that have them),
and turns faults into `-EFAULT` through the exception table. `badptr`
throws kernel pointers, unmapped pointers, wrapping and `USER_TOP`-crossing
ranges, bad fds, over-long names and unknown syscall numbers at the kernel,
and checks every errno. Its `wait` cases (kernel, unmapped and read-only
status pointers) must each fail without reaping the child, which a final
`wait` then collects.

A range check alone is not enough for `wait`'s status pointer: the page
may be unmapped or read-only, and the copy-out happens after the child is
reaped. So `sys_wait` writes the status once before waiting, through
`copy_to_user`. Processes are single-threaded, so nothing can unmap the
page between that write and the real one.

## Faults

A CPU exception in ring 3 kills the process with status `128 + vector`
and a log line; the kernel and every other process carry on. Two vectors
are excluded (see [traps.md](traps.md)): an NMI is counted and reported,
and the process continues; a machine check panics the kernel. `int3` in
ring 3 kills the process too (status 131). The tests run each hostile
program and check the exact status:
- NULL write: `#PF`
- `cli`: `#GP`
- reading kernel memory: `#PF`, protection, user
- writing its own text: `#PF`, W^X
- an unmasked x87 divide by zero (`x87fault`): `#MF`, status 144. This
  needs `CR0.NE`, which the BIOS path inherits clear. Without it the error
  goes to the legacy FERR#/IRQ 13 route, which nothing handles, and the
  program would exit normally.

`flagbomb` sets NT and AC, then makes 100 `yield` calls and spins for
30 ms (about 30 timer interrupts) with both set, while `hello` runs
alongside. Both must exit normally, `flagbomb` with its own NT and AC
still set. On a CPU with SMAP the test also checks that interrupts did
arrive with AC set, so the `clac` in `trap_dispatch()` was exercised.

## FPU

The kernel never uses FPU registers, so state is switched eagerly and only
for tasks that own an FXSAVE area (processes): save on switch-out, restore
on switch-in. The `fpu` test fills all 16 XMM registers with a
per-process pattern in assembly, then yields 300 times while two other
instances do the same, and checks that every register survived.

Every process starts from one template, saved by `fpu_init()` after
`fninit` and `MXCSR = 0x1F80` (all SSE exceptions masked). `fninit` resets
the x87 control words but not the register contents, and nothing resets
XMM0–15, so the template used to carry whatever the firmware left there
(OVMF uses SSE) into every process. `fpu_init()` now zeroes ST0–7,
XMM0–15 and the reserved tail of the template. `fpuclean` saves its own
state before doing anything else and fails if any of ST0–7 or XMM0–15 is
nonzero.

## init

`kmain` starts `init` and then reaps forever, adopting orphans. `init`
starts eleven programs (well-behaved, CPU-bound, sleeping, FPU-checking,
hostile and deliberately crashing), reaps them all, and exits 0 only if
each one ended exactly as expected. `disk-demo.img` boots this and checks
the result, so `make run` is itself under test.
