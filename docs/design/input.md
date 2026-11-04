# Input

Sources: `kernel/core/input.c`, `kernel/drivers/i8042.c`,
`kernel/drivers/serial.c` (receive), `kernel/lib/kbdmap.c`,
`kernel/proc/syscalls.c` (`sys_read`), `user/lib/lineedit.c`,
`user/programs/sh.c`.

## Data flow

```
 PS/2 keyboard ─► 8042 ─► IRQ 1 ─► input_poll_kbd ─► kbdmap (set 1 → byte) ─┐
                                                                            ├─► queue (256 B) ─► read(0) ─► sh: lineedit ─► write(1)
 COM1 line ─► 16550 FIFO ─► IRQ 4 ─► serial_rx_drain (bytes as they are) ───┤                                           (the echo)
                                                       input_inject (tests) ┘
```

The kernel delivers raw bytes and never echoes. Decoding scan codes is the
kernel's job, because only it sees the keyboard. Editing the line is the
reading program's job, because only it knows what a line is. The shell
echoes through `write()`, which goes to COM1 and the screen like any
output.

## The 8042 (`drivers/i8042.c`)

The kernel uses the keyboard port only. `i8042_init()` runs in this order:

1. If the status port reads `0xFF`, there is no controller (an absent port
   floats high). The kernel warns `no controller (status 0xff)` and has no
   keyboard. This check is the presence test: the ACPI flag that says
   whether an 8042 exists is missing from older FADT revisions, so the
   kernel does not rely on it.
2. Both ports off (`0xAD`, `0xA7`).
3. Flush: read the data port while a byte is waiting, at most 16 times.
4. Read the configuration byte (`0x20`).
5. Write a new one: the firmware's "system" bit, translation on, the mouse
   clock off, both IRQs off, **and the keyboard clock still off**. Read it
   back; a mismatch warns `config byte did not stick` and gives no keyboard.
6. Keyboard port on (`0xAE`), then scanning on (`0xF4`). The ACK (`0xFA`)
   is polled, since IRQ 1 is still off; it also drops any key that was
   waiting in the keyboard. A missing ACK only warns.
7. Write the final configuration byte: keyboard clock on, keyboard IRQ on.

Measured under QEMU 8.2:

| Firmware | Found | Left |
|----------|-------|------|
| SeaBIOS | `0x71` | `0x61` |
| OVMF | `0x77` | `0x65` |

Both firmwares leave the keyboard IRQ on, so the "IRQ off until the end"
rule is the driver's own.

**Why the keyboard clock stays off until `0xAE`.** A first version wrote
the new configuration byte with the keyboard clock bit clear. That turned
the port back on straight after `0xAD`. A key still queued in the keyboard
was then delivered, and was read back in place of the configuration byte.
Init saw a mismatch and gave up, so the whole boot had no keyboard. One key
pressed and released before `input_init()` was enough: it happened in 3 of
3 boots. `tests/qemu/boot_keypress.py` presses a key over the QEMU monitor
as the kernel logs its `irq:` line, and requires the keyboard to come up.

**Every wait is bounded.** Each wait for the controller has a 10 ms TSC
deadline, and the keyboard's ACK gets 25 ms. A status of `0xFF` at any
point ends the wait. Nothing here can hang a machine without a controller.

**The output port is never touched.** Commands `0xD0` and `0xD1` read and
write it, and it carries the A20 gate and the reset line. Stage 2 may have
used it to turn A20 on (the `kbc` method). The kernel's sequence does not
depend on what stage 2 did: the A20 images run it after stage 2's KBC path,
and expect the keyboard line.

**The IRQ 1 handler** (`input_poll_kbd`) reads at most 16 bytes per call.
QEMU raises a new edge for each byte it loads, so the cap cannot strand
one. Each byte goes through the decoder, under the input lock, and readers
are woken after it is released. A mouse byte (status bit 5) is dropped and
counted. A call that finds nothing is counted and expected.

**The masked-edge hazard.** A byte that reaches the controller while IRQ 1
is masked raises its only edge then:
- **I/O APIC:** a masked edge is dropped. When the line is unmasked, no
  interrupt comes, and the controller, still holding the byte, raises no
  new one.
- **8259:** the edge is latched in the IRR and delivered on unmask.

So `input_init()` calls `input_poll_kbd()` once by hand after
`irq_register()`. `kbd_byte_left_pending_while_masked_is_recovered` shows
both halves on the APIC path: the byte is lost to the edge, and the manual
poll recovers it. It passes on the 8259 and x2APIC paths too.

## COM1 receive (`drivers/serial.c`)

Output stays polled, with interrupts off, so `panic()` can always print.
Receive uses the received-data interrupt only, on IRQ 4. The FIFO stays at
`0xC7`, with a 14-byte trigger.

