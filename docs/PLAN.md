# Mini Operating System — Engineering Plan

> Status: **v1.0.0** = M0–M8 · **v1.1.0** = everything merged after it (parts 2–4); §11
> records the main additions and fixes, several of which fix v1.0.0 code ·
> Target: x86_64, single core, QEMU first, never yet booted on real hardware
>
> This is the plan as written before any code, kept as the record of what was
> intended. §11 records where the build is known to differ from it, and why.

This document is the design and execution plan for a small, from-scratch x86_64
kernel: our own bootloader, 4-level paging, a full interrupt/exception layer, and
a preemptive scheduler running both kernel threads and ring-3 processes.

The guiding principle: **every feature ships with a test that boots in CI.**
A kernel looks well engineered because its invariants are written down, enforced,
and checked automatically, not because it has a lot of features.

---

## 1. Goals and non-goals

### Goals (v1.0)

| # | Capability | Done means |
|---|------------|------------|
| G1 | **Bootloading** | Own two-stage BIOS loader: MBR → stage 2 → long mode → loads the ELF64 kernel and hands over a versioned `boot_info` structure. No GRUB. |
| G2 | **Memory management** | E820-driven buddy frame allocator, kernel-owned 4-level page tables, higher-half kernel, direct map of physical memory, W^X section permissions, guard pages, `kmalloc`. |
| G3 | **Interrupts and exceptions** | GDT/TSS/IDT, all 32 CPU exceptions with diagnostic dumps and symbolized backtraces, IST stacks for `#DF`/NMI/`#MC`, remapped PIC, PIT timer. |
| G4 | **Scheduling** | Preemptive round-robin scheduler, kernel threads, sleep and wait queues, process exit and reaping. |
| G5 | **Processes** | Ring-3 processes in separate address spaces, loaded from embedded ELF images, using `syscall`/`sysret` with validated user pointers. A faulting process is killed and the kernel keeps running. |
| G6 | **Engineering** | Reproducible build, CI that boots the kernel in QEMU, host unit tests under ASan/UBSan, in-kernel test framework, kernel UBSan, design docs. |

### Non-goals (v1.0), each a deliberate cut

- **SMP.** Locking primitives are SMP-shaped (`spin_lock_irqsave`), so adding it later
  does not mean a rewrite.
- **UEFI boot.** The `boot_info` ABI is loader-agnostic, so a UEFI loader can be added
  without touching the kernel.
- **Filesystems, a VFS, block drivers, networking.** User programs are embedded in the
  kernel image.
- **`fork()` and copy-on-write.** Processes are created with `spawn(image)`, as with
  `posix_spawn`. Fork needs COW to be worth having, and COW is a project of its own.
- **Graphics.** Serial is the primary console, with VGA text mode as a mirror.

Anything not listed under Goals is out of scope until v1.0 is tagged.

---

## 2. Key decisions

The rationale for each decision lives in this table and in `docs/design/` (see §11).

| Decision | Choice | Why | Rejected alternative |
|----------|--------|-----|----------------------|
| Architecture | **x86_64** | Modern ABI, 4-level paging, NX, `syscall`. This is the target real systems run on. | i386: easier, but it teaches a dead paging model. |
| Bootloader | **Custom 2-stage BIOS loader (NASM)** | The project asks for bootloading, and using GRUB/Limine would mean we never built it. | Limine/Multiboot2: faster to start, but it skips a stated requirement. |
| Toolchain | **clang + ld.lld**, `--target=x86_64-unknown-none-elf` | A real cross-compiler with nothing to build. Both are already installed. | Building a cross `x86_64-elf-gcc` costs about 30 minutes and adds a CI cache to maintain. |
| Language | **C17 + NASM** | NASM for boot and entry/exit stubs, C for everything else. Inline asm only for single instructions (`cpu.h`). | GAS syntax: workable, but mixing two asm dialects adds noise. |
| Build | **Non-recursive GNU Make**, out-of-tree `build/` | The standard for kernels, transparent, with `-MMD -MP` dependency tracking. | CMake: more indirection, and it fights freestanding toolchains. |
| Frame allocator | **Buddy allocator** | O(log n), contiguous multi-page allocations (kernel stacks, page tables), and testable on the host. | Bitmap: simpler, but O(n) and poor at contiguous allocations. |
| Timer | **PIT + 8259 PIC** (v1), LAPIC as a stretch goal | Lowest-risk path to a working tick. The LAPIC timer must be calibrated against the PIT anyway. | Going straight to the LAPIC adds ACPI/MADT parsing to the critical path. |
| Preemption | **Preemptible kernel, `preempt_count`** | Busy kernel threads must be preemptible, otherwise the scheduler only switches when threads cooperate. | Preempting only on return to user: kernel threads would never be preempted. |
| Syscall entry | **`syscall`/`sysret`** with an `iretq` fallback | The fast path is the modern one. The fallback covers the non-canonical `RCX` hazard (CVE-2012-0217). | `int 0x80`: slower and legacy. |

---

## 3. System architecture

