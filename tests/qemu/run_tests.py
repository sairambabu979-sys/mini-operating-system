#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Boot a test image in QEMU and judge the result.

The kernel, booted with "ktest", runs every KTEST() and reports TAP on
COM1, then exits QEMU through the isa-debug-exit device. A run passes only
if all of the following hold:

  * QEMU exits with status 33 (kernel wrote QEMU_EXIT_SUCCESS),
  * the TAP plan ("1..N") is present and N results were reported,
  * no result is "not ok",
  * it finishes within the timeout.

A triple fault shows up as QEMU exiting 0 (thanks to -no-reboot) with a
truncated log, so it can never be mistaken for success.

With --expect-panic the run must instead end in a kernel panic (status 35)
whose output matches every --expect regex, in order.
"""

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

EXIT_SUCCESS = (0x10 << 1) | 1  # 33
EXIT_FAILURE = (0x11 << 1) | 1  # 35

TAP_PLAN = re.compile(r"^1\.\.(\d+)$")
TAP_RESULT = re.compile(r"^(not ok|ok) (\d+) - (.+)$")


def qemu_command(image: Path, extra: list[str]) -> list[str]:
    return [
        "qemu-system-x86_64",
        "-m", "128M",
        "-cpu", "max",
        "-no-reboot",
        "-display", "none",
        "-monitor", "none",
        "-serial", "stdio",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-drive", f"file={image},format=raw,if=ide",
        *extra,
    ]


def parse_tap(lines: list[str]) -> tuple[int | None, list[tuple[bool, int, str]]]:
    plan = None
    results = []
    for line in lines:
        line = line.strip()
        if m := TAP_PLAN.match(line):
            plan = int(m.group(1))
        elif m := TAP_RESULT.match(line):
            results.append((m.group(1) == "ok", int(m.group(2)), m.group(3)))
    return plan, results


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--image", type=Path, required=True)
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--log-dir", type=Path, default=Path("build/logs"))
    ap.add_argument("--expect-panic", action="store_true",
                    help="the run must end in a kernel panic")
    ap.add_argument("--expect", action="append", default=[],
                    help="regex the output must match (in order); repeatable")
    ap.add_argument("qemu_args", nargs="*", help="extra QEMU arguments (after --)")
    args = ap.parse_args()

    args.log_dir.mkdir(parents=True, exist_ok=True)
    log_path = args.log_dir / f"{args.image.stem}.serial.log"
    cmd = qemu_command(args.image, args.qemu_args)

    start = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=args.timeout)
        status, timed_out = proc.returncode, False
        output = proc.stdout
    except subprocess.TimeoutExpired as e:
        status, timed_out = None, True
        output = e.stdout or b""
    elapsed = time.monotonic() - start

    text = output.decode("utf-8", errors="replace").replace("\r\n", "\n")
    log_path.write_text(text)
    lines = text.splitlines()
    plan, results = parse_tap(lines)

    # Echo the run so CI logs are self-contained.
    for line in lines:
        print(f"  | {line}")

    problems = []
    if timed_out:
        problems.append(f"timed out after {args.timeout:.0f}s")
    elif args.expect_panic:
        if status != EXIT_FAILURE:
            problems.append(f"expected a panic (status {EXIT_FAILURE}), got status {status}")
    elif status == EXIT_FAILURE:
        problems.append("kernel reported failure (QEMU_EXIT_FAILURE)")
    elif status != EXIT_SUCCESS:
        problems.append(f"QEMU exited with status {status}: crash or triple fault?")

    pos = 0
    for pattern in args.expect:
        m = re.compile(pattern, re.MULTILINE).search(text, pos)
        if not m:
            problems.append(f"output does not match /{pattern}/ (after offset {pos})")
            break
        pos = m.end()

    if not args.expect_panic:
        if plan is None:
            problems.append("no TAP plan in output")
        elif len(results) != plan:
            problems.append(f"TAP plan announced {plan} tests, {len(results)} reported")
    failed = [name for ok, _, name in results if not ok]
    if failed:
        problems.append("failed tests: " + ", ".join(failed))

    passed = sum(ok for ok, _, _ in results)
    what = "panicked as expected" if args.expect_panic else f"{passed}/{plan or 0} passed"
    summary = f"{args.image.name}: {what} in {elapsed:.1f}s (log: {log_path})"
    if problems:
        print(f"FAIL {summary}")
        for p in problems:
            print(f"  - {p}")
        return 1
    print(f"PASS {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