- **The drain** (`serial_rx_drain`) never waits. It reads while the line
  status shows data:
  - a byte with a parity or framing error, or a break, is dropped and
    counted;
  - an overrun is counted, and the byte kept;
  - a line status of `0xFF` reports the UART gone. The handler then turns
    receive off and warns once.
- **Line-status reads clear the error bits.** Reading a 16550's line status
  clears its receive-error bits, which describe the byte at the head of the
  FIFO. Transmit polling reads that register too. Every line-status read on
  the transmit side keeps the error and overrun bits it saw, and the drain
  applies them to the byte it takes next. Both sides run with interrupts
  off, on the one CPU.
- **The handler** drains at most 64 bytes per IRQ and queues them as they
  are. A serial terminal sends CR for Enter; the line editor takes CR, LF
  and CRLF alike. If data was still waiting at the cap, it re-arms the
  interrupt (IER off, then on) for a fresh edge.
- **At init**, with interrupts off: drain what arrived before, queue it, and
  only then enable the interrupt. Bytes arriving meanwhile wait in the FIFO
  and come with the first IRQ 4, so they queue after those, in order.

  A first version queued the drained bytes only after interrupts came back
  on. The bytes that arrived during the drain then went first: in QEMU,
  with stdin fed during boot, the early bytes came out 28 places late in 4
  of 5 boots. The typeahead run, below, checks this.
- **Only the handler reads the receive register** once receive is on. The
  other readers are init and the test hooks, which run with interrupts off
  or with IRQ 4 masked. `panic()` and `kprintf` only poll the line status
  and write the transmit register.

## The decoder (`lib/kbdmap.c`)

The decoder is pure code, host-tested and fuzzed. It turns scan code set 1
into bytes, US layout. The controller translates whatever set the keyboard
speaks into set 1.

| Input | Byte |
|-------|------|
| printable keys, with either Shift | `0x20`-`0x7E` |
| Ctrl + letter | letter & `0x1F` (Ctrl-C is `0x03`) |
| Enter, keypad Enter | `0x0A` |
| Backspace / Tab / Esc | `0x7F` / `0x09` / `0x1B` |
| keypad digits | always digits (no Num Lock state) |
| Alt, lock keys, F-keys, arrows and other `E0` keys, Pause | nothing |
| `FA`, `FE`, `EE`, `00`, `FF` (controller and keyboard chatter) | nothing (`00`/`FF` also drop a pending prefix) |

Break codes only update the modifiers. One scan code gives at most one
byte.

## The queue and `read()` (`core/input.c`, `sys_read`)

**Invariants:**
- **One leaf spinlock, `input_lock`, guards the ring and the stats.** It
  saves IRQ state. Nothing else is taken under it, and printing, waking
  and `copy_to_user` all happen after it is released.
- **Producers are the IRQ 1 handler, the IRQ 4 handler and `input_inject()`
  (tests).** Each pushes under the lock, unlocks, and then calls
  `wake_up_all`. When the ring is full, the newest byte is dropped and
  counted.
- **Readers sleep only in `wait_event()`, whose condition takes the bytes
  under the lock.** `wait_event()` marks the task BLOCKED and queues it
  before it evaluates the condition, so a byte pushed from then on wakes
  it. An interrupt that wakes the task it interrupted is handled by the
  scheduler (see the lost-wakeup argument in
  [scheduler.md](scheduler.md#never-losing-a-wakeup)). With several
  readers, each byte goes to exactly one of them; which one is unspecified.
- **Every 8042 wait is bounded.** `0xFF` means no controller.
- **`read()` consumes nothing unless it can deliver.**

**`read(fd, buf, len)`, syscall 8.** The order of the checks is part of
the ABI:
1. `-EBADF` unless the fd is open and is the console's input (fd 0,
   unless `spawn` gave the process another);
2. `-EFAULT` if the range is bad as a whole;
3. 0 for `len` 0;
4. `-EFAULT` unless every byte this call could fill, `min(len, 256)`, is
   mapped and writable. The buffer is probed (read, then written back
   unchanged) before anything is taken off the queue, so a bad buffer
   never swallows a keystroke;
5. `-ENODEV` with no input device;
6. otherwise it blocks until at least one byte is queued, and returns 1 to
   `min(len, 256)` bytes.

The block is uninterruptible, with no timeout. The kernel never reports
end of file: Ctrl-D is just the byte `0x04`.

## `init=` and the shell

`init=NAME` (1 to 15 characters) makes `NAME` the first process instead of
`init`. A longer or empty name panics with `init=: a program name has 1 to
15 characters`. A name that does not exist panics with `cannot start init
'NAME': -2`. `exit-after-init` now also makes a panic exit QEMU (status 35).

`sh` reads one byte at a time, so whatever was typed after a command's
Enter stays in the kernel's queue for the program that command starts (a
paste of `readkeys\nok\n` gives `readkeys` its `ok`). It feeds each byte
to the line editor, writing each echo with one `write()`. The editor
handles Backspace and DEL, Ctrl-U, Ctrl-C (`^C`, the line dropped), Ctrl-D
(end of input on an empty line), and swallows escape sequences. Lines hold
31 characters, so after the 4-column prompt the echo never wraps on an
80-column screen. A finished line is split into words at runs of spaces
(no quoting); the first names the program, and the rest are its
arguments (`echo` prints them back). On a finished line:

