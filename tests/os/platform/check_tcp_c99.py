"""Compile the owned POSIX TCP adapter with a real strict-C99 compiler."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
FEATURE_SELECTION = """/* Feature selection must precede every header under strict ISO C99. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200112L
#endif
"""


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux C99 compiler probe unavailable on this host")
class TcpC99Tests(unittest.TestCase):
    def test_feature_selection_and_missing_selection_negative_control(self):
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler, "Linux regression requires a C compiler")
        source = ROOT / "engine/src/os/platform/jce_tcp.c"
        original = source.read_text(encoding="utf-8")
        self.assertIn(FEATURE_SELECTION, original)
        output = ROOT / "build/tests/tcp-c99"
        output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as temporary:
            negative = Path(temporary) / "tcp_without_feature_selection.c"
            negative.write_text(original.replace(FEATURE_SELECTION, "", 1), encoding="utf-8", newline="\n")
            flags = [compiler, "-std=c99", "-Werror=implicit-function-declaration",
                     "-fsyntax-only", "-I", str(ROOT / "engine/include")]
            valid = subprocess.run([*flags, str(source)], capture_output=True, text=True)
            self.assertEqual(valid.returncode, 0, valid.stdout + valid.stderr)
            broken = subprocess.run([*flags, str(negative)], capture_output=True, text=True)
            self.assertNotEqual(broken.returncode, 0, "negative control unexpectedly compiled")
            self.assertIn("addrinfo", broken.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
