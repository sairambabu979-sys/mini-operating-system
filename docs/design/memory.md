# Memory management

Sources: `kernel/mm/`, `kernel/include/mm/`.

## Layers

```
 kmalloc / vmalloc          small objects, guarded kernel stacks
 VMM: page tables           4-level paging, direct map, W^X
 PMM: pmm_alloc_pages()     owns every physical frame
   └─ buddy allocator        pure algorithm, host-tested
 memmap_sanitize()          firmware map -> sorted disjoint regions
```

Bring-up order (`mm/init.c`), each step needing the previous one:

1. `memmap_sanitize`: pure, reads only `boot_info`.
2. `vmm_init`: builds page tables from a static pool, switches CR3, and
   brings the direct map live.
3. `pmm_init`: its metadata is reached through the direct map.
4. `vmm_init_late`: PMM-backed tables for the rest of the kernel half,
   which is then frozen.

## Virtual address space

| Range | PML4 | Contents |
|-------|------|----------|
| `0x0000000000000000 – 0x00007FFFFFFFFFFF` | 0–255 | user space (per process, M7) |
| `0xFFFF800000000000` + 512 GiB | 256 | direct map (HHDM) of physical RAM |
| `0xFFFFC00000000000` + 512 GiB | 384 | vmalloc: guarded kernel stacks |
| `0xFFFFFFFF80000000` – top | 511 | kernel image (`-mcmodel=kernel`) |

## Sanitizing the firmware map

BIOS E820 maps may be unsorted and overlapping, and their boundaries may
not be page-aligned. `memmap_sanitize()` sweeps every entry boundary.
Between two consecutive cut points the most restrictive type wins:
usable < ACPI reclaimable < ACPI NVS < reserved < bad, and unknown types
count as reserved. Adjacent regions of the same type merge, and usable
regions shrink inward to whole pages. Gaps the firmware does not describe
are never treated as memory. The host tests include a randomized check of
the output (sorted, disjoint, page-aligned) against a brute-force answer
for every region.

## Buddy allocator

Blocks of 2^k frames (k ≤ 10, so up to 4 MiB), naturally aligned. A
block's buddy differs only in bit k. Freeing merges with a free buddy
repeatedly, so **the free lists never contain both halves of a free
block**. `buddy_verify()` checks this along with every other invariant.

All bookkeeping lives in a `struct page` array (24 bytes per frame), never
inside the free frames:
- memory that no page table maps can still be managed,
- a stray write into a free page cannot corrupt the allocator,
- the whole algorithm runs on the host under ASan/UBSan, including a
  20,000-step randomized alloc/free fuzz with full verification.

The tests were mutation-checked: deliberately breaking the merge loop or
the merge address is caught by the verifier or by ASan.

## PMM

`pmm_init()` sizes the page array for the highest usable frame below
512 GiB and places it in the first usable RAM above the kernel image that
is clear of the initrd. It then gives the allocator every usable frame
below 512 GiB except these reserved ranges:

