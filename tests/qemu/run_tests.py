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
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

EXIT_SUCCESS = (0x10 << 1) | 1  # 33
EXIT_FAILURE = (0x11 << 1) | 1  # 35

TAP_PLAN = re.compile(r"^1\.\.(\d+)$")

# OVMF (UEFI firmware for QEMU) as packaged by Debian/Ubuntu (`apt install ovmf`).
OVMF_CODE_CANDIDATES = ["/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_CODE.fd"]


def ovmf_drives(workdir: Path) -> list[str]:
    """pflash drives for OVMF: read-only code plus a private copy of the variables."""
    for code in map(Path, OVMF_CODE_CANDIDATES):
        vars_template = code.with_name(code.name.replace("CODE", "VARS"))
        if code.exists() and vars_template.exists():
            vars_copy = workdir / "OVMF_VARS.fd"
            shutil.copyfile(vars_template, vars_copy)
            return ["-drive", f"if=pflash,format=raw,readonly=on,file={code}",
                    "-drive", f"if=pflash,format=raw,file={vars_copy}"]
    sys.exit("run_tests: OVMF not found (apt install ovmf); cannot run a UEFI test")
TAP_RESULT = re.compile(r"^(not ok|ok) (\d+) - (.+)$")


def qemu_command(image: Path, memory: str, extra: list[str]) -> list[str]:
    return [
        "qemu-system-x86_64",
        "-m", memory,
        "-cpu", "max",
        "-no-reboot",
        "-display", "none",
        "-monitor", "none",
        "-serial", "stdio",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        # -snapshot: writes go to a temporary overlay, so concurrent runs of
        # one image never contend for its write lock and never modify it.
        "-snapshot",
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
    ap.add_argument("--memory", default="128M", help="guest RAM, e.g. 128M or 4G")
    ap.add_argument("--uefi", action="store_true", help="boot through OVMF instead of the BIOS")
    ap.add_argument("--log-dir", type=Path, default=Path("build/logs"))
    ap.add_argument("--log-suffix", default="",
                    help="distinguishes the log of a run with different QEMU arguments")
    ap.add_argument("--expect-panic", action="store_true",
                    help="the run must end in a kernel panic")
    ap.add_argument("--skips", type=int,
                    help="exact number of tests that must report # SKIP: a test that starts "
                         "skipping (say, because hardware discovery broke) fails the run")
    ap.add_argument("--no-tap", action="store_true",
                    help="the run reports no TAP (judge by exit status and --expect only)")
    ap.add_argument("--expect", action="append", default=[],
                    help="regex the output must match (in order); repeatable")
    ap.add_argument("qemu_args", nargs="*", help="extra QEMU arguments (after --)")
    args = ap.parse_args()

    args.log_dir.mkdir(parents=True, exist_ok=True)
    suffix = f"-{args.log_suffix}" if args.log_suffix else ""
    log_path = args.log_dir / f"{args.image.stem}-{args.memory}{suffix}.serial.log"
    workdir = Path(tempfile.mkdtemp(prefix="minios-qemu-"))
    firmware = ovmf_drives(workdir) if args.uefi else []
    cmd = qemu_command(args.image, args.memory, firmware + args.qemu_args)

    start = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=args.timeout)
        status, timed_out = proc.returncode, False
        output, stderr = proc.stdout, proc.stderr
    except subprocess.TimeoutExpired as e:
        status, timed_out = None, True
        output, stderr = e.stdout or b"", e.stderr or b""
    elapsed = time.monotonic() - start
    shutil.rmtree(workdir, ignore_errors=True)

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
    if status not in (EXIT_SUCCESS, EXIT_FAILURE) and stderr.strip():
        problems.append("QEMU stderr: " + stderr.decode("utf-8", errors="replace").strip())

    pos = 0
    for pattern in args.expect:
        m = re.compile(pattern, re.MULTILINE).search(text, pos)
        if not m:
            problems.append(f"output does not match /{pattern}/ (after offset {pos})")
            break
        pos = m.end()

    if not args.expect_panic and not args.no_tap:
        if plan is None:
            problems.append("no TAP plan in output")
        elif len(results) != plan:
            problems.append(f"TAP plan announced {plan} tests, {len(results)} reported")
    failed = [name for ok, _, name in results if not ok]
    if failed:
        problems.append("failed tests: " + ", ".join(failed))
    if args.skips is not None and not args.expect_panic and not args.no_tap:
        skipped_names = [name for ok, _, name in results if ok and "# SKIP" in name]
        if len(skipped_names) != args.skips:
            problems.append(f"expected {args.skips} skipped tests, got {len(skipped_names)}: "
                            + "; ".join(skipped_names))

    skipped = sum(ok and "# SKIP" in name for ok, _, name in results)
    passed = sum(ok for ok, _, _ in results) - skipped
    # On a FAIL the problems below say what happened; the summary must not claim success.
    if args.expect_panic:
        what = "expected a panic" if problems else "panicked as expected"
    elif args.no_tap:
        what = (f"{len(args.expect)} expectations" if problems
                else f"exited cleanly, {len(args.expect)} expectations met")
    else:
        what = f"{passed}/{plan or 0} passed ({skipped} skipped)"
    config = args.memory + (f", {' '.join(args.qemu_args)}" if args.qemu_args else "")
    summary = f"{args.image.name} ({config}): {what} in {elapsed:.1f}s (log: {log_path})"
    if problems:
        print(f"FAIL {summary}")
        for p in problems:
            print(f"  - {p}")
        return 1
    print(f"PASS {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
