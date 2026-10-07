"""Live startup observations cannot be stale or inferred after child exit."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

from tools.process_sampler import _process_reader, run_process


class LiveMarkerTests(unittest.TestCase):
    def scratch(self):
        root = Path(__file__).resolve().parents[2] / "build/tests/process-sampler"
        root.mkdir(parents=True, exist_ok=True)
        return tempfile.TemporaryDirectory(dir=root)

    def test_real_live_marker_and_missing_control(self):
        with self.scratch() as temporary:
            root = Path(temporary)
            path = root / "ready"
            result = run_process([sys.executable, "-c",
                "from pathlib import Path; import time; "
                "Path('ready').write_bytes(b'ready'); time.sleep(0.3)"],
                env=os.environ.copy(), cwd=root, timeout=5, output_dir=root,
                interval=0.02, live_markers={"ready": (path, b"ready"),
                    "missing": (root / "missing", b"ready")})
            self.assertEqual(result["returncode"], 0)
            self.assertGreater(result["live_markers_s"]["ready"], 0)
            self.assertLess(result["live_markers_s"]["ready"], result["elapsed_s"])
            self.assertIsNone(result["live_markers_s"]["missing"])
            with self.assertRaises(ValueError):
                run_process(["unused"], env={}, cwd=root, timeout=5,
                    output_dir=root, live_markers={"stale": (path, b"ready")})

    def test_exit_only_marker_cannot_become_startup_latency(self):
        with self.scratch() as temporary:
            root = Path(temporary)
            child = mock.Mock(returncode=0)
            child.poll.return_value = 0
            with mock.patch("tools.process_sampler.subprocess.Popen", return_value=child), \
                    mock.patch("tools.process_sampler._process_reader", return_value=None), \
                    mock.patch("tools.process_sampler._has_live_marker", return_value=True) as check:
                result = run_process(["unused"], env={}, cwd=root, timeout=5,
                    output_dir=root, live_markers={"ready": (root / "ready", b"ready")})
            check.assert_not_called()
            self.assertIsNone(result["live_markers_s"]["ready"])

    def test_exit_during_initial_lookup_retains_exit_and_logs(self):
        class Gone(Exception):
            pass
        psutil = SimpleNamespace(Error=Gone, Process=mock.Mock(side_effect=Gone("already exited")))
        with self.scratch() as temporary, mock.patch.dict(sys.modules, psutil=psutil):
            root = Path(temporary)
            result = run_process([sys.executable, "-c", "print('early exit'); raise SystemExit(7)"],
                                 env=os.environ.copy(), cwd=root, timeout=5, output_dir=root)
            self.assertEqual(result["returncode"], 7)
            self.assertFalse(result["timed_out"])
            self.assertFalse(result["sampling_available"])
            self.assertIsNone(result["peak_rss_bytes"])
            self.assertIsNone(result["cpu_time_s"])
            self.assertIn("early exit", result["stdout"])
            self.assertTrue((root / "result.json").is_file())

    def test_initial_lookup_does_not_hide_programming_errors(self):
        psutil = SimpleNamespace(Error=OSError, Process=mock.Mock(side_effect=RuntimeError("bug")))
        with mock.patch.dict(sys.modules, psutil=psutil), self.assertRaises(RuntimeError):
            _process_reader(123)


if __name__ == "__main__":
    unittest.main()