```
 ┌───────────────────────────────────────────────────────────────────────┐
 │  User space (ring 3)    init   ·   demo programs   ·   libsys (crt0)  │
 ├───────────────────────────────── syscall ─────────────────────────────┤
 │  Kernel (ring 0)                                                      │
 │   ┌──────────┐  ┌────────────┐  ┌──────────────┐  ┌───────────────┐   │
 │   │ syscall  │  │  process   │  │  scheduler   │  │   klog/panic  │   │
 │   │ dispatch │  │ lifecycle  │  │ RR + waitq   │  │   ktest       │   │
 │   └────┬─────┘  └─────┬──────┘  └──────┬───────┘  └───────────────┘   │
 │        │       ┌──────┴──────┐  ┌──────┴───────┐                      │
 │        │       │ VMM (4-lvl) │  │ context sw.  │                      │
 │        │       │ kmalloc     │  │ (asm)        │                      │
 │        │       └──────┬──────┘  └──────────────┘                      │
 │        │       ┌──────┴──────┐  ┌──────────────┐  ┌───────────────┐   │
 │        │       │ PMM (buddy) │  │ IDT / traps  │  │ drivers:      │   │
 │        │       └─────────────┘  │ GDT / TSS    │  │ serial PIT PIC│   │
 │        │                        └──────────────┘  │ VGA text      │   │
 │        └──────────── arch/x86_64 ─────────────────┴───────────────┘   │
 ├───────────────────────────────────────────────────────────────────────┤
 │  Boot: stage1 (MBR) → stage2 (real → long mode, E820, ELF load)       │
 └───────────────────────────────────────────────────────────────────────┘
```

Dependency rule: arrows only point downward. `arch/` exposes a narrow interface
(`arch_*` functions, `cpu.h`), and nothing above `mm/` touches CR registers or MSRs directly.

---

## 4. Repository layout

```
.
├── boot/
│   ├── stage1.asm            # MBR, 446 bytes of code + partition table + 0x55AA
│   ├── stage2.asm            # A20, E820, unreal-mode loader, long mode, ELF64 load
│   └── inc/                  # layout.inc (boot memory map); asm-offsets.inc is generated
├── kernel/
│   ├── arch/x86_64/
│   │   ├── entry.asm         # _start: stack, rbp=0, call kmain(boot_info*)
│   │   ├── isr.asm           # 256 vector stubs (macro-generated), common trap path
│   │   ├── switch.asm        # switch_to(prev, next)
│   │   ├── syscall.asm       # swapgs, stack switch, sysret/iretq exit
│   │   ├── gdt.c  idt.c  tss.c  traps.c  cpu.c  fpu.c  usercopy.c
│   │   └── include/arch/     # cpu.h msr.h io.h regs.h paging.h segments.h
│   ├── mm/                   # pmm_buddy.c  vmm.c  vmalloc.c  kmalloc.c  memmap.c
│   ├── sched/                # task.c  sched.c  waitq.c  sleep.c
│   ├── proc/                 # process.c  elf_loader.c  syscalls.c
│   ├── drivers/              # serial.c  vga_text.c  pic8259.c  pit.c
│   ├── core/                 # main.c  panic.c  klog.c  ksyms.c  ubsan.c  ktest.c
│   ├── lib/                  # string.c  printf.c  list.h  bitops.h
│   ├── include/              # kernel-wide headers: types.h  compiler.h  errno.h  ...
│   ├── asm-offsets.c         # generates struct offsets for NASM (Linux-style)
│   └── linker.ld             # higher-half link, page-aligned sections, W^X
├── user/
│   ├── crt0.asm  libsys/     # syscall wrappers, minimal printf
│   └── programs/             # init.c, spinner.c, faulter.c, sleeper.c ...
├── tests/
│   ├── host/                 # unit tests of pure modules, run on Linux under ASan/UBSan
│   ├── kernel/               # KTEST() cases compiled into the test kernel
│   └── qemu/run_tests.py     # boots the image, parses TAP from serial, sets the exit code
├── tools/
│   ├── mkimage.py            # assembles disk.img, patches the stage2 header with LBA/size
│   ├── gen_ksyms.py          # symbol table for backtraces (multi-pass link)
│   └── gdb/                  # gdbinit
├── docs/
│   ├── PLAN.md               # this file
│   └── design/               # boot.md traps.md memory.md scheduler.md usermode.md
├── .github/workflows/ci.yml
├── .clang-format  .clang-tidy  .editorconfig
├── Makefile
└── README.md
```

---

## 5. Subsystem designs

### 5.1 Boot (`boot/`)

**Disk image layout** (built by `tools/mkimage.py`):

```
LBA 0          stage1 (MBR, one active partition entry for real-HW compatibility)
LBA 1..63      stage2 (≤ 31.5 KiB, in the conventional post-MBR gap)
LBA 64..       kernel.elf (sector-padded)
```

`mkimage.py` patches a small header at the start of stage 2 with the kernel's
starting LBA and sector count, so the loader has no hard-coded sizes.

**Stage 1** (16-bit, loaded at `0x7C00`):
1. `cli`, zero the segment registers, stack below `0x7C00`, save the BIOS boot drive (`DL`).
2. Check for INT 13h extensions (`AH=41h`), then load stage 2 with `AH=42h` (LBA DAP).
3. Far jump to stage 2. On any failure, print a one-letter error code and `hlt`.

**Stage 2** (16 → 64-bit; real mode switches straight to long mode):
1. **A20**: BIOS `INT 15h AX=2401h`, then the keyboard controller, then fast A20 (port `0x92`).
   Each attempt is checked with a memory-wraparound test.
2. **Memory map**: `INT 15h EAX=E820h`, handling both 20- and 24-byte entries (ACPI 3.0 attributes).
   Stored in the `boot_info` buffer.
