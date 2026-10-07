import unittest
from unittest.mock import patch
from pathlib import Path
from report_particle_capacity import matrix, load, checked


class ReportControls(unittest.TestCase):
    def test_matrix_binary_backend_negative_controls(self):
        rows = [({"backend": b,"executable_sha256":"same"},{}) for b in ("d3d11","d3d12","vulkan","gl")]
        matrix(rows)
        with self.assertRaises(ValueError): matrix(rows[:3])
        rows[-1][0]["executable_sha256"] = "changed"
        with self.assertRaises(ValueError): matrix(rows)
        rows[-1][0]["executable_sha256"] = "same"
        rows[-1][0]["backend"] = "d3d11"
        with self.assertRaises(ValueError): matrix(rows)

    def test_raw_hash_tampering_rejected(self):
        with patch("report_lights.sha256_file", return_value="changed"):
            with self.assertRaisesRegex(ValueError, "raw input changed"):
                checked(Path("capture.png"), "original")

    def test_fail_and_incomplete_suite_rejected(self):
        import json
        for value in ({"status":"FAIL"}, {"status":"GPU_CAPACITY_CONTROLS_PASS", "cases": {}}):
            with patch.object(Path,"read_text",return_value=json.dumps(value)):
                with self.assertRaises(ValueError): load(Path("suite.json"))


if __name__ == "__main__": unittest.main()
