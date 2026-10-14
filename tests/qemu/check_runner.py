#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for the test runner: it must FAIL, for the right reason, when the
kernel hangs or triple-faults. A runner that cannot fail proves nothing.

  check_runner.py --hang-image H --triplefault-image T --log-dir DIR
"""

import argparse
import subprocess
import sys
from pathlib import Path

RUNNER = Path(__file__).with_name("run_tests.py")


def expect_failure(image: Path, log_dir: Path, reason: str, extra: list[str]) -> bool:
    proc = subprocess.run([sys.executable, str(RUNNER), "--image", str(image), "--log-dir",
                           str(log_dir), "--no-tap", *extra], capture_output=True, text=True)
    out = proc.stdout + proc.stderr
    ok = proc.returncode != 0 and "FAIL" in out and reason in out
    print(f"{'PASS' if ok else 'FAIL'} runner rejects {image.name}: expected '{reason}'")
    if not ok:
        print(out)
    return ok


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hang-image", type=Path, required=True)
    ap.add_argument("--triplefault-image", type=Path, required=True)
    ap.add_argument("--log-dir", type=Path, required=True)
    args = ap.parse_args()

    results = [
        expect_failure(args.hang_image, args.log_dir, "timed out", ["--timeout", "5"]),
        expect_failure(args.triplefault_image, args.log_dir, "crash or triple fault", []),
    ]
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
