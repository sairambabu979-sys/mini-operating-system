# Console

Where kernel output goes, and how it reaches a screen without costing the
clock ticks. Sources: `kernel/core/printk.c`, `kernel/drivers/screen.c`,
`kernel/lib/{textgrid,fbrender,font8x8}.c`.

## Why a screen at all

COM1 was the only console. Under QEMU that is all anyone needs, but most
real machines, laptops above all, have no serial port: a boot that worked
perfectly would show a blank screen. The screen console makes the first
real-hardware boot observable, including a panic.

| Loader | Screen | How |
|--------|--------|-----|
| UEFI | GOP framebuffer | The loader reports the firmware's current mode in `boot_info.framebuffer` (version 4). The kernel draws an 8x8 font as 8x16 cells (16x32 from 1920 pixels wide). |
| BIOS | VGA text mode | If the BIOS data area says the display is in text mode (2, 3 or 7). The kernel adopts the screen as the BIOS left it, including the loader's progress line and the cursor position. |

`console=serial` turns the screen console off.

## Output path

```
kprintf / write()
   │  under the console lock (interrupts off): the log ring (16 KiB) and COM1
   ▼
message_done ── interrupts on, or still booting, or panicking? ── no ──► left in the ring
   │ yes                                                                  │
   ▼                                                                      │
feed_screen: one context at a time; loops until caught up ◄──────────────┘
   │  (next printer with interrupts on, or the idle loop)
   ▼
textgrid_putc (memory only) … then one textgrid_render (draws the diff)
```

**The ring.** Every byte goes into a 16 KiB ring as well as to COM1. The
screen needs the memory manager (`iomap`, `kmalloc`), so it registers
after the first messages. It is then fed the whole ring, and shows the
boot from its first line. The ring wraps mid-line; the replay starts at
the next full line. `console_log_read()` exposes it, for the tests now
and for a `dmesg` later.

**Never draw with interrupts off** once the system is live. Measured
under QEMU (TCG), drawing synchronously inside the console lock took up
to **8.4 ms per line** on a 1280x800 framebuffer once it scrolled, all of
it with interrupts off. The kernel's clock counts ticks, so every scroll
lost several of them. Now:
- Bytes are logged under the lock. The screen is fed outside it.
- Output produced with interrupts off (IRQ handlers, code holding a
  spinlock) is only logged. The next printer with interrupts on draws it,
  or the idle loop does (`console_drain()`).
- One context feeds the screen at a time. Another that finds it busy
  leaves its bytes to the feeder, which loops until it has caught up.
  Nothing is dropped, and nobody waits.
- Before interrupts are enabled (boot), drawing is immediate, so a hang
  in early boot is visible. In a panic it is immediate too, with no lock.

`screen_renders_with_irqs_off()` counts violations. A test requires it
to be 0 after the whole boot and the entire test suite.

**Deferred rendering.** `textgrid_putc` only updates the `cells` array.
`textgrid_render` then compares `cells` row by row against `shown` (what
the display last received) and draws only the cells that differ:
- A burst of output that scrolls twenty times costs one redraw.
- An unchanged row costs one `memcmp`.
- Nothing ever reads video memory back. Framebuffer reads are uncached,
  and on hardware very slow.

**Write-combining.** The framebuffer is mapped `VM_WC`. The kernel
programs the PAT so that entry 1 (PWT alone) means write-combining
instead of write-through, which nothing used. Entries 0, 2 and 3 keep
their power-on types, so `PCD|PWT` still means strong uncached. Without
a PAT, `VM_WC` falls back to uncached. `iomap()` refuses WC and UC
mappings of RAM the direct map caches, for the same reason as before:
mixed memory types are undefined.

## Terminal semantics (`lib/textgrid.c`)

A VT100 subset, enough for a log:
- printable ASCII goes at the cursor; anything else shows as `?`;
- writing the last column sets a pending wrap, resolved by the next
  printable character, so an exactly-full line followed by `\n` does not
  leave a blank line;
- `\n` is a new line, `\r` the start of this one, `\t` the next multiple
  of 8, `\b` one column left;
- below the last row, everything scrolls up.

## Tests

| Where | What |
|-------|------|
| `tests/host/test_textgrid.c` | Hand-checked cases (wrap, pending wrap, tabs, backspace, scroll), draw counts proving the diff, and 20,000 random streams against an **independent reference model**, rendered at random moments. The display built from the draw calls must equal the model's screen. |
| `tests/host/test_fbrender.c` | Exact pixels of plain and scaled glyphs; row padding never written; clipping at every edge in exactly-sized buffers under ASan. |
| `tests/kernel/test_console.c` | The log ring. The screen matches the loader. Printed text is in the grid **and in video memory**: VGA cells, or every framebuffer pixel of the first two glyphs read back. Scrolling. Output printed with interrupts off is logged but not drawn until they are back on, and no render ever ran with them off. The PAT holds WC at entry 1, and a `VM_WC` mapping uses it. |

Each layer was mutation-checked by planting bugs, and every one was
caught:
- six in the text grid, two of them in the render diff;
- three in the renderer;
- one that removes the deferral.

The deferral check had a vacuous assertion until then. On the bottom row
a drawn line scrolls the grid instead of moving the cursor, so the test
now checks the scroll count too.

## Limits

- **Panics before the memory manager is up** (GDT/IDT setup, the CPU
  checks, `mm_init` itself) reach COM1 only. The screen is mapped through
  `iomap()`. An early console drawn through the loader's identity map
  would close the gap.
- Output only: no keyboard yet.
- One font size; no colors or escape sequences.
- GOP mode as the firmware set it: no mode switch.
- BIOS graphics modes (VBE) are not used: a BIOS that boots into a
  graphics splash gets no screen console.
