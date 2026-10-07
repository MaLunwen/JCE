"""Normal reports reject replaced binaries before interpreting captures."""
import json
from pathlib import Path
import tempfile
import unittest

from report_gbuffer_assets import load, sha256_file


class ReportIdentity(unittest.TestCase):
    def test_changed_executable_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "consumer"
            executable.write_bytes(b"tested binary")
            suite = root / "suite.json"
            suite.write_text(json.dumps({"status": "GBUFFER_ASSET_CONTROLS_PASS",
                "executable": str(executable),
                "executable_sha256": sha256_file(executable)}), encoding="utf-8")
            executable.write_bytes(b"different binary")
            with self.assertRaisesRegex(ValueError, "raw input changed"):
                load(suite)


if __name__ == "__main__":
    unittest.main()
