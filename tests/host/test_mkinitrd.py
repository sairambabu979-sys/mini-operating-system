#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check tools/mkinitrd.py: the archive it writes, and the paths it refuses.

test_tar.c reads the archive the build makes; this covers what the build
never feeds it: names that would leave the archive's root or clash.
"""

import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

MKINITRD = Path(__file__).resolve().parents[2] / "tools" / "mkinitrd.py"


class MkinitrdTest(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.tmp = Path(self.dir.name)
        self.src = self.tmp / "src"
        self.src.write_bytes(b"data")

    def tearDown(self) -> None:
        self.dir.cleanup()

    def run_tool(self, *specs: str, out: str = "out.tar") -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(MKINITRD), "-o", str(self.tmp / out), *specs],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_writes_files_and_their_directories_deterministically(self) -> None:
        specs = [f"etc/motd={self.src}", f"bin/hello={self.src}"]
        self.assertEqual(self.run_tool(*specs).returncode, 0)
        self.assertEqual(self.run_tool(*reversed(specs), out="again.tar").returncode, 0)
        first = (self.tmp / "out.tar").read_bytes()
        self.assertEqual(first, (self.tmp / "again.tar").read_bytes())
        with tarfile.open(self.tmp / "out.tar") as tar:
            members = {m.name: m for m in tar.getmembers()}
            self.assertEqual(list(members), ["bin", "bin/hello", "etc", "etc/motd"])
            self.assertTrue(members["bin"].isdir() and members["etc"].isdir())
            self.assertEqual(members["bin/hello"].mode, 0o755)
            self.assertEqual(members["etc/motd"].mode, 0o644)
            self.assertTrue(all(m.mtime == 0 and m.uid == 0 for m in members.values()))
            self.assertEqual(tar.extractfile("etc/motd").read(), b"data")

    def test_refuses_bad_paths(self) -> None:
        for dest in ["", "/bin/x", "bin//x", "bin/", "./x", "bin/./x", "../x", "bin/../x"]:
            with self.subTest(dest=dest):
                r = self.run_tool(f"{dest}={self.src}")
                self.assertNotEqual(r.returncode, 0)
                self.assertIn("bad DEST=SRC", r.stderr)
        self.assertIn("bad DEST=SRC", self.run_tool("no-equals-sign").stderr)

    def test_refuses_duplicates_and_clashes(self) -> None:
        r = self.run_tool(f"x={self.src}", f"x={self.src}")
        self.assertIn("given twice", r.stderr)
        r = self.run_tool(f"bin={self.src}", f"bin/x={self.src}")
        self.assertIn("both a file and the directory", r.stderr)
        self.assertFalse((self.tmp / "out.tar").exists())


if __name__ == "__main__":
    unittest.main()
