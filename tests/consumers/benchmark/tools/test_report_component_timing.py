import unittest
from pathlib import Path
from report_component_timing import rehash, measured
from unittest.mock import patch


class IntegrityTests(unittest.TestCase):
    def test_changed_hash(self):
        with patch("report_component_timing.sha256_file", return_value="changed"):
            with self.assertRaises(ValueError):
                rehash(Path("input"), "original")

    def test_missing_frames(self):
        with patch("report_component_timing.read_frames", return_value={"frame_ms": [1.0]}):
            with self.assertRaises(ValueError):
                measured(Path("csv"), 720)


if __name__ == "__main__":
    unittest.main()