3. **Load the kernel**: enter *unreal mode* (32-bit data-segment limits while in real mode).
   Read in ≤127-sector chunks to a bounce buffer below 1 MiB, then copy each chunk to the
   staging area above 1 MiB. This avoids bouncing between real and protected mode per chunk.
4. **CPU checks**: CPUID extended leaf present, `LM` bit (`0x80000001:EDX[29]`). Fail loudly.
5. **Boot page tables**: identity-map 0–1 GiB with 2 MiB pages, and map the same range at
   `0xFFFFFFFF80000000`.
6. **Long mode**: `CR4.PAE`, load `CR3`, `EFER.LME` (MSR `0xC0000080`), `CR0.PE|PG`, then a far jump
   into a 64-bit code segment.
7. **ELF64 load**: validate the header (magic, class, machine, type). For each `PT_LOAD`, copy
   `p_filesz` to `p_paddr` and zero up to `p_memsz`.
8. Jump to `e_entry` with `RDI = &boot_info` (SysV first argument).

**Boot protocol** (`kernel/include/boot/boot_info.h`, the single source of truth):

```c
#define BOOT_INFO_MAGIC   0x4F464E49544F4F42ULL /* "BOOTINFO" little-endian */
#define BOOT_INFO_VERSION 1

struct boot_info {
    u64 magic;
    u32 version;
    u32 mmap_count;
    u64 kernel_phys_start, kernel_phys_end;
    u64 boot_drive;
    char cmdline[BOOT_CMDLINE_MAX];      /* e.g. "ktest loglevel=debug" */
    struct e820_entry mmap[BOOT_MMAP_MAX];
};
```

The NASM mirror `asm-offsets.inc` is **generated** from the C header via `asm-offsets.c`,
so the two cannot drift apart. `static_assert`s pin the layout.

### 5.2 Early kernel and diagnostics (`core/`, `drivers/`)

- `_start`: load the kernel's boot stack, `xor rbp, rbp` (terminates backtraces), `cld`,
  verify `boot_info->magic`, then `call kmain`.
- **Console**: COM1 16550 UART (115200 8N1) is the primary console. VGA text is a mirror.
- **klog**: levels (`ERR`/`WARN`/`INFO`/`DEBUG`), a ring buffer, tick timestamps, and
  `printk`-style format checking via `__attribute__((format(printf, 1, 2)))`.
- **panic()**: disables interrupts, prints the reason, registers and control registers, and a
  frame-pointer backtrace symbolized against the embedded symbol table. Then it halts (or
  exits QEMU with a failure code in test builds).
- **Symbol table**: kallsyms-style multi-pass link. Link, extract sorted symbols with `nm`,
  compile them into `.ksyms`, and relink. The table sits at the end of the image so its
  size converges.
- **Kernel UBSan**: debug builds use `-fsanitize=undefined` with our own `__ubsan_handle_*`
  implementations that report through klog.

### 5.3 CPU tables and exceptions (`arch/x86_64/`)

**GDT layout**. The order is required by `syscall`/`sysret` (STAR MSR semantics):

| Selector | Descriptor |
|----------|------------|
| `0x00` | null |
| `0x08` | kernel code 64 |
| `0x10` | kernel data |
| `0x18` | user code 32 (placeholder, required by the `sysret` layout) |
| `0x20` | user data (`0x23` with RPL 3) |
| `0x28` | user code 64 (`0x2B` with RPL 3) |
| `0x30` | TSS (16-byte system descriptor) |

**TSS**: `RSP0` is updated on every context switch. IST1 holds the `#DF` stack, IST2 NMI,
and IST3 `#MC`, each with its own guard page, so a kernel stack overflow produces a clean
`#DF` report instead of a triple fault.

**IDT**: 256 stubs generated by a NASM macro. Vectors that have no CPU error code
push a dummy one, so the trap frame always has the same shape:

```c
struct trap_frame {            /* matches isr.asm push order exactly */
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector, error_code;
    u64 rip, cs, rflags, rsp, ss;   /* pushed by the CPU */
};
```

The common path saves registers, does `swapgs` iff `cs & 3`, calls `trap_dispatch(frame)`,
then runs the **return-to-context** path. That path is the single place where pending
reschedules are honored.

**Exception policy**:
- From user mode: log the fault, then kill the process (exit code `128 + vector`).
- From kernel mode, inside a `copy_{to,from}_user` window: the exception-table fixup
  returns `-EFAULT`.
- From kernel mode anywhere else: `panic()` with a full decode. For `#PF` that includes
  `CR2` and the P/W/U/R/I bits, plus the page-table walk for the faulting address.

### 5.4 Physical memory (`mm/pmm_buddy.c`)

- Input: E820 map, sanitized. Entries are sorted, overlaps resolved toward the most
  restrictive type, and boundaries page-aligned inward.
- Reserved: the low 1 MiB, the kernel image, `boot_info`, the boot page tables.
- **Buddy allocator**, orders 0–10 (4 KiB to 4 MiB), with free lists per order and a
  per-frame `struct page` array (flags, order, refcount).
- API: `pmm_alloc(order)`, `pmm_free(pfn, order)`, `pmm_stats()`.
- Invariants are checked by `pmm_verify()` in debug builds: no double free, buddies
  coalesce, and free plus used equals total.
- **Host-tested**: the allocator core has no hardware dependencies and is compiled into
  `tests/host` with ASan/UBSan and a randomized alloc/free fuzz loop.

### 5.5 Virtual memory (`mm/vmm.c`, `mm/vmalloc.c`, `mm/kmalloc.c`)

