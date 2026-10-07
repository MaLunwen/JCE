"""Raw reports reject edited CSVs and invented percentile summaries."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from report_frames import frame_summary, load_trials, sha256_file


class FrameReportTests(unittest.TestCase):
    def test_raw_identity_and_recalculated_summary_are_required(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            raw, intent, suite = (root / name for name in ("frames.csv", "intent.json", "suite.json"))
            raw.write_bytes(b"raw-clock-fixture")
            intent.write_text(json.dumps({"warmup_intervals": 120, "measured_intervals": 3}), encoding="utf-8")
            samples = {"frame_ms": [1, 2, 4], "cpu_frame_ms": [], "gpu_frame_ms": [], "phase_ms": {}}
            row = {"directory": str(root), "raw_sha256": sha256_file(raw), "frame": frame_summary(samples)}
            document = {"status": "PASS", "runs": [row]}
            with mock.patch("report_frames.read_frames", return_value=samples) as reader:
                suite.write_text(json.dumps(document), encoding="utf-8")
                _, rows = load_trials(suite)
                self.assertEqual(rows[0]["frame"]["frame_median_ms"], 2)
                reader.assert_called_once_with(raw, warmup=120, sample_count=3)
                row["frame"]["frame_p99_ms"] = 99
                suite.write_text(json.dumps(document), encoding="utf-8")
                with self.assertRaises(ValueError):
                    load_trials(suite)
                row["frame"] = frame_summary(samples)
                suite.write_text(json.dumps(document), encoding="utf-8")
                raw.write_bytes(b"edited")
                with self.assertRaises(ValueError):
                    load_trials(suite)


if __name__ == "__main__":
    unittest.main()
