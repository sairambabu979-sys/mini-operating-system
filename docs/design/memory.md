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

`pmm_init()` sizes the page array for the highest usable frame and places
it in the first usable RAM above the kernel image. It then gives the
allocator every usable frame except three reserved ranges:

| Range | Why |
|-------|-----|
| `0x0 – 0x100000` | IVT, BIOS data, EBDA, `boot_info`, boot page tables, loader, ROM shadows |
| kernel image | `[kernel_phys_start, kernel_phys_end)` from `boot_info` |
| page array | the allocator's own metadata |

The raw ELF staging area at 16 MiB is *not* reserved: by then the kernel
has been copied out of it, so it is ordinary free memory.

Physical address 0 can never be allocated, so `0` doubles as the failure
value (`PMM_ALLOC_FAILED`). An invalid free (double free, wrong order,
interior frame) is always a bug, so it panics.

The array covers every frame up to the highest usable one, holes
included. With 4 GiB of guest RAM, QEMU puts 1 GiB above the PCI hole, so
the array spans 5 GiB of frames (30 MiB). A sparse-memory model would save
that, at the cost of a lookup on every frame-to-page conversion. Not worth
it at this scale.

## Page tables (VMM)

**Bootstrapping without an allocator.** The page tables are built before
the PMM exists, from a 32-page pool in `.bss`. The direct map is the only
way to reach arbitrary frames, and it cannot exist until those tables do.
Pool frames belong to the kernel image, so they are always written through
the (writable) `.bss` mapping, never through their read-only direct-map
alias. The first boot of M4 faulted exactly there, which showed the alias
protection works.

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

## vmalloc

```
[guard][area 0 ...][guard][area 1 ......][guard][area 2] ...
```

Virtual addresses come from a bump pointer and are **never reused**:
- 512 GiB is room for millions of kernel stacks,
- a dangling pointer into a freed area always faults instead of aliasing
  a new allocation,
- an SMP kernel would otherwise need TLB shootdowns on reuse.

An area needs no metadata: it ends at the next unmapped page. `vfree()`
of anything but an area's first page panics.

## kmalloc

Size classes 16 B – 2 KiB use one-page slabs with a 64-byte header. Larger
requests take whole buddy blocks through the direct map. Slab objects are
never page-aligned, while large allocations always are, so `kfree()` tells
them apart with no extra metadata. Empty slabs go back to the PMM, except
one kept warm per class.

Debug builds add three checks:
- freed objects are filled with `0x6B`, and the poison is verified on reuse
  (use-after-free writes panic),
- free lists are scanned for double frees,
- the check runs *before* unlinking, so a detected corruption leaves the
  slab consistent.

## Locking

None yet. Nothing runs concurrently until interrupt handlers and threads
arrive (M5/M6); the allocator locks come in the same change.

## Tests

| File | Covers |
|------|--------|
| `tests/host/test_memmap.c` | sorting, merging, overlap resolution, page shrinking, unknown types, top-of-address-space wrap, randomized brute-force check |
| `tests/host/test_buddy.c` | carving, split/merge round trip, alignment and disjointness, exhaustion, bad frees, reserved islands, 20k-step fuzz |
| `tests/kernel/test_pmm.c` | consistency after init; managed = usable minus known reservations; allocations avoid reserved memory; drain-and-restore through every frame; double free panics |
| `tests/kernel/test_vmm.c` | each forbidden access attempted and its `#PF` decoded; translate permissions; map/unmap round trip; guard pages; **a real kernel stack overflow** caught as `#DF` on IST1 with CR2 in the guard |
| `tests/kernel/test_kmalloc.c` | alignment, disjointness, reuse, large allocations, `kcalloc` overflow, stats; double free, use after free and foreign-pointer frees (via `panic_expect()`) |

The suite runs with 128 MiB and with 4 GiB of guest RAM. The 4 GiB run puts
memory above the PCI hole, so the sanitizer, direct map and buddy allocator
are exercised on a sparse map.