**Kernel virtual address map:**

```
0x0000_0000_0000_0000 ─ 0x0000_7FFF_FFFF_FFFF   user space (per process)
            ── non-canonical hole ──
0xFFFF_8000_0000_0000 ─ +512 GiB                direct map of all physical RAM (HHDM)
0xFFFF_C000_0000_0000 ─ +512 GiB                vmalloc: kernel stacks with guard pages, etc.
0xFFFF_FFFF_8000_0000 ─ 0xFFFF_FFFF_FFFF_FFFF   kernel image (-2 GiB; -mcmodel=kernel)
```

- The kernel **builds its own page tables** and does not reuse the loader's. Once
  running at the high address, the identity map is dropped, so NULL and low-address
  dereferences fault.
- **W^X**: the linker script page-aligns sections. `.text` is R-X, `.rodata` is R-- NX,
  and `.data`/`.bss` are RW- NX. `EFER.NXE` and `CR0.WP` are enabled. **SMEP/SMAP** are
  enabled when CPUID reports them, with `stac`/`clac` around user copies.
- **Shared kernel half**: all 256 upper-half PML4 entries get their PDPTs at boot. Every
  process PML4 copies those entries, so kernel mappings never need to be synchronized
  across address spaces.
- API: `vmm_map(as, va, pa, flags)`, `vmm_unmap`, `vmm_translate`, `as_create`, `as_destroy`.
  `invlpg` runs on every unmap or permission downgrade, and a full `CR3` reload on
  address-space switch (no PCID in v1).
- **Kernel stacks**: 16 KiB in vmalloc space with an unmapped guard page below each one.
- **kmalloc**: slab-style size classes (16 to 2048 bytes) carved from pages. Larger requests
  go straight to the PMM. Debug builds use redzones and a poison pattern on free.

### 5.6 Interrupts and time (`drivers/pic8259.c`, `drivers/pit.c`)

- Remap the 8259 PIC to vectors `0x20–0x2F` (ICW1–ICW4) and mask everything except
  enabled lines.
- **Spurious IRQ 7/15**: read the ISR before sending EOI. A spurious IRQ 15 still needs EOI
  sent to the master PIC.
- PIT channel 0 at **1000 Hz** sets a global `jiffies`-style tick counter. `arch_irq_save()`/
  `arch_irq_restore()` are the only way code toggles `IF`.
- IRQ handler registration: `irq_register(line, handler, ctx)`. Handlers run with
  interrupts disabled and must not sleep, which `might_sleep()` checks in debug builds.

### 5.7 Scheduler (`sched/`)

```c
struct task {
    struct cpu_context ctx;       /* callee-saved regs + rsp; offsets via asm-offsets */
    void *kstack_top;
    struct address_space *as;     /* NULL for kernel threads (borrow kernel PML4) */
    struct fpu_state *fpu;        /* FXSAVE area, user tasks only */
    enum task_state state;        /* RUNNABLE, RUNNING, BLOCKED, ZOMBIE */
    pid_t pid;
    struct task *parent;
    u32 time_slice;               /* ticks remaining in quantum */
    int exit_code;
    struct list_node run_node, wait_node, child_node;
    struct list_head children;
    char name[16];
};
```

- **Policy**: round-robin with a 10 ms quantum. The timer tick decrements `time_slice` and
  sets `need_resched` when it reaches zero. Interrupt handlers **never switch directly**. The
  switch happens on the return path when `preempt_count == 0`.
- **Context switch** (`switch_to`, about 20 lines of asm): push callee-saved registers, store
  `rsp` in `prev->ctx`, load `next->ctx.rsp`, pop, `ret`. The C wrapper switches `CR3` when
  the address space changes, updates `TSS.RSP0` and the per-CPU syscall stack, and
  saves/restores FPU state with `fxsave64`/`fxrstor64` for user tasks. Kernel code is built
  with `-mgeneral-regs-only` and never touches SSE.
- **Idle task**: `sti; hlt` loop, never on the run queue.
- **Blocking**: `wait_queue` with `wait_event(wq, cond)` / `wake_up(wq)`, written to avoid the
  lost-wakeup race (set state, check condition, then schedule). `sleep_ms()` uses a
  deadline-ordered sleep list checked on each tick.
- **Exit**: a task becomes a `ZOMBIE`, wakes its parent, and switches away for the last time.
  Its kernel stack and address space are freed by the **reaper** (parent `wait()` or the
  reaper thread for kernel threads), because a task cannot free the stack it is running on.
  Orphans are reparented to init.
- **Locks**: `spinlock_t` with `spin_lock_irqsave`. Taking a lock increments `preempt_count`.
  On one core, the lock body only needs IRQ-disable, but the API keeps SMP open.

### 5.8 Processes and syscalls (`proc/`, `arch/x86_64/syscall.asm`)

- **Images**: user programs are built as static ELF64 executables linked at `0x400000`,
  then embedded into the kernel as `.incbin` blobs with a name table.
- **Spawn**: create an address space, map `PT_LOAD` segments with exact permissions, map a
  user stack (64 KiB plus a guard page) near the top of the lower half, set up an initial
  `trap_frame`, and enqueue the task.
- **Entry**: `STAR`/`LSTAR`/`FMASK` (masking IF, DF, TF, AC). `swapgs`, then switch to the
  per-task kernel stack via `gs:` and build a `trap_frame`, so syscalls and interrupts share
  one return path.
