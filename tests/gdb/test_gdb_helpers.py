#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Drive the gdb helpers against a live kernel and check their output.

Boots the run image halted (-S), attaches gdb, stops at process_spawn
(scheduler and page tables are up by then), and runs minios-tasks and
minios-ptwalk on a text, a data and an unmapped address.
"""

import argparse
import re
import socket
import subprocess
import sys
import time
from pathlib import Path


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def run_gdb(kernel: Path, port: int) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["gdb", "-batch", "-nx", "-ex", "set pagination off", "-ex", "source tools/gdb/minios.py",
         "-ex", f"file {kernel}", "-ex", f"target remote 127.0.0.1:{port}", "-ex",
         "break process_spawn", "-ex", "continue", "-ex", "minios-tasks", "-ex",
         "minios-ptwalk (unsigned long)&kmain", "-ex", "minios-ptwalk (unsigned long)&boot_task",
         "-ex", "minios-ptwalk 0x1000"],
        capture_output=True, text=True, timeout=60)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", type=Path, required=True)
    ap.add_argument("--kernel", type=Path, required=True)
    args = ap.parse_args()

    port = free_port()
    qemu = subprocess.Popen(
        ["qemu-system-x86_64", "-m", "128M", "-cpu", "max", "-display", "none", "-monitor",
         "none", "-serial", "null", "-snapshot", "-drive", f"file={args.image},format=raw",
         "-gdb", f"tcp:127.0.0.1:{port}", "-S"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        # QEMU opens the gdb port asynchronously: retry while it refuses, never guess a delay.
        for _ in range(40):
            gdb = run_gdb(args.kernel, port)
            if "Connection refused" not in gdb.stdout + gdb.stderr:
                break
            time.sleep(0.25)
    finally:
        qemu.kill()
        qemu.wait()

    out = gdb.stdout
    checks = {
        "tasks lists the boot task as current": r"\*\s+1\s+RUNNING\s+kmain",
        "tasks lists the idle task": r"\s0\s+RUNNABLE\s+idle",
        "kernel text maps R-X, global": r"-> phys 0x1[0-9a-f]+ \(R S X G, 4 KiB page\)",
        "kernel data maps RW, NX": r"-> phys 0x[0-9a-f]+ \(W S NX G, 4 KiB page\)",
        "identity map is gone": r"va 0x0000000000001000[\s\S]*?not present",
    }
    failed = [name for name, pattern in checks.items() if not re.search(pattern, out)]
    for name in checks:
        print(f"{'FAIL' if name in failed else 'PASS'} gdb: {name}")
    if failed:
        print(out, gdb.stderr, sep="\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
