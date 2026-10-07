"""Actual shared binary wrapper C format, explicit already-built host tool."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
EXE = None
class WrapperTests(unittest.TestCase):
    def setUp(self):
        if EXE is None:
            self.skipTest("explicit --exe required for actual host wrapper")
    def run_wrapper(self, data, symbol="stock"):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            path = Path(temporary)
            (path / "input.bin").write_bytes(data)
            result = subprocess.run([str(EXE), "--input", str(path / "input.bin"),
                "--output", str(path / "output.c"), "--symbol", symbol, "--format", "c"],
                capture_output=True, text=True)
            return result, (path / "output.c").read_text() if (path / "output.c").exists() else ""
    def test_all_bytes_and_empty_payload_are_exact(self):
        for data in (bytes(range(256)), b""):
            result, source = self.run_wrapper(data)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(bytes(int(n, 16) for n in re.findall(r"0x([0-9a-f]{2})", source)), data)
            self.assertIn("const size_t stock_size = %d;" % len(data), source)
            self.assertNotIn(str(EXE), source)
    def test_invalid_symbol_cannot_inject_source(self):
        for symbol in ("1bad", "x;evil", "x\n#error injected"):
            result, source = self.run_wrapper(b"data", symbol)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(source, "")
if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    args, remaining = parser.parse_known_args()
    EXE = args.exe.resolve(strict=True)
    unittest.main(argv=[__file__] + remaining)