- **Exit**: `sysret` only when the return `RCX` is canonical and no signal-like state is
  pending. Otherwise the generic `iretq` path runs (CVE-2012-0217 mitigation).
- **ABI**: `rax` = number, args in `rdi, rsi, rdx, r10, r8, r9`, return in `rax`, negative
  values are errno.

| # | Syscall | Notes |
|---|---------|-------|
| 0 | `exit(code)` | |
| 1 | `write(fd, buf, len)` | fd 1/2 → console; validates the buffer with `copy_from_user` |
| 2 | `yield()` | |
| 3 | `getpid()` | |
| 4 | `sleep_ms(ms)` | |
| 5 | `spawn(name)` | spawns an embedded image, returns its pid |
| 6 | `wait(pid, *status)` | reaps a zombie child |

- **User-pointer validation**: a range check (`addr + len` must not overflow and must stay
  below the user ceiling), plus the exception-table fixup for unmapped pages. **No syscall
  dereferences a user pointer directly.**

---

## 6. Engineering standards

**Compiler flags** (kernel). Each one is there for a reason:

```
--target=x86_64-unknown-none-elf -std=c17 -ffreestanding -nostdlib -fno-builtin
-mcmodel=kernel -mno-red-zone -mgeneral-regs-only -fno-pic -fno-pie
-fno-omit-frame-pointer -fno-strict-aliasing -fno-common -fno-asynchronous-unwind-tables
-Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla
-Wcast-align -Wpointer-arith -Wundef -Wimplicit-fallthrough
-ffile-prefix-map=$(SRC)=.            # reproducible builds
```

`-mno-red-zone` is the flag that most often causes silent corruption when forgotten:
interrupts clobber the 128 bytes below `rsp`. It is checked in CI by grepping the
compile commands.

**Code rules**
- Fixed-width types (`u8 … u64`, `uptr`) everywhere hardware layout matters, with
  `static_assert` on every hardware-defined struct size and offset.
- No magic numbers: `CR0_WP`, `EFER_NXE`, `PTE_NX` and the rest are named in `arch/` headers.
- Every asm routine opens with a register contract comment: inputs, outputs, clobbers,
  and the IF state it expects.
- Functions are `static` unless exported. Exported APIs carry a subsystem prefix (`pmm_`,
  `vmm_`, `sched_`).
- Errors are negative errno values. Fallible functions are `__must_check`.
- `assert()` (debug) and `BUG_ON()` (always) document invariants. No silent fallthrough
  on impossible states.
- Formatting comes from `.clang-format`, and lint from `.clang-tidy` with a curated check list.
  Both run in CI.

**Git hygiene**: one milestone per PR, Conventional Commits (`feat(mm): …`,
`fix(sched): …`), each commit builds and boots.

---

## 7. Testing strategy

Four layers, all run by `make test` and in CI:

| Layer | What | How |
|-------|------|-----|
| **Host unit tests** | Buddy allocator, `printf`, `string.c`, ELF parser, list/bitops, E820 sanitizer | Compiled for Linux with `-fsanitize=address,undefined`, plus randomized fuzz loops. |
| **In-kernel tests** | `KTEST(name) { … }` placed in a `.ktest` linker section | The test kernel (`cmdline=ktest`) runs every case and reports **TAP** over serial. |
| **QEMU integration** | Boot, scheduling, user-mode, fault isolation | `tests/qemu/run_tests.py` boots with `-device isa-debug-exit -serial stdio -display none -no-reboot`, enforces a timeout, and parses TAP. |
| **Negative tests** | Things that *must* fail safely | A write to `.text` faults. A kernel stack overflow reports `#DF` instead of triple-faulting. A user NULL deref kills only that process. A bad syscall pointer returns `-EFAULT`. |

**CI** (`.github/workflows/ci.yml`): format check → lint → build (debug + release) →
host tests → QEMU tests → upload `disk.img` and the serial log as artifacts.

**Debugging workflow**: `make debug` starts QEMU with `-s -S`, and `tools/gdb/.gdbinit` loads
kernel symbols and adds `ptwalk <va>` and `tasks` commands. When something
triple-faults, `-d int,cpu_reset -no-reboot` is the first step.

---

## 8. Milestones

Each milestone is one PR, merges green, and ends with a demonstrable, automated check.

| M | Milestone | Exit criteria (all automated) | Est.* |
|---|-----------|-------------------------------|-------|
| **M0** | Scaffolding | Makefile, CI, clang-format/tidy, host test harness, QEMU runner. CI is green on an empty kernel stub. | 2–3 d |
| **M1** | Bootloader | Disk image boots in QEMU. Stage 2 enters long mode and loads the ELF. The kernel prints the E820 map on serial and exits QEMU with success. | 5–8 d |
| **M2** | Kernel foundation | klog, panic with symbolized backtrace, GDT/TSS/IDT, all exception handlers, kernel UBSan. KTESTs: `#DE`, `#UD`, `#BP` caught and decoded. `#DF` lands on its IST stack. | 4–6 d |
| **M3** | Physical memory | E820 sanitizer and buddy allocator, host-fuzzed. KTEST: allocating all memory, freeing it, and re-verifying gives identical stats. | 3–4 d |
| **M4** | Virtual memory | Own page tables, HHDM, W^X, NX/WP/SMEP/SMAP, guard pages, vmalloc, kmalloc. KTESTs: a write to `.text` faults, a stack overflow hits the guard page, a NULL deref faults. | 5–7 d |
| **M5** | Interrupts and time | PIC remap, PIT 1000 Hz, spurious-IRQ handling, `sleep_ms` primitives. KTESTs: the tick advances at the programmed rate (±2% against TSC, KVM-only; TCG checks monotonicity), IRQ masking works. | 2–3 d |
| **M6** | Scheduler | Kernel threads, preemptive RR, wait queues, sleep, exit and reap. KTESTs: a busy-looping thread is preempted, 4 CPU-bound threads stay within ±10% of fair share, no lost wakeups under stress. | 6–9 d |
| **M7** | User mode | Ring 3, per-process address spaces, ELF loader, syscall/sysret, the 7 syscalls, fault isolation. Tests: `init` spawns children and reaps them, a faulting child is killed while the kernel and its siblings keep running. | 7–10 d |
| **M8** | v1.0 polish | Design docs per subsystem, README with an architecture diagram and boot log, benchmarks (context switch, syscall round-trip, page alloc), labeled as TCG or KVM. Tag `v1.0.0`. | 3–4 d |

