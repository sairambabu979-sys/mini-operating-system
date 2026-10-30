#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Assemble a bootable raw disk image.

Layout (see docs/design/boot.md):

    LBA 0           stage 1 (MBR) with one active partition entry
    LBA 1..63       stage 2, header patched with the kernel location/cmdline
    LBA 64..        kernel ELF, padded to a whole sector (to a whole page
                    when an initrd follows, so the initrd lands on one)
    then            the initrd, if any, padded to a whole sector

The image is deterministic: identical inputs produce identical bytes.
"""

import argparse
import struct
import sys
from pathlib import Path

SECTOR = 512
STAGE2_LBA = 1
STAGE2_SECTORS = 63
KERNEL_LBA = STAGE2_LBA + STAGE2_SECTORS

STAGE2_MAGIC = b"S2HD"
STAGE2_HEADER_OFF = 4
# struct { u32 magic; u32 kernel_lba; u32 kernel_sectors; char cmdline[128];
#          u32 initrd_sectors; u32 initrd_size; }
STAGE2_HEADER = struct.Struct("<4sII128sII")
CMDLINE_MAX = 128
PAGE_SECTORS = 8
# Stage 2 stages the kernel and the initrd from 16 MiB, and must end below 4 GiB.
STAGING_MAX_SECTORS = ((1 << 32) - (16 << 20)) // SECTOR

PARTITION_TABLE_OFF = 446
PARTITION_TYPE = 0x7F  # "reserved for individual or local use"


def die(msg: str) -> None:
    print(f"mkimage: error: {msg}", file=sys.stderr)
    sys.exit(1)


def partition_entry(lba_start: int, sectors: int) -> bytes:
    # CHS fields set to the "use LBA" sentinel (FE FF FF).
    chs_lba = bytes([0xFE, 0xFF, 0xFF])
    return struct.pack("<B3sB3sII", 0x80, chs_lba, PARTITION_TYPE, chs_lba, lba_start, sectors)


def sectors(data: bytes, multiple: int = 1) -> int:
    n = -(-len(data) // SECTOR)
    return -(-n // multiple) * multiple


def build(stage1: bytes, stage2: bytes, kernel: bytes, cmdline: str, initrd: bytes) -> bytes:
    if len(stage1) != SECTOR or stage1[510:512] != b"\x55\xAA":
        die("stage 1 must be exactly 512 bytes ending in 0x55AA")
    if any(stage1[PARTITION_TABLE_OFF:510]):
        die("stage 1 partition table area must be zero (mkimage fills it)")
    if len(stage2) != STAGE2_SECTORS * SECTOR:
        die(f"stage 2 must be exactly {STAGE2_SECTORS} sectors, got {len(stage2)} bytes")
    if kernel[:4] != b"\x7fELF":
        die("kernel is not an ELF file")

    encoded = cmdline.encode("ascii")
    if len(encoded) >= CMDLINE_MAX:
        die(f"command line longer than {CMDLINE_MAX - 1} bytes")

    magic, *_ = STAGE2_HEADER.unpack_from(stage2, STAGE2_HEADER_OFF)
    if magic != STAGE2_MAGIC:
        die(f"stage 2 header magic not found at offset {STAGE2_HEADER_OFF}")

    kernel_sectors = sectors(kernel, PAGE_SECTORS if initrd else 1)
    initrd_sectors = sectors(initrd)
    if kernel_sectors + initrd_sectors >= STAGING_MAX_SECTORS:
        die("kernel and initrd too large for stage 2's staging area")
    total_sectors = KERNEL_LBA + kernel_sectors + initrd_sectors

    s1 = bytearray(stage1)
    s1[PARTITION_TABLE_OFF : PARTITION_TABLE_OFF + 16] = partition_entry(1, total_sectors - 1)

    s2 = bytearray(stage2)
    STAGE2_HEADER.pack_into(s2, STAGE2_HEADER_OFF, STAGE2_MAGIC, KERNEL_LBA, kernel_sectors,
                            encoded.ljust(CMDLINE_MAX, b"\0"), initrd_sectors, len(initrd))

    image = (bytes(s1) + bytes(s2) + kernel.ljust(kernel_sectors * SECTOR, b"\0") +
             initrd.ljust(initrd_sectors * SECTOR, b"\0"))
    assert len(image) == total_sectors * SECTOR
    return image


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--stage1", type=Path, required=True)
    ap.add_argument("--stage2", type=Path, required=True)
    ap.add_argument("--kernel", type=Path, required=True)
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--initrd", type=Path, help="archive stage 2 loads after the kernel")
    ap.add_argument("-o", "--output", type=Path, required=True)
    args = ap.parse_args()

    initrd = args.initrd.read_bytes() if args.initrd else b""
    if args.initrd and not initrd:
        die(f"{args.initrd} is empty (leave out --initrd for no initrd)")
    image = build(args.stage1.read_bytes(), args.stage2.read_bytes(), args.kernel.read_bytes(),
                  args.cmdline, initrd)
    args.output.write_bytes(image)


if __name__ == "__main__":
    main()
