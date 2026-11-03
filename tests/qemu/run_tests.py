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
whose output matches every --expect regex, in order. Any run fails if its
output matches a --reject regex anywhere.

With --type-after REGEX --type TEXT (repeatable, in pairs) the runner types
into the guest: it waits for each REGEX to appear after the previous match,
then types TEXT (backslash escapes allowed, e.g. '\\n', '\\x03'). --input
ps2 (the default) types keys on the PS/2 keyboard through QMP send-key;
--input serial writes the bytes to COM1.
"""

import argparse
import codecs
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

EXIT_SUCCESS = (0x10 << 1) | 1  # 33
EXIT_FAILURE = (0x11 << 1) | 1  # 35

TAP_PLAN = re.compile(r"^1\.\.(\d+)$")

# OVMF (UEFI firmware for QEMU): as Debian/Ubuntu package it (`apt install ovmf`), then as
# Homebrew's QEMU ships it (Apple Silicon, then Intel prefix). $OVMF_CODE and $OVMF_VARS
# name another pair.
OVMF_CODE_CANDIDATES = ["/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_CODE.fd",
                        "/opt/homebrew/share/qemu/edk2-x86_64-code.fd",
                        "/usr/local/share/qemu/edk2-x86_64-code.fd"]


def ovmf_vars_for(code: Path) -> "Path | None":
    """The variable store that goes with a firmware image, by its name; None if it says nothing."""
    if code.name.startswith("edk2-"):  # QEMU's own build: one 32-bit vars template for both
        return code.with_name("edk2-i386-vars.fd")
    if "CODE" in code.name:
        return code.with_name(code.name.replace("CODE", "VARS"))
    return None


def find_ovmf() -> tuple[Path, Path]:
    """
    The OVMF firmware image and its variable store. $OVMF_CODE, with $OVMF_VARS or the
    store its name implies, is used alone: a missing file is an error, not a reason to
    try the others. Otherwise the first candidate whose store exists.
    """
    env_code, env_vars = os.environ.get("OVMF_CODE"), os.environ.get("OVMF_VARS")
    if env_vars and not env_code:
        sys.exit("run_tests: OVMF_VARS needs OVMF_CODE, the image it goes with")
    if env_code:
        code = Path(env_code)
        vars_template = Path(env_vars) if env_vars else ovmf_vars_for(code)
        if not code.is_file():
            sys.exit(f"run_tests: OVMF_CODE={code}: no such file")
        if vars_template is None or vars_template == code or not vars_template.is_file():
            sys.exit(f"run_tests: no variable store for {code}: set OVMF_VARS")
        return code, vars_template
    for code in map(Path, OVMF_CODE_CANDIDATES):
        vars_template = ovmf_vars_for(code)
        if code.is_file() and vars_template and vars_template.is_file():
            return code, vars_template
    sys.exit("run_tests: OVMF not found (apt install ovmf, or brew install qemu, or set "
             "OVMF_CODE); cannot run a UEFI test")


def ovmf_drives(workdir: Path) -> list[str]:
    """pflash drives for OVMF: read-only code plus a private copy of the variables."""
    code, vars_template = find_ovmf()
    vars_copy = workdir / "OVMF_VARS.fd"
    shutil.copyfile(vars_template, vars_copy)
    return ["-drive", f"if=pflash,format=raw,readonly=on,file={code}",
            "-drive", f"if=pflash,format=raw,file={vars_copy}"]


# sockaddr_un.sun_path, NUL included: 108 bytes on Linux, 104 on macOS and the BSDs.
SUN_PATH_MAX = 108 if sys.platform.startswith("linux") else 104

TAP_RESULT = re.compile(r"^(not ok|ok) (\d+) - (.+)$")


# $QEMU overrides the emulator, e.g. a newer build for the x2APIC run.
QEMU = os.environ.get("QEMU") or "qemu-system-x86_64"


def qemu_command(image: Path, memory: str, extra: list[str]) -> list[str]:
    return [
        QEMU,
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


# The US keyboard, as QMP qcodes: a character's key, and whether Shift is held.
QCODE_NAMED = {" ": "spc", "\n": "ret", "\t": "tab", "\b": "backspace", "\x7f": "backspace",
               "\x1b": "esc", "-": "minus", "=": "equal", "[": "bracket_left",
               "]": "bracket_right", "\\": "backslash", ";": "semicolon", "'": "apostrophe",
               "`": "grave_accent", ",": "comma", ".": "dot", "/": "slash"}
QCODE_SHIFTED = {"!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8",
                 "(": "9", ")": "0", "_": "minus", "+": "equal", "{": "bracket_left",
                 "}": "bracket_right", "|": "backslash", ":": "semicolon", '"': "apostrophe",
                 "~": "grave_accent", "<": "comma", ">": "dot", "?": "slash"}


def qcodes(ch: str) -> list[str]:
    """The keys to press together to type `ch`; ValueError if no key types it."""
    if ch in QCODE_NAMED:
        return [QCODE_NAMED[ch]]
    if ch in QCODE_SHIFTED:
        return ["shift", QCODE_NAMED.get(QCODE_SHIFTED[ch], QCODE_SHIFTED[ch])]
    if "a" <= ch <= "z" or "0" <= ch <= "9":
        return [ch]
    if "A" <= ch <= "Z":
        return ["shift", ch.lower()]
    if "\x01" <= ch <= "\x1a":
        return ["ctrl", chr(ord(ch) + 0x60)]
    raise ValueError(f"no key types {ch!r}")


class Qmp:
    """Just enough of the QEMU Machine Protocol to press keys."""

    def __init__(self, path: str, deadline: float):
        self.sock = socket.socket(socket.AF_UNIX)
        while True:
            try:
                self.sock.connect(path)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.05)
        self.sock.settimeout(max(1.0, deadline - time.monotonic()))
        self.buf = b""
        self.reply()  # the greeting
        self.command({"execute": "qmp_capabilities"})

    def reply(self) -> dict:
        while True:
            while b"\n" not in self.buf:
                data = self.sock.recv(4096)
                if not data:
                    raise OSError("QMP connection closed")
                self.buf += data
            line, self.buf = self.buf.split(b"\n", 1)
            msg = json.loads(line)
            if "event" not in msg:  # events (e.g. RESET) are not replies
                return msg

    def command(self, cmd: dict) -> None:
        self.sock.sendall(json.dumps(cmd).encode() + b"\n")
        msg = self.reply()
        if "error" in msg:
            raise OSError(f"QMP {cmd['execute']}: {msg['error']}")

    def type(self, text: str) -> None:
        for ch in text:
            keys = [{"type": "qcode", "data": k} for k in qcodes(ch)]
            self.command({"execute": "send-key", "arguments": {"keys": keys}})


def typing_steps(ap, args) -> list[tuple[re.Pattern, str]]:
    """Checks every --type-after/--type pair before QEMU starts; a bad one is a usage error."""
    steps = []
    for pattern, raw in zip(args.type_after, args.type):
        try:
            regex = re.compile(pattern, re.MULTILINE)
            text = codecs.decode(raw, "unicode_escape")
            if args.input == "serial":
                text.encode("latin-1")
            else:
                for ch in text:
                    qcodes(ch)
        except (re.error, UnicodeError, ValueError) as e:
            ap.error(f"cannot type {raw!r} after /{pattern}/: {e}")
        steps.append((regex, text))
    return steps


def run_typing(cmd: list[str], args, steps, workdir: Path):
    """Runs QEMU while typing the --type steps. Returns (status, timed_out, out, err, problems)."""
    deadline = time.monotonic() + args.timeout
    qmp_path = str(workdir / "qmp.sock")
    if args.input == "ps2":
        if len(qmp_path.encode()) >= SUN_PATH_MAX:
            sys.exit(f"run_tests: QMP socket path too long for AF_UNIX ({SUN_PATH_MAX - 1} bytes "
                     f"at most here): {qmp_path}; set TMPDIR to a shorter directory, e.g. /tmp")
        cmd = cmd + ["-qmp", f"unix:{qmp_path},server=on,wait=off"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            stdin=subprocess.PIPE if args.input == "serial" else subprocess.DEVNULL)
    out, err, lock = bytearray(), bytearray(), threading.Lock()

    def collect(stream, buf):
        for chunk in iter(lambda: stream.read1(4096), b""):
            with lock:
                buf.extend(chunk)

    readers = [threading.Thread(target=collect, args=(proc.stdout, out), daemon=True),
               threading.Thread(target=collect, args=(proc.stderr, err), daemon=True)]
    for t in readers:
        t.start()
    problems, qmp, pos = [], None, 0
    status, timed_out = None, False
    try:
        for k, (regex, text) in enumerate(steps, 1):
            pattern = regex.pattern
            while True:
                with lock:
                    now = out.decode("utf-8", errors="replace").replace("\r\n", "\n")
                if m := regex.search(now, pos):
                    pos = m.end()
                    break
                if proc.poll() is not None:
                    problems.append(f"prompt /{pattern}/ never appeared before QEMU exited; "
                                    f"did not type step {k}, {text!r}")
                    break
                if time.monotonic() > deadline:
                    problems.append(f"prompt /{pattern}/ never appeared in {args.timeout:.0f}s; "
                                    f"did not type step {k}, {text!r}")
                    break
                time.sleep(0.01)
            if problems:
                break
            try:
                if args.input == "serial":
                    proc.stdin.write(text.encode("latin-1"))
                    proc.stdin.flush()
                else:
                    if qmp is None:
                        qmp = Qmp(qmp_path, deadline)
                    qmp.type(text)
            except OSError as e:
                problems.append(f"QEMU stopped taking input at step {k} ({e})")
                break
        if not problems:  # after a problem nothing more will come: do not wait out the timeout
            status = proc.wait(timeout=max(0.1, deadline - time.monotonic()))
    except subprocess.TimeoutExpired:
        timed_out = True
    finally:
        if qmp is not None:
            qmp.sock.close()
        if proc.poll() is None:  # never leave a QEMU behind, whatever went wrong
            proc.kill()
        if status is None:
            status = proc.wait()
    for t in readers:
        t.join(timeout=5)
    return status, timed_out, bytes(out), bytes(err), problems


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
    if sys.argv[1:] == ["--print-ovmf"]:  # for make run-uefi: the same lookup, as "CODE VARS"
        print(*find_ovmf())
        return 0
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
    ap.add_argument("--reject", action="append", default=[],
                    help="regex the output must not match anywhere; repeatable")
    ap.add_argument("--stdin-file", type=Path,
                    help="feed this file to QEMU's stdin, which reaches COM1 as typed bytes")
    ap.add_argument("--type-after", action="append", default=[], metavar="REGEX",
                    help="wait for REGEX after the previous match, then type the paired --type")
    ap.add_argument("--type", action="append", default=[], metavar="TEXT",
                    help="text to type (backslash escapes allowed); pairs with --type-after")
    ap.add_argument("--input", choices=["ps2", "serial"], default="ps2",
                    help="type on the PS/2 keyboard (QMP send-key) or into COM1")
    ap.add_argument("qemu_args", nargs="*", help="extra QEMU arguments (after --)")
    args = ap.parse_args()
    if len(args.type_after) != len(args.type):
        ap.error("--type-after and --type come in pairs")
    if args.type and args.stdin_file:
        ap.error("--stdin-file and --type cannot be combined")
    steps = typing_steps(ap, args)

    args.log_dir.mkdir(parents=True, exist_ok=True)
    suffix = f"-{args.log_suffix}" if args.log_suffix else ""
    log_path = args.log_dir / f"{args.image.stem}-{args.memory}{suffix}.serial.log"
    workdir = Path(tempfile.mkdtemp(prefix="minios-qemu-"))
    start = time.monotonic()
    typing_problems = []
    try:
        firmware = ovmf_drives(workdir) if args.uefi else []
        cmd = qemu_command(args.image, args.memory, firmware + args.qemu_args)
        if steps:
            status, timed_out, output, stderr, typing_problems = run_typing(cmd, args, steps,
                                                                            workdir)
        else:
            try:
                # COM1 receives: stray terminal input must not reach the guest.
                with open(args.stdin_file or os.devnull, "rb") as stdin:
                    proc = subprocess.run(cmd, capture_output=True, timeout=args.timeout,
                                          stdin=stdin)
                status, timed_out = proc.returncode, False
                output, stderr = proc.stdout, proc.stderr
            except subprocess.TimeoutExpired as e:
                status, timed_out = None, True
                output, stderr = e.stdout or b"", e.stderr or b""
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    elapsed = time.monotonic() - start

    text = output.decode("utf-8", errors="replace").replace("\r\n", "\n")
    log_path.write_text(text)
    lines = text.splitlines()
    plan, results = parse_tap(lines)

    # Echo the run so CI logs are self-contained.
    for line in lines:
        print(f"  | {line}")

    problems = list(typing_problems)
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
    for pattern in args.reject:
        if m := re.compile(pattern, re.MULTILINE).search(text):
            problems.append(f"output matches rejected /{pattern}/: {m.group(0)!r}")

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
        checks = len(args.expect) + len(args.reject)
        what = (f"{checks} expectations" if problems
                else f"exited cleanly, {checks} expectations met")
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