\*Estimates are a guess for one focused developer who already knows C and has some OS
background, about **6–9 weeks** in total. Expect roughly double for a first kernel. M1 and
M6 carry most of the schedule risk.

### Stretch goals (post-v1.0, in priority order)
1. LAPIC timer (PIT-calibrated) + IOAPIC via ACPI MADT (done, HPET-calibrated; see §11)
2. Priority scheduling / MLFQ, with latency benchmarks against RR
3. SMP bring-up (INIT-SIPI-SIPI, per-CPU run queues, real spinlocks)
4. Copy-on-write `fork()`
5. initrd + a minimal VFS, so programs no longer need embedding
6. UEFI loader producing the same `boot_info` (done; see §11)
7. KASLR for the kernel image

---

## 9. Risk register

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Bootloader bugs triple-fault with no output | High | High | Single-letter progress codes on screen/serial at each stage. QEMU `-d int,cpu_reset`. Keep stage 2 minimal. Do M1 early, while nothing else depends on it. |
| BIOS INT 13h quirks (127-sector cap, 64 KiB segment crossing) | Medium | Medium | Chunked reads aligned to never cross a 64 KiB boundary. Tested with a kernel image larger than 1 MiB. |
| Red zone or SSE use in the kernel corrupts state | Medium | High | `-mno-red-zone -mgeneral-regs-only`, verified in CI. `objdump` check for `xmm` in kernel `.text`. |
| Trap frame or asm offsets drift from C structs | Medium | High | `asm-offsets.c` generation plus `static_assert`. One definition of every layout. |
| A task frees its own kernel stack on exit | Medium | High | Zombie plus reaper design (§5.7). KTEST that spawns and reaps 10k threads. |
| `sysret` with non-canonical RCX → ring-0 #GP on user stack | Low | Critical | Canonical check, `iretq` fallback (§5.8). |
| Kernel mappings diverge between address spaces | Medium | High | Upper-half PDPTs allocated at boot and shared (§5.5). |
| **Scope creep** ("make it more complicated") | **High** | **High** | Non-goals are written down (§1). New features go into Stretch goals, not the current milestone. |

---

## 10. Definition of done (v1.0)

- [x] `make run` boots to a user-space `init` that spawns demo processes, which then print,
      sleep, fault, and exit, while the kernel stays up. (Itself tested: `disk-demo.img`.)
- [x] `make test` runs host, in-kernel, and QEMU suites. All are green in CI on every commit.
- [x] No warnings at `-Wall -Wextra -Werror`. clang-tidy is clean. Formatting is enforced.
- [x] Every subsystem has a design doc that states its invariants and the reasons behind
      its choices.
- [x] Every hardware-facing struct has `static_assert`ed layout. Every asm routine has a
      register contract.
- [x] README: a one-paragraph pitch, a quickstart (3 commands), the architecture diagram,
      the memory map, a test matrix, benchmarks, and known limitations.
- [x] Tag `v1.0.0`: on `8cf9fa5`, the merge of M0–M8, tagged when v1.1.0 was released.
      Its boot banner still reads `minios 0.1.0`: the version string was not raised
      until v1.1.0.

---

## 11. How the build differed from the plan

Recorded so the plan stays honest. Each item is a deliberate change, a cut,
or something the implementation taught us.

**Changed during implementation**

