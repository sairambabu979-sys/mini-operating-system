#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A key pressed while the kernel boots must not cost it the keyboard.

Presses one key over the QEMU monitor as soon as the kernel logs its irq
line, a moment before input_init() programs the 8042, then requires the
keyboard line and no i8042 or input warning. A key pressed too late only
types a character, so this can miss the bug but never fail a correct
kernel.

  boot_keypress.py --image disk-demo.img --log-dir DIR
"""

import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

QEMU = os.environ.get("QEMU") or "qemu-system-x86_64"
EXIT_SUCCESS = 33  # (QEMU_EXIT_SUCCESS << 1) | 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True, type=Path)
    ap.add_argument("--log-dir", required=True, type=Path)
    args = ap.parse_args()
    args.log_dir.mkdir(parents=True, exist_ok=True)
    log = args.log_dir / f"{args.image.stem}-bootkey.serial.log"
    log.unlink(missing_ok=True)
    sock_dir = tempfile.mkdtemp(prefix="bootkey-")  # AF_UNIX paths must stay short
    sock = os.path.join(sock_dir, "m.sock")
    qemu = subprocess.Popen(
        [QEMU, "-m", "128M", "-cpu", "max", "-no-reboot", "-display", "none",
         "-serial", f"file:{log}", "-monitor", f"unix:{sock},server=on,wait=off",
         "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-snapshot",
         "-drive", f"file={args.image},format=raw,if=ide"], stdin=subprocess.DEVNULL)
    mon = socket.socket(socket.AF_UNIX)
    pressed = False
    try:
        for _ in range(200):
            try:
                mon.connect(sock)
                break
            except OSError:
                time.sleep(0.05)
        deadline = time.time() + 30
        while time.time() < deadline and qemu.poll() is None:
            if log.exists() and "irq: using" in log.read_text(errors="replace"):
                mon.sendall(b"sendkey a\n")
                pressed = True
                break
            time.sleep(0.002)
        status = qemu.wait(timeout=30)
    except subprocess.TimeoutExpired:
        qemu.kill()
        status = None
    finally:
        mon.close()
        shutil.rmtree(sock_dir, ignore_errors=True)
    text = log.read_text(errors="replace") if log.exists() else ""
    problems = []
    if not pressed:
        problems.append("never saw the irq line to press the key at")
    if status != EXIT_SUCCESS:
        problems.append(f"QEMU exited with {status}, want {EXIT_SUCCESS}")
    if not re.search(r"^input: PS/2 keyboard on IRQ 1", text, re.M):
        problems.append("no 'input: PS/2 keyboard on IRQ 1' line")
    for line in re.findall(r"^WARNING: (?:i8042|input): .*$", text, re.M):
        problems.append(f"rejected line: {line}")
    if problems:
        print(f"FAIL {args.image.name} with a key pressed during boot (log: {log})")
        for p in problems:
            print(f"  - {p}")
        return 1
    print(f"PASS {args.image.name} with a key pressed during boot: keyboard up (log: {log})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