| Range | Why |
|-------|-----|
| `0x0 – 0x100000` | IVT, BIOS data, EBDA, the BIOS loader's `boot_info`, boot page tables, loader, ROM shadows |
| kernel image | `[kernel_phys_start, kernel_phys_end)` from `boot_info` |
| initrd | `[initrd_phys, initrd_phys + initrd_size)` page-rounded, valid archive or not: the loaders leave it in pages the map calls usable (see [boot.md](boot.md#the-initrd)) |
| page array | the allocator's own metadata |

The kernel works from its own copy of `boot_info`, made at entry, so the
UEFI loader's `boot_info` page (anywhere below 1 GiB) needs no
reservation: it is ordinary RAM.

The raw ELF staging area at 16 MiB is *not* reserved: by then the kernel
has been copied out of it, so it is ordinary free memory, except for the
initrd stage 2 leaves right after the ELF, which is reserved as above.

Physical address 0 can never be allocated, so `0` doubles as the failure
value (`PMM_ALLOC_FAILED`). An invalid free (double free, wrong order,
interior frame) is always a bug, so it panics.

The array covers every frame up to the highest usable one, holes
included. With 4 GiB of guest RAM, QEMU puts 1 GiB above the PCI hole, so
the array spans 5 GiB of frames (30 MiB). A sparse-memory model would save
that, at the cost of a lookup on every frame-to-page conversion. Not worth
it at this scale.

**Only RAM below 512 GiB.** Every frame the PMM hands out is used through
the direct map, which is 512 GiB long, so frames above it are not managed
at all: the page array stops there, and `pmm_init()` warns how much RAM
it left out (`pmm: <n> MiB of RAM above the 512 GiB direct map left
unused`). Without the clamp such frames were handed out, and the first
use through the direct map faulted.

**Owner tags.** The head `struct page` of each block carries a 16-bit
`owner`, reset to `PMM_OWNER_NONE` (0) on every allocation and free.
`pmm_set_owner()` tags an allocated block, and `pmm_block_owner()` reads
the tag back (-1 if the address is not the head of an allocated block).
kmalloc tags its large blocks `PMM_OWNER_KMALLOC` and its slab pages
`PMM_OWNER_SLAB`, so that `kfree()` can refuse memory it did not hand
out, such as a page table or a user frame. For a pointer inside a page it
checks the tag before reading the slab header: the header is only bytes
on the page, and a user frame can hold an exact copy of one. The tag fits
in the 24-byte `struct page`.

## Page tables (VMM)

**Bootstrapping without an allocator.** The page tables are built before
the PMM exists, from a 32-page pool in `.bss`. The direct map is the only
way to reach arbitrary frames, and it cannot exist until those tables do.
Pool frames belong to the kernel image, so they are always written through
the (writable) `.bss` mapping, never through their read-only direct-map
alias. The first boot of M4 faulted exactly there, which showed the alias
protection works.

The pool covers the kernel image plus the whole 512 GiB direct map when
the CPU has 1 GiB pages. Without them the direct map needs one page
directory per GiB, and the pool covers only about 24 GiB of RAM. Running
out is a panic that says so (`vmm: early page-table pool exhausted (32
pages): without 1 GiB pages, about 24 GiB of RAM fit`), not corruption.

**Kernel image.** Each section class gets its own 4 KiB mappings, matching
the PT_LOAD segments `tools/check_kernel.py` verifies:
`.text` R-X, `.rodata` R--, `.data/.bss` RW-, all global.

**Direct map.** Only firmware-described RAM (usable, ACPI reclaimable,
ACPI NVS) is mapped. Reserved ranges and holes stay unmapped, because a
write-back mapping over MMIO invites speculative device accesses. The map
uses 1 GiB / 2 MiB pages wherever alignment allows. Two more rules keep it
from becoming a W^X bypass:
- it is never executable,
- the kernel image's frames are mapped read-only in it.

**CPU protections.** `EFER.NXE` (set before any table using bit 63 is
loaded), `CR0.WP` (ring 0 honours read-only pages), and `CR4.PGE`,
`CR4.SMEP` and `CR4.SMAP` when supported.

**Shared kernel half.** `vmm_init_late` gives every kernel-half PML4 entry
(256–511) a PDPT up front, about 1 MiB in total. After that the entries
never change (`walk()` panics if one would be created), so a process's
address space shares the kernel by copying 256 entries once, with no
synchronization later.

**W^X at the API.** `vmm_map()` rejects `VM_WRITE | VM_EXEC`.

**A failed `vmm_map()` maps nothing.** If a page is already mapped
(`-EEXIST`) or a page table cannot be allocated (`-ENOMEM`) part-way, the
pages that call mapped are taken back; the page tables it created stay,
since walkers read kernel tables without a lock. A range that wraps,
leaves the canonical addresses or crosses `KERNEL_HALF` is `-EINVAL`
before anything is mapped: one running from the last user page into the
kernel half would otherwise write the kernel tables every process shares.

## vmalloc

```
[guard][area 0 ...][guard][area 1 ......][guard][area 2] ...
```

Virtual addresses come from a bump pointer. A freed area's range is
reused, but only by an allocation of exactly the same number of pages,
and only once 16 newer freed ranges wait behind it
(`VMALLOC_QUARANTINE`). Until then a dangling pointer into a freed area
keeps faulting instead of aliasing a new allocation. Freed ranges wait in
a FIFO ring of 512 slots (`VMALLOC_REUSE_SLOTS`), and an allocation takes
the oldest eligible one before it bumps the pointer. When the ring is
full, the oldest range is dropped and its VA is never used again.

Reuse is what bounds the page tables. `vfree()` never frees a page table,
because `vmm_translate()` walks kernel tables without a lock. With fresh
VA for every kernel stack, as in the first version, each task creation
slowly cost page-table memory for good: about one frame per 100 tasks,
which any user program that spawns in a loop could drive. Kernel stacks
are the only regular vmalloc users, and the scheduler checks at build time
that `TASK_MAX` plus the quarantine fits in the ring, so stack VA stays
bounded. (On SMP, reuse will also need a TLB shootdown.)

An area needs no metadata: it ends at the next unmapped page. `vfree()`
of anything but an area's first page panics. A failed `vmalloc()` or
`iomap()` gives its frames back and its VA range too: to the bump pointer
if it was the last range taken from it, otherwise (a range from the reuse
ring, as kernel stacks are once tasks have exited) to the ring's oldest
end, where it ages no freed range: at the newest end, sixteen failed
allocations would make a just-freed stack reusable at once. Abandoned
ranges cost little address space (the region is 512 GiB) but spread
later allocations over fresh page tables, which are never freed.

The price of reuse: a double or stale `vfree()` panics only while its
range is quarantined. Once 16 newer ranges were freed, the range may
belong to a new owner, and the stale call frees that owner's area
instead. Two tests pin the ring down: a range comes back only after
exactly 16 newer frees, and a reused range leaves the ring (the oldest
range of its size can sit behind others, and one still listed would go to
a second owner).

**`iomap()`** maps physical ranges the direct map leaves out (device
registers, framebuffers, firmware tables in reserved memory) into the same
region, behind the same guard pages, never executable. Memory types:
`VM_NOCACHE` is strong uncached, and `VM_WC` write-combining. For WC the
kernel reprograms PAT entry 1 from write-through, which nothing used.
Two rules:
- An uncached or WC request over RAM the direct map already caches is
  refused. Mixed memory types for one frame are undefined (SDM 12.12.4).
- Its PTEs carry a software-available bit, and `vfree()` panics on it.
  An iomap area has the same shape as a vmalloc area, and freeing one
  would hand device memory to the PMM.

## kmalloc

Size classes 16 B – 2 KiB use one-page slabs with a 64-byte header. Larger
requests take whole buddy blocks through the direct map, so the largest
allocation is one block of the highest order: `KMALLOC_MAX`, 4 MiB. Any
larger size returns NULL. The check comes first, which also keeps the
arithmetic honest: without it, a size near `SIZE_MAX` plus the debug
redzone overhead wrapped to a 16-byte object, and the block-order search
never finished for sizes above 2^63.

Slab objects are never page-aligned, while large allocations always are,
so `kfree()` tells them apart with no extra metadata. A page-aligned
pointer must be the head of a live PMM block tagged `PMM_OWNER_KMALLOC`;
anything else panics before the block is touched. Empty slabs go back to
the PMM, except one kept warm per class.

Debug builds add three checks:
- freed objects are filled with `0x6B`, and the poison is verified on reuse
  (use-after-free writes panic),
- free lists are scanned for double frees,
- every small object gets a **redzone**: at least 8 `0xCC` bytes between
  the requested size and a hidden `u16` size at the object's end, checked
  by `kfree()`, so a write past the end panics with the exact offset,
- the check runs *before* unlinking, so a detected corruption leaves the
  slab consistent.

## Locking

Each allocator has its own spinlock, always taken with
`spin_lock_irqsave`. An interrupt cannot arrive while one is held, so the
allocators may be called from IRQ handlers as well as from tasks (not
from the NMI handler: an NMI can arrive while a lock is held).

| Lock | Where | Protects |
|------|-------|----------|
| `kmalloc` (`heap_lock`) | `kmalloc.c` | all slab state; large allocations take only `pmm` |
| `vmalloc` (`va_lock`) | `vmalloc.c` | the VA bump pointer and the ring of freed ranges; an area, once reserved, belongs to its caller |
| `kernel page tables`, `user page tables` | `vmm.c`, one per address space | that address space's tables, in `vmm_map()` and `vmm_unmap_page()` |
| `pmm` (`pmm_lock`) | `pmm.c` | every buddy operation, owner tags included |

The order, outer first, is `kmalloc`, `vmalloc`, page tables, `pmm`. Only
two nestings occur, both ending in `pmm`: `kmalloc` takes it when a slab
grows or shrinks, and a page-table lock takes it for a new table.
`vmalloc` holds its lock only around its own state, and maps pages after
dropping it. So nothing may call `kmalloc` or `vmalloc` while holding a
page-table or `pmm` lock. kmalloc's misuse checks and the PMM's bad-free
check drop their lock before panicking, so a panic caught in a test
leaves the allocator usable. `vmm_translate()` takes no lock: it reads
entries with single aligned loads, and kernel-half tables are never
freed. The full table, with the scheduler, I/O APIC and console locks,
is in [traps.md](traps.md).

## Tests

| File | Covers |
|------|--------|
| `tests/host/test_memmap.c` | sorting, merging, overlap resolution, page shrinking, unknown types, top-of-address-space wrap, randomized brute-force check |
| `tests/host/test_buddy.c` | carving, split/merge round trip, alignment and disjointness, exhaustion, bad frees, reserved islands, 20k-step fuzz |
| `tests/kernel/test_pmm.c` | consistency after init; managed = usable RAM below the 512 GiB direct map minus known reservations; allocations avoid reserved memory, the initrd included; drain-and-restore through every frame, none of them in the initrd; double free panics |
| `tests/kernel/test_vmm.c` | each forbidden access attempted and its `#PF` decoded; translate permissions; map/unmap round trip; guard pages, and a freed area that faults; failures part-way through `vmm_map()`, `vmalloc()` and `iomap()` (with `pmm_test_fail_after()` making the allocator fail on cue) leave nothing mapped and hand back frames and addresses; ranges that leave their half refused; **a real kernel stack overflow** caught as `#DF` on IST1 with CR2 in the guard |
| `tests/kernel/test_kmalloc.c` | alignment, disjointness, reuse, large allocations, `kcalloc` overflow, stats; sizes above `KMALLOC_MAX` and near `SIZE_MAX` refused; foreign-pointer frees, including a live PMM block kmalloc did not allocate, which stays allocated (via `panic_expect()`); in debug builds only, double free, use after free and heap overflow |
| `tests/kernel/test_sched.c`, `test_user.c` | after a warm-up, 200 threads and 40 processes created and reaped give back every frame, with no slack (before VA reuse, the tests allowed 8 and 4 frames for vmalloc's page tables); reaping 8 zombies frees at most 5 frames each, a kernel stack and a task struct |

The suite runs with 128 MiB and with 4 GiB of guest RAM. The 4 GiB run puts
memory above the PCI hole, so the sanitizer, direct map and buddy allocator
are exercised on a sparse map.