| Plan said | Built | Why |
|-----------|-------|-----|
| Stage 2 goes 16 → 32 → 64-bit | real mode → long mode in one step | Setting CR0.PE and CR0.PG together with EFER.LME is architecturally valid and removes an entire 32-bit stage. |
| `boot_info.inc` generated from the header | `asm-offsets.inc`, one generated file | The same mechanism also serves trap-frame, task and per-CPU offsets. |
| PMM placed metadata in the 1 GiB boot window | metadata behind the direct map | M4 brought the direct map up before the PMM, which lifted the 1 GiB limit. The suite now also runs with 4 GiB. |
| `init` from the start, 7 syscalls | 8 syscalls (`time_ms` added) | User programs needed a clock for the spinner and sleeper tests. |
| Tick rate within ±2% of the TSC (KVM only) | PIT divisor verified exactly by counter read-back; 100 ticks within 50–400 ms of TSC time (8259 path) or 94–400 ms of HPET time (HPET reference). Separately, the TSC rate against the test's own PIT channel 2 window, and the LAPIC timer rate against the TSC, each within 2% plus the measured uncertainty of the endpoints | Counting ticks measured emulator timing (100 ticks took 91.6–102.6 ms, and well over 130 ms on a loaded host); the read-back tests what the kernel actually controls. Clock-against-clock checks need no interrupt to arrive on time, so they can be tight. |
| Scheduler fairness within ±10% | CPU time in ticks within one slice | Loop counts measured the emulator: ±25% of them failed once on a cold 4 GiB run while the ticks were exactly equal. Ticks are what round robin divides, so the check is now tight: one slice, plus two ticks for the threads' exit runs. |
| "SYSRET hazard → #GP in ring 0" test | accepts #GP (hardware) or user #PF (TCG); fixup tested with a bad SS | QEMU 8.2's TCG does not check RIP canonicality in `iretq` (9.2's does, so the x2APIC runs take the real `#GP` path); the bad-SS frame exercises the same recovery path on every CPU. |
| IST stacks, "each with its own guard page" (§5.3) | three adjacent 16 KiB stacks in `.bss`, no guard pages | The goal, a clean `#DF` on a kernel stack overflow, comes from the guard pages under every vmalloc'd task stack (tested). An overflow of one IST stack itself would run into the next. |
| A reaper thread frees kernel threads; orphans go to init (§5.7) | detached threads are freed by the next task to run (`finish_switch`), others by their parent's `wait()`; orphans go to `kmain`, the boot task, which reaps whatever it is given | No thread needed: the first moment a dead task's stack is provably unused is the switch away from it. `kmain` already loops on `wait`. |
| Kernel stack and address space freed by the reaper (§5.7) | address space and FPU area freed at exit; only the task struct and kernel stack wait for the parent's `wait()` (or, for a detached thread, the next switch) | A parent that never waits must not pin its children's memory. |
| `FMASK` masks IF, DF, TF, AC (§5.8) | `FMASK` clears every flag ring 3 can set, NT and AC included | A user-set NT survived `SYSCALL`, reached another task's `iretq` and raised `#GP` there (see below). |
| A user-mode fault kills the process (§5.3) | except NMI (counted, the process continues) and `#MC` (panics) | Neither is caused by the running process. |

**Planned but not built**

| Item | Status |
|------|--------|
| klog ring buffer | Built after v1.0, with the screen console: a 16 KiB ring that the screen is fed from (it replays the boot) and that `console_log_read()` exposes. |
| kmalloc redzones | Built after v1.0: debug builds put >= 8 guard bytes and a hidden size after every small object; `kfree()` reports overflows with the offending offset. |
| gdb helpers (`ptwalk`, `tasks`) | Built after v1.0 as `minios-tasks` and `minios-ptwalk` (`tools/gdb/minios.py`), tested against a live kernel by `make gdb-test`. |
| ADR files in `docs/adr/` | Not written. Rationale lives in §2 and the design docs. |
| VGA text as a mirror of serial (§1, §5.2) | Not in v1.0. Built after v1.0 as the screen console (below), which also draws on UEFI framebuffers. |
| klog tick timestamps (§5.2) | Not built: messages carry a level tag only. |
| A page-table walk in `#PF` panics (§5.3) | Not built. The panic decodes CR2 and the error bits; `minios-ptwalk` (gdb) walks the tables of a live kernel. |
| `might_sleep()` (§5.6) | Not built. `schedule()` panics when called while atomic or from an IRQ handler instead. |
| A kernel image larger than 1 MiB to test INT 13h chunking (§9) | Not done. The stripped kernel is about 520 KiB in debug and 330 KiB in release, and nothing pads it. Loads do span many 32 KiB chunks. |
| A KTEST that spawns and reaps 10k threads (§9) | Scaled down: 200 threads after a 200-thread warm-up round, and 40 processes, each run required to give back every frame. |
| `-mno-red-zone` checked in CI | Done differently: the Makefile refuses to build without `-mno-red-zone`, `-mgeneral-regs-only` and `-mcmodel=kernel`, and `check_kernel.py` rejects vector registers in the binary. |

**Built after v1.0** (beyond the original scope; released as v1.1.0)

