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
| BIOS | VGA text mode | If the BIOS data area says the display is in text mode (2, 3 or 7). The kernel adopts the screen as the BIOS left it: the loader's progress line and the cursor stay, the BIOS's colors do not. If another display page is active, page 0 is shown instead, cleared. |

**Which GOP.** The loader tries the GOP on the console-out handle first,
because it drives the visible display. On a machine with several outputs,
that one can be a Blt-only splitter, so the loader then takes the first
GOP with a usable framebuffer. A usable framebuffer has:
- 32-bit pixels: a bit-mask mode whose masks stop at bit 23 is 24-bit,
  and is rejected rather than drawn with the wrong pixel size;
- byte-aligned channels;
- a size at least `pitch × height`, so the kernel can never write past
  its end.

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
- One context feeds the screen at a time, **with preemption off**.
  Another that finds it busy leaves its bytes to the feeder, which loops
  until it has caught up. Nothing is dropped, and nobody waits.
- The first version left preemption on, and the adversarial review found
  the consequence. The idle task, preempted mid-redraw by a timer wakeup
  (a redraw takes longer than a tick), kept the screen "busy" for as long
  as any task stayed runnable. Every message in that time reached COM1
  and never the screen.
- A `console` kernel thread drains every 50 ms. Output deferred from an
  interrupt handler then reaches the screen even if no task ever prints
  again and the CPU never idles. It sleeps on the timer rather than being
  woken from `printk`, because a wakeup takes the scheduler lock, which
  the printing code may already hold.
- Before interrupts are enabled (boot), drawing is immediate, so a hang
  in early boot is visible. In a panic it is immediate too, with no lock.
  It resumes at the exact byte an interrupted feeder reached, so nothing
  is drawn twice. A fault inside the drawing code itself leaves the
  nested panic on COM1 instead of recursing.
- After each framebuffer render, an `sfence` drains the write-combining
  buffers. A panic that halts never would.

Once the system is live, a render that runs with interrupts off or
preemption on is a violation. `screen_render_violations()` counts them,
and debug builds panic. Every test of every boot in `make test` checks
the rule, not just the console tests: removing the `preempt_disable()`
fails the very first render.

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
  of 8 (it neither moves nor cancels a pending wrap), `\b` one column
  left;
- below the last row, everything scrolls up.

## Tests

| Where | What |
|-------|------|
| `tests/host/test_textgrid.c` | Hand-checked cases (wrap, pending wrap, tabs, backspace, scroll), draw counts proving the diff, and 20,000 random streams against an **independent reference model**, rendered at random moments. The display built from the draw calls must equal the model's screen. |
| `tests/host/test_fbrender.c` | Exact pixels of plain and scaled glyphs; row padding never written; clipping at every edge in exactly-sized buffers under ASan, including the exact pixels of the visible part of a clipped glyph. |
| `make test` (QEMU runs) | UEFI without a GOP (`-vga none`) and with a Blt-only one (`-vga none -device virtio-gpu-pci`): the loader says why, the kernel logs `no framebuffer from the loader: no screen console`, and the console stays on serial, through the in-kernel suite (with its exact skip count) and the `init` demo. A UEFI boot that panics after the framebuffer console is up (`-machine pc,pit=off,hpet=off`) must panic exactly once, with no nested panic. `console=serial loglevel=warn` (`disk-ktest-quiet.img`): no screen console, and none of the info-level lines the boot and the suite print. |
| `tests/kernel/test_console.c` | The log ring. The screen matches the loader. Printed text is drawn by the time `kprintf` returns, with no drain: it is in the grid, in what the grid last rendered (`shown`), **and in video memory**: VGA cells, or every framebuffer pixel of the first two glyphs read back. Scrolling, with the last line rendered. Output printed with interrupts off is logged but not drawn until they are back on, and no render ever ran with them off. The PAT holds WC at entry 1, and a `VM_WC` mapping uses it. |

Each layer was mutation-checked by planting bugs, and every one was
caught:
- six in the text grid, two of them in the render diff;
- three in the renderer;
- one that removes the deferral;
- one that removes the `preempt_disable()`.

The adversarial review of the first version also found:
- the preempted-feeder stall described above;
- duplicated lines on a panic screen;
- the 24-bit and multi-GOP cases;
- a tab that cancelled a pending wrap;
- a clipping test that could not detect under-drawing.

All of them are fixed.

The deferral check had a vacuous assertion until then. On the bottom row
a drawn line scrolls the grid instead of moving the cursor, so the test
now checks the scroll count too.

## Limits

- **COM1 is polled with interrupts off.** Bytes go to the UART under the
  console lock, waiting for the transmitter before each one, so a message
  keeps interrupts off for its whole transmission. On a real UART at
  115200 baud (8N1) that is about 87 µs per byte, and a `\n` is sent as
  two bytes. One 256-byte chunk of a `write()` call takes about 22 ms (up
  to 44 ms if it is all newlines). Of the ticks that fall due meanwhile,
  one stays pending and the rest are lost to the clock.
  Draining COM1 from a buffer, as the screen is fed, would fix it.
- **No wait on COM1 is unbounded.** The loopback self-test and each
  transmitted byte give up after `SERIAL_POLL_MAX` (100,000) line-status
  polls, with no clock needed; a transmitter that never has room is then
  given up on for good, COM1 reads as absent and its receive interrupt is
  turned off, so a stuck UART costs one wait, not one per byte, and
  `panic()` still reaches the screen. The receive drain counts dropped
  (errored) bytes against its limit, so a line delivering nothing but
  errors cannot hold the CPU in IRQ 4. A model UART in the host tests
  (`tests/host/test_serial.c`) shows each of these ends.
- **Panics before the memory manager is up** (GDT/IDT setup, the CPU
  checks, `mm_init` itself) reach COM1 only. The screen is mapped through
  `iomap()`. An early console drawn through the loader's identity map
  would close the gap.
- Input comes from the PS/2 keyboard and COM1, through `read()`
  ([input.md](input.md)). The kernel never echoes: the reading program
  does, through `write()`, so typed text lands in the log ring like any
  output. A kernel message printed in the middle of a typed line is not
  redrawn.
- One font size; no colors or escape sequences.
- GOP mode as the firmware set it: no mode switch.
- BIOS graphics modes (VBE) are not used: a BIOS that boots into a
  graphics splash gets no screen console.
