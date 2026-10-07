"""Edited captures and failed suites cannot become a history-fix report."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from report_occlusion import REVEAL_FRAMES, capture_history, sha256_file


class OcclusionReportTests(unittest.TestCase):
    def test_raw_image_hash_and_complete_pass_are_required(self):
        scratch = Path(__file__).resolve().parents[4] / "build/tests/benchmark-tool-regressions"
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch, prefix="occlusion-report-") as temporary:
            root = Path(temporary)
            self.assertEqual(root.resolve().parent, scratch.resolve())
            captures = {}
            labels = ["empty"] + ["reveal-%s-%u" % (mode, frame)
                       for frame in REVEAL_FRAMES for mode in ("on", "off")]
            for label in labels:
                path = root / (label + ".png")
                path.write_bytes(b"mock-png-fixture")
                captures[label] = {"image": str(path), "image_sha256": sha256_file(path)}
            suite = root / "suite.json"
            data = {"status": "PASS", "count": 4, "captures": captures}
            suite.write_text(json.dumps(data), encoding="utf-8")
            def pixels(path):
                return (128, 72, 3, bytes([0 if path.stem == "empty" else 255]) * (128 * 72 * 3))
            with mock.patch("report_occlusion.read_png", side_effect=pixels):
                _, history, _ = capture_history(suite, require_repaired=True)
                self.assertFalse(any(history.values()))
                Path(captures["reveal-on-80"]["image"]).write_bytes(b"changed")
                with self.assertRaises(ValueError):
                    capture_history(suite, require_repaired=True)
            data["status"] = "FAILED"
            suite.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(ValueError):
                capture_history(suite)


if __name__ == "__main__":
    unittest.main()