| Item | Notes |
|------|-------|
| UEFI loader | Promoted from stretch goal 6. Same `boot_info` hand-off as the BIOS path (version 2 adds `loader`), so the in-kernel suite and the init demo also run under OVMF in `make test`. |
| Screen console | The plan had only a VGA text mirror of serial, and v1.0 shipped without it. Real machines rarely have a serial port, so a first hardware boot would show nothing. UEFI GOP framebuffer (write-combining through the PAT) or BIOS VGA text, both on a pure, host-fuzzed text grid. Drawing never happens with interrupts off once they run. See [design/console.md](design/console.md). |
| LAPIC timer + I/O APIC via the MADT (stretch goal 1) | Calibrated against the HPET rather than the PIT when there is one, with PIT channel 2 as the fallback reference and the whole 8259 + PIT path kept behind `noapic`. ACPI table parsing is pure and fuzzed on the host. `boot_info` version 3 carries the RSDP from UEFI. Since hardened for real hardware, though never run on it: an x2APIC hand-off is kept, every calibration wait is bounded and a dead HPET falls back to the PIT (no test reaches these three), and no working clock is a clear panic (the `pit=off,hpet=off` run boots into it; `pit=off` shows the APIC path needs no PIT). The `x2apic` option switches to x2APIC mode, and `make test-x2apic` runs the whole in-kernel suite that way (debug build) under QEMU 9 (CI builds 9.2.0; Ubuntu 24.04's 8.2 cannot emulate x2APIC under TCG). See [design/interrupts.md](design/interrupts.md). |
| Runner self-tests | `check_runner.py` boots kernels that hang and triple-fault, and requires `run_tests.py` to reject them. |

**Learned the hard way** (each one is now covered by a test or a check)

- The early page-table pool sits in the kernel image, whose direct-map
  alias is read-only by design. Writing tables through the direct map
  faulted on the first M4 boot.
- An out-of-bounds UBSan test wrote past a stack array; the stack
  protector caught the smashed canary. Both nets fired, in order.
- Concurrent QEMU runs contended for the disk image's write lock, which
  looked like a kernel crash (status 1, empty log). The runner now uses
  `-snapshot` and prints QEMU's stderr.
- Generated files that skipped rewriting identical output stayed older
  than their inputs, so the build never reached a no-op state. They now
  always write.
- Freed task structs read by the tests themselves were caught by the
  kmalloc poison (`0x6b6b6b6b` where a pid was expected).
- A PIT test sampled a fixed number of times and assumed that spanned half
  a timer period; the faster CI runner disproved it. It then sampled
  across two ticks, which a loaded host (vCPU descheduled for most of
  them) disproved in turn. It now samples until the range is covered,
  with a deadline.
- NASM 2.16's `-MD` lists only the source file, not its `%include`s, so
  changing `boot_info` did not rebuild stage 2 (the kernel then refused
  the stale loader's version). Every assembly file had the same gap.
  Dependencies now come from a separate `-M` pass, and touching
  `asm-offsets.inc` was checked to rebuild them all.
- Drawing the console inside its lock, with interrupts off, took up to
  8.4 ms per line on a framebuffer once it scrolled (measured under TCG):
  enough to lose timer ticks, which are the clock. The screen is now fed
  outside the lock, and a test proves no render runs with interrupts off.
- Stress-running the APIC tests (8 QEMUs at once) failed two of them 1 run
  in 12. Both asserted emulator timing, not kernel behavior. Register
  read-back is now the exact check, and rate checks only catch factor
  errors.
- The UEFI loader keeps running across its own CR3 switch, and its tables
  identity-mapped only 0-4 GiB, so it refused to run from anywhere higher.
  OVMF loads it at 5 GiB on a machine with 8 GiB of RAM. The loader now
  identity-maps its own image and stack wherever they are, and `make test`
  boots the UEFI demo with 8 GiB.
- The two calibration rate tests (below) failed 4 of 16 runs under 8-way
  parallel load. The tests were wrong, not the kernel: one bound of the PIT
  edge was read on the wrong side of a poll, and the LAPIC reads were not
  bracketed. Both are now bracketed, and the tolerance includes the
  measured uncertainty.
- The UEFI loader claimed physical page `0x1000` for `boot_info`, and the
  kernel accepted it nowhere else. Low pages are what UEFI firmware tends
  to keep, so the loader now takes any free page below 1 GiB and the
  kernel copies `boot_info` at entry.
- The scheduler fairness test (from M6) compared loop counts, and failed
  once on a cold 4 GiB release run: under emulation, iterations per tick
  follow the host. It now compares CPU time in ticks, which round robin
  divides exactly (instrumented runs: 110 ticks for each of four threads).

**Found by an adversarial audit** of the code after the APIC work. Each fix
has a test:

- A ring-3 program could set RFLAGS.NT, which `FMASK` did not clear. The
  kernel does not save RFLAGS across context switches, so the flag reached
  another task's `iretq`, which raised `#GP`: other processes died, or the
  kernel panicked. `FMASK` now clears every flag ring 3 can set (`flagbomb`).
- AC set in ring 3, or by a user copy an interrupt landed in, stayed set in
  the interrupt handler and in any task it switched to, with SMAP off.
  `trap_dispatch()` now runs `clac` when the CPU has SMAP.
- An interrupt could wake the very task it interrupted, between
  `sched_prepare_wait()` and the condition check, and queued it although it
  was running. Had it then exited, its zombie would have been scheduled.
  `wake_locked()` no longer queues the running task.
- The NMI handler printed, so an NMI landing inside the console lock
  panicked the kernel (recursive lock). It now only counts, and the next maskable interrupt reports.
- Two tests could not fail. The loader IF/DF check read RFLAGS after
  `_start`'s own `cli; cld`. The churn tests allowed a few frames of slack,
  which hid that vmalloc never reused VA and so leaked page tables, about
  one frame per 100 tasks. `_start` now saves RFLAGS first, vmalloc reuses
  freed ranges, and the churn tests require every frame back.
- An injected 2x calibration error passed every other test. Two tests now
  measure the TSC against their own PIT channel 2 window, and the LAPIC
  timer against the TSC, within 2% plus the measured endpoint uncertainty.
- SeaBIOS hands over with A20 already on, so none of stage 2's three A20
  methods ever ran under test, and deleting the call passed CI. Three test
  builds of stage 2 now turn A20 off, check that addresses wrap, and let
  only one method turn it back on; `make test` boots each.
- OVMF leaves values in the XMM registers, and the FPU template every
  process starts from was a snapshot of them. BIOS boots also ran with
  `CR0.NE` clear, so x87 errors never raised `#MF`.
- `sleep_ms(1)` could return 130 µs after it was called. Sleeps now add a
  tick, so the requested time is a minimum.
