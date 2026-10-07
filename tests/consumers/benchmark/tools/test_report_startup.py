"""Startup reports reject changed inputs and impossible live timing evidence."""
import json
from pathlib import Path
import tempfile
import unittest

from report_startup import sha256_file, startup_rows

SCRATCH = Path(__file__).resolve().parents[4] / "build/tests/benchmark-tool-regressions"


class StartupReportTests(unittest.TestCase):
    def fixture(self, root):
        (root / "profile.ini").write_text("[renderer]\nvsync=true\n", encoding="utf-8", newline="\n")
        trial = root / "trial"
        trial.mkdir()
        (trial / "profile.ini").write_bytes((root / "profile.ini").read_bytes())
        (trial / "frame.png").write_bytes(b"previously validated capture bytes")
        (trial / "frames.csv").write_bytes(b"previously measured raw intervals")
        (trial / "intent.json").write_text(json.dumps({"executable_sha256": "binary"}), encoding="utf-8")
        process = dict(returncode=0, timed_out=False, elapsed_s=4, sampling_available=True,
                       marker_sampling_interval_s=0.02, peak_rss_bytes=1024, peak_commit_bytes=2048,
                       live_markers_s={"application_ready": 1, "first_gpu_readback": 2})
        entry = dict(directory=str(trial), image_sha256=sha256_file(trial / "frame.png"),
                     raw_sha256=sha256_file(trial / "frames.csv"),
                     profile_after_sha256=sha256_file(trial / "profile.ini"), process={"process": process})
        return dict(profile_sha256=sha256_file(root / "profile.ini"), executable_sha256="binary",
                    vsync="on", startup=[entry]), process, trial

    def test_rehashes_profile_capture_and_raw_intervals(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        for name in ("profile.ini", "frame.png", "frames.csv"):
            with tempfile.TemporaryDirectory(dir=SCRATCH) as temporary:
                root = Path(temporary)
                data, _, trial = self.fixture(root)
                self.assertEqual(startup_rows(data, root / "suite.json")[0]["ready_s"], 1)
                (trial / name).write_bytes(b"changed")
                with self.assertRaises(ValueError):
                    startup_rows(data, root / "suite.json")

    def test_missing_exit_only_and_reversed_markers_are_rejected(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        for ready, readback in ((None, 2), (5, 6), (3, 2), (False, 2)):
            with tempfile.TemporaryDirectory(dir=SCRATCH) as temporary:
                root = Path(temporary)
                data, process, _ = self.fixture(root)
                process["live_markers_s"] = {"application_ready": ready, "first_gpu_readback": readback}
                with self.assertRaises(ValueError):
                    startup_rows(data, root / "suite.json")

    def test_effective_vsync_mismatch_is_rejected(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=SCRATCH) as temporary:
            root = Path(temporary)
            data, _, _ = self.fixture(root)
            data["vsync"] = "off"
            with self.assertRaises(ValueError):
                startup_rows(data, root / "suite.json")


if __name__ == "__main__":
    unittest.main()
