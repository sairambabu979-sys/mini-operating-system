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
is reaped, on its parent's stack and page tables, never while the space is
active.

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

`SYSCALL` leaves RSP pointing at the user stack and interrupts disabled
(`FMASK` clears IF, DF, TF and AC). The entry does `swapgs`, saves the user
RSP in per-CPU scratch space, loads the kernel stack, builds the frame,
and only then re-enables interrupts. Syscalls are preemptible.

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

QEMU's TCG does not check RIP canonicality in `iretq`. There the hazard
program instead dies one instruction later, with a user-mode `#PF`. The
test accepts either outcome and documents why.

## System calls

| # | Call | Notes |
|---|------|-------|
| 0 | `exit(code)` | |
| 1 | `write(fd, buf, len)` | fd 1/2 only; copied in 256-byte chunks; each chunk is one console message |
| 2 | `yield()` | |
| 3 | `getpid()` | |
| 4 | `sleep_ms(ms)` | capped at one hour |
| 5 | `spawn(name)` | name copied byte by byte (it may end just before an unmapped page) |
| 6 | `wait(pid or -1, int *status)` | the status pointer is validated *before* reaping, so a bad pointer cannot consume the child |
| 7 | `time_ms()` | |

**User memory** is reached only through `copy_from_user`, `copy_to_user`
and `strncpy_from_user`. Each checks that the whole range lies below
`USER_TOP`, which rejects kernel pointers and wrapping lengths. Each also
lifts SMAP for the copy alone (`stac`/`clac`, only on CPUs that have them),
and turns faults into `-EFAULT` through the exception table. `badptr`
throws kernel pointers, unmapped pointers, wrapping and `USER_TOP`-crossing
ranges, bad fds, over-long names and unknown syscall numbers at the kernel,
and checks every errno.

## Faults

A CPU exception in ring 3 kills the process with status `128 + vector`
and a log line; the kernel and every other process carry on. The tests
run each hostile program and check the exact status:
- NULL write: `#PF`
- `cli`: `#GP`
- reading kernel memory: `#PF`, protection, user
- writing its own text: `#PF`, W^X

## FPU

The kernel never uses FPU registers, so state is switched eagerly and only
for tasks that own an FXSAVE area (processes): save on switch-out, restore
on switch-in. The `fpu` test fills all 16 XMM registers with a
per-process pattern in assembly, then yields 300 times while two other
instances do the same, and checks that every register survived.

## init

`kmain` starts `init` and then reaps forever, adopting orphans. `init`
starts eleven programs (well-behaved, CPU-bound, sleeping, FPU-checking,
hostile and deliberately crashing), reaps them all, and exits 0 only if
each one ended exactly as expected. `disk-demo.img` boots this and checks
the result, so `make run` is itself under test.