| Line | Output |
|------|--------|
| empty | nothing |
| `help` | `builtins: help exit`, `programs (in /bin):`, then each file `readdir("/bin")` lists |
| `exit` | the shell exits 0 |
| `help` or `exit` with words after it | `sh: help takes no arguments` (or `exit`) |
| a program name, then up to 8 words | the program, run with the words as its arguments: its output, then `[exit N]` |
| more than 8 words after the name | `sh: too many arguments (8 at most)` |
| unknown | `sh: NAME: not found` |
| 16 characters or more | `sh: name too long (15 characters at most)` |
| at `TASK_MAX` | `sh: too many tasks` |

Ctrl-D on an empty line exits 0. If `read()` fails, the shell prints `sh:
cannot read input (-19)` and exits 1. Every way out ends its output with a
newline. The shell waits for each program it starts, so it never holds
more than one child.

`make run-shell` boots `disk-shell.img` with this terminal as COM1
(`-serial mon:stdio`: Ctrl-C reaches the guest, Ctrl-A x quits QEMU).
`QEMU_DISPLAY=gtk` opens a window whose keystrokes reach the PS/2
keyboard.

## Tests

| What | How | Where |
|------|-----|-------|
| decoder: every character, control and extended keys, an exhaustive walk over its states, random prefixes | host, ASan and UBSan | `tests/host/test_kbdmap.c` |
| line editor: editing keys, CR/LF/CRLF, escape sequences, a 200k-byte fuzz against a reference model and the real text grid | host | `tests/host/test_lineedit.c` |
| queue: order, overflow, a reader sleeping and woken, a byte arriving between a reader's empty check and its sleep, 3000 bytes producer to consumer, two readers | `input_inject()` and a test hook; blocking readers are detached threads with deadlines | `tests/kernel/test_input.c` |
| 8042: the configuration byte, `Hi\n` through IRQ 1, every printable character, the masked edge, control and extended keys | command `0xD2`, which delivers a set 1 byte as if typed | `tests/kernel/test_input.c` |
| COM1: bytes through IRQ 4, a full FIFO in order, a woken reader, the drain's cap, an error seen by transmit polling | the UART's own loopback, interrupts off | `tests/kernel/test_input.c` |
| `read()` from ring 3: bad buffers and the check order, blocking, at most 256 bytes, never more than asked, no device, a reader woken by IRQ 1 | `readkeys`, `readmax`, `badptr` | `tests/kernel/test_user.c` |
| the shell: `[exit N]`, not found, name too long, arguments (`echo a  b`, too many, a builtin with one), `^C`, blank and padded lines, `exit` (not the Ctrl-D behind it), task count restored, Ctrl-D alone, `-ENODEV` | `input_inject()` | `tests/kernel/test_user.c` |
| boot | a key pressed during boot (`boot_keypress.py`); bytes 0-239 on stdin during boot must arrive rising (the typeahead run) | `make qemu-test` |
| typed sessions | the shell typed at through QMP `send-key` (APIC, 8259, UEFI) and over COM1 with no 8042; a bad `init=` | `make qemu-test` |

Each of these planted bugs fails at least one test:
- no wakeup, `wake_up_one`, a check before getting on the wait queue, a
  reader that polls;
- no buffer probe or a one-byte probe, no 256-byte clamp, asking the queue
  for 256 whatever the caller's length, the checks in another order;
- index arithmetic broken at the u32 wrap;
- translation off, the keyboard IRQ never enabled, raw scan codes queued;
- no wakeup from IRQ 4, receive never enabled, a drain that stops early or
  misreports what is left;
- receive errors seen by transmit polling ignored;
- the shell without its `exit` builtin.

No test can reach these:
- an 8042 flush that does nothing, or scanning never enabled: QEMU hands
  over with an empty buffer, and its keyboard scans from reset;
- mouse bytes not dropped: the mouse clock is off, so none arrive;
- no drain after `irq_register`: nothing is pending at boot. The
  masked-edge test shows the hazard and that the same call recovers it;
- receive errors in the line itself, the re-arm at the drain's cap, and a
  dead UART: QEMU reports no errors, re-raises IRQ 4 on its own, and its
  UART never reads `0xFF`.

Nothing has run on real hardware. The `0xD2` and loopback tests are
QEMU-only, and would fail, not skip, on hardware without them. The
README's limitations list the rest.
