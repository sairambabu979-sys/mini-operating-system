#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build a UEFI boot image: a FAT32 EFI system partition as a raw disk.

    /EFI/BOOT/BOOTX64.EFI   the loader (the default removable-media boot path)
    /minios/kernel.elf      the same stripped kernel the BIOS images carry
    /minios/cmdline         the kernel command line
    /minios/initrd          the initrd, if given

Uses mtools, so no root and no loop devices. The image is a "superfloppy"
(no partition table), which OVMF and most firmware boot from USB media.
"""

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

SIZE_MIB = 34  # smallest comfortable FAT32 volume


def run(*cmd: str) -> None:
    env = dict(os.environ, SOURCE_DATE_EPOCH="0", MTOOLS_SKIP_CHECK="1")
    subprocess.run(cmd, check=True, env=env)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--loader", type=Path, required=True)
    ap.add_argument("--kernel", type=Path, required=True)
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--initrd", type=Path)
    ap.add_argument("-o", "--output", type=Path, required=True)
    args = ap.parse_args()

    tmp = args.output.with_suffix(".tmp")
    with open(tmp, "wb") as f:
        f.truncate(SIZE_MIB << 20)
    img = f"{tmp}"
    run("mformat", "-i", img, "-F", "-N", "4d494e49", "-v", "MINIOS", "::")
    run("mmd", "-i", img, "::/EFI", "::/EFI/BOOT", "::/minios")
    run("mcopy", "-i", img, str(args.loader), "::/EFI/BOOT/BOOTX64.EFI")
    run("mcopy", "-i", img, str(args.kernel), "::/minios/kernel.elf")
    if args.initrd:
        run("mcopy", "-i", img, str(args.initrd), "::/minios/initrd")
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as c:
        c.write(args.cmdline + "\n")
    try:
        run("mcopy", "-i", img, c.name, "::/minios/cmdline")
    finally:
        os.unlink(c.name)
    tmp.replace(args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
