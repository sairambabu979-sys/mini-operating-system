#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Static checks on the linked kernel ELF.

These catch build mistakes that compile cleanly but corrupt state at
runtime, long before they would surface as a heisenbug:

  * SSE/AVX instructions in kernel code. The kernel never saves user FPU
    state on entry, so any vector instruction corrupts user registers.
    (-mgeneral-regs-only should make this impossible; this proves it.)
  * Segment layout: exactly three PT_LOAD segments, R-X / R-- / RW-, all
    page-aligned and in the higher half (the W^X mapping depends on it).
  * Entry point at the start of .text.
"""

import re
import subprocess
import sys

KERNEL_VMA = 0xFFFFFFFF80000000
PAGE = 0x1000
EXPECTED_FLAGS = ["R E", "R", "RW"]
VECTOR_REG = re.compile(r"%[xyz]mm\d+")


def run(*cmd: str) -> str:
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def fail(msg: str) -> None:
    print(f"check_kernel: FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def check_no_vector_instructions(elf: str) -> None:
    disasm = run("llvm-objdump", "-d", "--no-show-raw-insn", elf)
    offenders = [line.strip() for line in disasm.splitlines() if VECTOR_REG.search(line)]
    if offenders:
        fail("vector registers used in kernel code:\n  " + "\n  ".join(offenders[:10]))


def check_segments(elf: str) -> None:
    out = run("llvm-readelf", "--program-headers", "--wide", elf)
    loads = [line.split() for line in out.splitlines() if line.strip().startswith("LOAD")]
    if len(loads) != len(EXPECTED_FLAGS):
        fail(f"expected {len(EXPECTED_FLAGS)} PT_LOAD segments, found {len(loads)}")
    for fields, want in zip(loads, EXPECTED_FLAGS):
        # LOAD Offset VirtAddr PhysAddr FileSiz MemSiz Flg... Align
        vaddr, paddr = int(fields[2], 16), int(fields[3], 16)
        flags = " ".join(fields[6:-1])
        if flags != want:
            fail(f"segment at {vaddr:#x} has flags '{flags}', expected '{want}'")
        if vaddr % PAGE or paddr % PAGE:
            fail(f"segment at {vaddr:#x} is not page-aligned")
        if vaddr < KERNEL_VMA or vaddr - KERNEL_VMA != paddr:
            fail(f"segment at {vaddr:#x} is not in the kernel window (paddr {paddr:#x})")


def check_entry(elf: str) -> None:
    header = run("llvm-readelf", "--file-header", elf)
    entry = int(re.search(r"Entry point address:\s+(0x[0-9a-fA-F]+)", header).group(1), 16)
    syms = run("llvm-nm", elf)
    text_start = next(int(l.split()[0], 16) for l in syms.splitlines() if l.endswith(" __text_start"))
    if entry != text_start:
        fail(f"entry point {entry:#x} is not at the start of .text ({text_start:#x})")


def main() -> None:
    elf = sys.argv[1]
    check_no_vector_instructions(elf)
    check_segments(elf)
    check_entry(elf)
    print(f"check_kernel: {elf}: OK")


if __name__ == "__main__":
    main()
