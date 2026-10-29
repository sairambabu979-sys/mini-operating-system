#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds the initrd: a ustar archive of files, read-only at boot.

  mkinitrd.py -o initrd.tar DEST=SRC [DEST=SRC ...]

Each DEST is the path inside the archive (e.g. bin/hello), SRC the file to
store there. Directories are added for every DEST's parents. The output is
deterministic: entries sorted, owner 0, mode 0644 (0755 for bin/ and
directories), mtime 0, so the same inputs give the same bytes.
"""

import argparse
import io
import sys
import tarfile
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("files", nargs="*", metavar="DEST=SRC")
    args = ap.parse_args()

    files = {}
    for spec in args.files:
        dest, sep, src = spec.partition("=")
        if not sep or not dest or dest.startswith("/") or ".." in dest.split("/"):
            ap.error(f"bad DEST=SRC: {spec!r}")
        if dest in files:
            ap.error(f"{dest} given twice")
        files[dest] = Path(src)
    dirs = {"/".join(d.split("/")[:i]) for d in files for i in range(1, d.count("/") + 1)}

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
            data = files[name].read_bytes()
            info.size = len(data)
            info.mode = 0o755 if name.startswith("bin/") else 0o644
            tar.addfile(info, io.BytesIO(data))
    args.output.write_bytes(out.getvalue())
    return 0


if __name__ == "__main__":
    sys.exit(main())
