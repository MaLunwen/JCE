"""LOD report provenance rejects replacement executables and incomplete suites."""
import json
from pathlib import Path
import tempfile
import unittest

from report_lod import load, sha256_file


class ReportIdentity(unittest.TestCase):
    def test_replaced_binary_and_incomplete_matrix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "consumer"
            executable.write_bytes(b"tested executable")
            suite = root / "suite.json"
            suite.write_text(json.dumps({"status": "LOD_ROUTE_CONTROLS_PASS",
                "executable_sha256": sha256_file(executable), "cases": {}}),
                encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "complete LOD capture matrix"):
                load(suite, executable)
            executable.write_bytes(b"replacement executable")
            with self.assertRaisesRegex(ValueError, "raw input changed"):
                load(suite, executable)


if __name__ == "__main__":
    unittest.main()
