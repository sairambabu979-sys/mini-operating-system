"""The archive builder's public command-line contract."""
import io
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class InitrdTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.src = self.root / "payload"
        self.src.write_bytes(b"hello\n")
        self.output = self.root / "initrd.tar"

    def build(self, *names):
        return subprocess.run(
            [sys.executable, str(ROOT / "tools/mkinitrd.py"), "-o", str(self.output),
             *(f"{name}={self.src}" for name in names)],
            capture_output=True, text=True)

    def test_deterministic_sorted_archive_and_metadata(self):
        self.assertEqual(self.build("etc/motd", "bin/hello").returncode, 0)
        first = self.output.read_bytes()
        self.assertEqual(self.build("bin/hello", "etc/motd").returncode, 0)
        self.assertEqual(first, self.output.read_bytes())
        with tarfile.open(fileobj=io.BytesIO(first)) as tar:
            self.assertEqual(tar.getnames(), ["bin", "bin/hello", "etc", "etc/motd"])
            for member in tar:
                self.assertEqual((member.uid, member.gid, member.mtime), (0, 0, 0))
            self.assertEqual(tar.getmember("bin/hello").mode, 0o755)
            self.assertEqual(tar.getmember("etc/motd").mode, 0o644)
            self.assertEqual(tar.extractfile("bin/hello").read(), b"hello\n")

    def test_invalid_paths_and_collisions_leave_existing_output_intact(self):
        self.output.write_bytes(b"previous archive")
        cases = [(x,) for x in ("", "/x", "./x", "a//b", "a/../b", "a/./b", "x/")]
        cases += [("same", "same"), ("a", "a/b"), ("a/b", "a")]
        for names in cases:
            with self.subTest(names=names):
                self.assertNotEqual(self.build(*names).returncode, 0)
                self.assertEqual(self.output.read_bytes(), b"previous archive")


if __name__ == "__main__":
    unittest.main()
