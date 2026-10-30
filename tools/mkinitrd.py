#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds the initrd: a ustar archive of files, read-only at boot.

  mkinitrd.py -o initrd.tar [--manifest DEST] DEST=SRC [DEST=SRC ...]

Each DEST is the path inside the archive (e.g. bin/hello), SRC the file to
store there. Directories are added for every DEST's parents. The output is
deterministic: entries sorted, owner 0, mode 0644 (0755 for bin/ and
directories), mtime 0, so the same inputs give the same bytes.

--manifest adds a file at DEST listing every other file, one line each:
"HASH SIZE PATH", HASH the 64-bit FNV-1a of the contents in 16 hex
digits, SIZE in decimal, sorted by PATH. Tar checksums cover only the
headers; the kernel tests check the contents against this.
"""

import argparse
import io
import sys
import tarfile
from pathlib import Path


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--manifest", metavar="DEST", help="add a hash manifest of the files at DEST")
    ap.add_argument("files", nargs="*", metavar="DEST=SRC")
    args = ap.parse_args()

    def check_dest(dest: str, what: str) -> None:
        if any(p in ("", ".", "..") for p in dest.split("/")):
            ap.error(f"bad {what}: {dest!r} (a relative path of plain names)")
        if dest in files:
            ap.error(f"{dest} given twice")

    files = {}
    for spec in args.files:
        dest, sep, src = spec.partition("=")
        if not sep:
            ap.error(f"bad DEST=SRC: {spec!r}")
        check_dest(dest, "DEST=SRC")
        files[dest] = Path(src).read_bytes()
    if args.manifest:
        check_dest(args.manifest, "--manifest")
        files[args.manifest] = "".join(f"{fnv1a64(data):016x} {len(data)} {name}\n"
                                       for name, data in sorted(files.items())).encode()
    dirs = {"/".join(d.split("/")[:i]) for d in files for i in range(1, d.count("/") + 1)}
    for clash in sorted(dirs & files.keys()):
        ap.error(f"{clash} is both a file and the directory of another")

    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for name in sorted(dirs | files.keys()):
            info = tarfile.TarInfo(name)
            info.mtime = 0
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            if name in dirs:
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                tar.addfile(info)
                continue
            data = files[name]
            info.size = len(data)
            info.mode = 0o755 if name.startswith("bin/") else 0o644
            tar.addfile(info, io.BytesIO(data))
    args.output.write_bytes(out.getvalue())
    return 0


if __name__ == "__main__":
    sys.exit(main())
