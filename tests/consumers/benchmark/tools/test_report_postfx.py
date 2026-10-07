"""A report must reject a different binary before interpreting any pictures."""
import json
from pathlib import Path
import tempfile
import unittest

from report_postfx import LABELS, revalidate
from postfx_suite import sha256_file


class ReportIdentity(unittest.TestCase):
    def test_changed_executable_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            exe = root / "consumer"
            exe.write_bytes(b"original fixture binary")
            suite = root / "suite.json"
            suite.write_text(json.dumps({"status": "POSTFX_LIVENESS_PASS",
                "cases": dict.fromkeys(LABELS, {}), "executable_sha256": sha256_file(exe)}),
                encoding="utf-8")
            exe.write_bytes(b"different fixture binary")
            with self.assertRaisesRegex(ValueError, "archived consumer"):
                revalidate(suite, exe)


if __name__ == "__main__":
    unittest.main()
