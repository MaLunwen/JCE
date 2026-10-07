"""Actual CMake checksum writer: explicit output path and immutable input."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CMAKE = shutil.which('cmake')


@unittest.skipUnless(CMAKE, 'CMake executable unavailable')
class HashWriter(unittest.TestCase):
    def setUp(self):
        base = ROOT / 'build/tests/checksum-writer'
        base.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=base)
        self.root = Path(self.temp.name).resolve()
        assert self.root.is_relative_to(base.resolve())
        self.input = self.root / 'input.bin'
        self.input.write_bytes(bytes(range(256)))

    def tearDown(self):
        self.temp.cleanup()

    def run_writer(self, source, output):
        return subprocess.run([CMAKE, '-DFILE=' + str(source), '-DOUT=' + str(output),
                               '-P', str(ROOT / 'tools/write_sha256.cmake')],
                              cwd=self.root, capture_output=True, timeout=20)

    def test_nested_unicode_output_is_created_and_lf(self):
        output = self.root / 'nested \u4e2d\u6587' / 'receipt.txt'
        result = self.run_writer(self.input, output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output.read_bytes(), hashlib.sha256(self.input.read_bytes()).hexdigest().encode() + b'\n')
        self.assertEqual(self.input.read_bytes(), bytes(range(256)))

    def test_relative_filename_and_changed_input(self):
        self.assertEqual(self.run_writer(self.input, 'receipt.txt').returncode, 0)
        before = (self.root / 'receipt.txt').read_bytes()
        self.input.write_bytes(b'changed input')
        self.assertEqual(self.run_writer(self.input, 'receipt.txt').returncode, 0)
        self.assertNotEqual((self.root / 'receipt.txt').read_bytes(), before)
        self.assertEqual((self.root / 'receipt.txt').read_bytes(), hashlib.sha256(b'changed input').hexdigest().encode() + b'\n')

    def test_missing_input_fails_without_receipt(self):
        output = self.root / 'must-not-exist.txt'
        self.assertNotEqual(self.run_writer(self.root / 'missing.bin', output).returncode, 0)
        self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
