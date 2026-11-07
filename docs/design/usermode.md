# User mode and system calls

Sources: `kernel/proc/`, `kernel/arch/x86_64/{syscall.asm,isr.asm,usercopy.c,fpu.c,percpu.c}`,
`kernel/include/uapi/syscall.h`, `user/`.

## Processes

A process is a task with its own address space and FPU state, started
from an ELF program in the initrd, `bin/NAME` (`user_image_find()`;
[boot.md](boot.md#the-initrd) has how the initrd arrives). The kernel
reads the program in place, as the initrd is never freed. Programs read
the initrd's files themselves through `open`, `read`, `close` and
`readdir` (below); `spawn(name, args)` creates a process from a program,
as `posix_spawn` would, rather than via fork/exec. With no usable
initrd there is no `init`, and the kernel panics saying so. The program starts as `main(argc, argv)`, `argv[0]` its name:
the kernel copies the strings to the top of the new stack with the
NULL-terminated vector below them, leaves RSP 16-byte aligned below both,
and passes argc in RDI and argv in RSI, which `crt0` hands on to `main`.

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
| 1 | `write(fd, buf, len)` | `-EBADF` unless `fd` is open and its file can be written: the console's output, a pipe's write end, or a `/tmp` file opened to write (below). The console: copied in 256-byte chunks; each chunk is one console message, sent to COM1 with interrupts off (see [console.md](console.md)). A pipe: below |
| 2 | `yield()` | |
| 3 | `getpid()` | |
| 4 | `sleep_ms(ms)` | capped at one hour; sleeps at least `ms` (see [scheduler.md](scheduler.md)) |
| 5 | `spawn(name, args, stdio)` | name copied byte by byte (it may end just before an unmapped page); `args` is NULL or a NULL-terminated list of at most 8 strings, 256 bytes in all with their NULs (`-E2BIG` past either); `stdio` is NULL (the child gets the caller's fds 0-2) or three of the caller's fds to be the child's 0-2 (`-EBADF` for one that is not open). All of it is copied and checked before anything is created (`-EFAULT`, `-ENAMETOOLONG`, `-E2BIG`, `-EBADF`, in that order of discovery); `-EAGAIN` at `TASK_MAX` tasks |
| 6 | `wait(pid or -1, int *status)` | the status pointer is written (with 0) *before* reaping, so an unmapped, read-only or kernel pointer returns `-EFAULT` and the child stays waitable |
| 7 | `time_ms()` | |
| 8 | `read(fd, buf, len)` | `-EBADF` unless `fd` is open and its file can be read, then as the file's kind says. The console's input (fd 0 unless spawn said otherwise): raw bytes, never echoed; blocks until at least one byte is queued, returns at most 256. Fails in this order, before anything is taken off the queue: `-EFAULT` for the range, 0 for `len` 0, `-EFAULT` unless every byte it could fill is mapped and writable (probed, contents kept, so a bad buffer never swallows a keystroke), `-ENODEV` with no input device. An initrd file: from its position, as much as fits, 0 at the end; `-EFAULT` for the range, 0 for `len` 0, then copied out one user page at a time: the first page that is not writable ends the read exactly there (`-EFAULT` if it is the first), and the position moves by exactly the bytes before it. A pipe's read end: below |
| 9 | `open(path, flags)` | a file (paths below). `flags`: `O_RDONLY` 0, `O_WRONLY` 1 or `O_RDWR` 2, with any of `O_CREAT` (make it if missing, in `/tmp` only), `O_TRUNC` (empty it) and `O_APPEND` (every write at its end); any other bit, mode 3, or `O_TRUNC`/`O_APPEND` without writing is `-EINVAL`, so a v1.5 program's `open(path)` still opens to read. The path is copied first (`-EFAULT`, `-ENAMETOOLONG` with no NUL in `PATH_MAX` = 256 bytes), then the flags (`-EINVAL`), then the lookup (`-EINVAL`, `-ENOENT`, `-ENOTDIR`), `-EISDIR`, `-EROFS` (writing or making a file outside `/tmp`), `-EMFILE` with `FILES_MAX` = 8 open, then making it (`-ENOSPC`, `-ENOMEM`). `O_TRUNC` empties a file only once nothing else can fail. Returns the lowest free fd from 3 |
| 10 | `close(fd)` | `-EBADF` for 0-2, which stay open until exit, and for any fd not open |
| 11 | `readdir(path, index, struct dirent *out)` | entry `index` of a directory, from 0: the root lists `tmp` first, then the initrd's root in archive order; `/tmp` lists its files in the order they were made; 1 with `out` filled (`size`, `type` `DIRENT_FILE` or `DIRENT_DIR`, its own `name`), 0 past the last. The path first (`-EFAULT`, `-ENAMETOOLONG`), then `-EFAULT` unless `out` is in the user range, then `-EINVAL`, `-ENOENT`, `-ENOTDIR`, then `-EFAULT` if `out` is not writable. No handle: listing a directory is calling it with 0, 1, 2... |
| 12 | `fstat(fd, struct stat *out)` | what an open fd is: `type` `STAT_FILE` (an initrd or `/tmp` file; `size` its size now), `STAT_CONSOLE` (`size` 0) or `STAT_PIPE` (either end; `size` the bytes waiting). `-EBADF`, then `-EFAULT` unless `out` is writable |
| 13 | `pipe(int fds[2])` | a new pipe: `fds[0]` its read end and `fds[1]` its write end, at the two lowest free fds from 3. `-EFAULT` unless both ints are writable (probed, contents kept, before anything is made), `-EMFILE` with fewer than two fds free, `-ENOMEM` |
| 14 | `unlink(path)` | removes a `/tmp` file's name. The path (`-EFAULT`, `-ENAMETOOLONG`), then the lookup (`-EINVAL`, `-ENOENT`, `-ENOTDIR`), then `-EISDIR` for a directory (`/tmp`, `/`, `/bin`), `-EROFS` for an initrd file. The file goes when its last fd closes |

**Files.** Two file systems: the initrd, read-only, and `/tmp`, files
in RAM (below). A path is
absolute: `/` is the root and `/bin/sh` the archive's `bin/sh`; a
trailing `/` names a directory (`-ENOTDIR` on a file); an empty, `.` or
`..` component, or a path not starting with `/`, is `-EINVAL`
(`kernel/fs/tarfs.c`, host-tested). There is no current directory. A
directory is one the archive has an entry for, or the root, and every
parent on a path must be one (`-ENOENT` if missing, `-ENOTDIR` if a
file). The first entry of a name wins, for a lookup and in a listing
alike; the tar reader refuses names with an empty, `.` or `..`
component outright, so a hostile archive cannot make a lookup and a
listing disagree.

**/tmp** (`kernel/fs/tmpfs.c`, pure and host-tested, and
`kernel/fs/vfs.c`). A flat directory of files held in RAM: there is no
disk driver, so they are gone at reboot. `/tmp/NAME`, with `NAME` at most
63 bytes (`TMP_NAME_MAX`); there are no subdirectories, so
`/tmp/f/x` is `-ENOTDIR` for a file `f` and `-ENOENT` otherwise. The
path rules are the initrd's, and `/tmp` hides any `tmp` the archive has
(`mkinitrd.py` refuses to make one). A file is a name, a size and up to
256 pages of 4 KiB, allocated as writes reach them; at most 32 files
(`TMP_FILES_MAX`), counting removed ones still open, and 1 MiB of pages
in all (`TMP_BYTES_MAX`): past either, `-ENOSPC`. A write that runs out
of room part-way returns what it wrote. `unlink` takes the name away;
the file lives on through the fds open on it, and goes with the last.
Each `open` makes its own open file with its own position, starting at
0 (`O_APPEND` moves it to the end at each write); the fds `spawn` hands
on share theirs. Reads and writes copy a piece at a time, within one
page of the file and one page of the user's buffer: the first piece
that faults ends the call there, with `-EFAULT` if it was the first, so
the position always moves by exactly what was copied. Every use of
`/tmp` holds one sleeping mutex, the copies included, so a file and its
positions change one call at a time.

**Open files** (`kernel/fs/file.c`). An fd refers to an open file: the
console's input, the console's output, an initrd file (a range of the
never-freed initrd, and a position), a `/tmp` file and a position, or
one end of a pipe. Each kind says whether it can be
read and written; one that cannot is `-EBADF`. Every fd that refers to a
file shares it, its position included, and holds a reference; the last
to close frees it. The console's two files are static and never freed.

Each process has a table of 11 fds in its task. 0-2 are its standard
input, output and error: for `init`, the console's input and its output
twice; for a child, what `spawn` named, or else its parent's own 0-2.
They cannot be closed. 3-10 (`FILES_MAX` from `FD_FIRST`) are the files
it opened, and a new process has none of those. Every fd closes when
the process exits, however it ends: `task_exit()` drops each reference
before it frees the address space. A process is single-threaded and only
it changes its own table, so the table needs no lock; a file's count is
atomic, as processes share it, and a read of an initrd file runs with
preemption off from reading the position to moving it, so two processes
reading one file never both read from the same place.

The console has no end of its own, so a program that reads standard
input to its end (`cat` with no arguments) goes through `stdin_read()`
in `user/lib/libsys.c`: when `fstat` says fd 0 is the console, it edits
input a line at a time with the shell's line editor (75 characters at
most), echoes it to fd 2 when that is the console too, and takes Ctrl-D
on an empty line as the end, as a terminal would. A first byte LF is
skipped: the shell starts a program at the CR of a terminal's CRLF, and
the LF is still queued.

**Pipes** (`kernel/fs/pipe.c`). A pipe is a 4 KiB ring and its two
ends, each an open file shared by every fd on it; an end closes when the
last of those does, and the pipe is freed when both have. A read waits
for at least one byte, then takes what is there; once no write end is
open anywhere, it returns what is left and then 0. A write waits until
all of it is in, 256 bytes (`PIPE_ATOMIC`) at a time, each piece put in
whole, so writes of up to 256 bytes from several processes never mix;
once no read end is open, it returns the bytes it put in, or `-EPIPE`
if none (there are no signals). A process that reads a pipe it also
holds the only write end of, with nothing in it, waits forever, as on
Unix. User memory is copied outside the
pipe's lock, a piece at a time through a buffer on the kernel stack. A
read probes each piece of the buffer before it takes a byte, as the
console's read does, so a buffer that is not writable never loses data:
the call ends at the first bad piece, with `-EFAULT` if it is the first.
Both sides wait with `wait_event()`, which checks after the task is
queued and marked blocked, so a wakeup cannot be lost between the check
and the sleep. `pipetest` checks all of this from ring 3, with children
that wait on a full and an empty pipe, a 200-byte write that must wait
with none of it in while the pipe has room for only 150, two writers of
whole records into one pipe, and `cat` between two pipes; the kernel
test around it checks that every pipe and end is freed, and another
fails each of the four allocations a pipe makes in turn
(`kmalloc_test_fail_one()`).

`tmptest` checks `/tmp` from ring 3: each of `open`'s flags and errors
in order, the initrd's `-EROFS`, files made, written, read, emptied,
appended to from two fds and removed while open, names at and past the
limit, the space and file limits to the byte, a full fd table, buffers
that fault part-way, and `echo` and `cat` with `/tmp` files as their
stdin and stdout. The kernel test around it checks that every file,
page and open file is given back; another fails each allocation
`open` makes in turn. The host tests run the core through every path
rule, limit and lifetime, and 20,000 random operations against a model.

`fstest` checks each error above from ring 3, the order where two can
apply, a read whose buffer runs into an unmapped page, and `spawn`'s
`stdio`: a child sharing the parent's file position, a grandchild
inheriting fd 0 after the parent closed it, the console's input and
output swapped, and one killed by a fault with a closed fd among open ones. The kernel
test around it checks that no file outlives the processes.

**User memory** is reached only through `copy_from_user`, `copy_to_user`
and `strncpy_from_user`. Each checks that the whole range lies below
`USER_TOP`, which rejects kernel pointers and wrapping lengths. Each also
lifts SMAP for the copy alone (`stac`/`clac`, only on CPUs that have them),
and turns faults into `-EFAULT` through the exception table. `badptr`
throws kernel pointers, unmapped pointers, wrapping and `USER_TOP`-crossing
ranges, bad fds, over-long names and unknown syscall numbers at the kernel,
and checks every errno. Its `read` cases also pin the order of the checks
(a bad fd with a kernel pointer is `-EBADF`, a kernel pointer with length
0 is `-EFAULT`) and probe every byte a read could fill (8 bytes from 4
below the stack top). Like `wait`, `read` probes its buffer before it takes
anything, so a bad buffer never swallows a keystroke ([input.md](input.md)). Its `wait` cases (kernel, unmapped and read-only
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

`init=NAME` starts another program first. The shell images
(`disk-shell.img`, `disk-uefi-shell.img`) boot `init=sh exit-after-init`;
`disk-badinit.img` boots `init=no-such-prog` and must panic with that
name ([input.md](input.md)).
