"""A matrix report must not promote missing, repeated or mixed-build evidence."""
import copy
import unittest

from report_gpu_motion import BACKENDS, MOTIONS, validate_inventory


class MotionReportTests(unittest.TestCase):
    def entries(self):
        return [{"motion": motion, "backend": backend, "executable_sha256": "one build"}
                for motion in MOTIONS for backend in BACKENDS]

    def test_complete_matrix_required(self):
        entries = self.entries()
        validate_inventory(entries)
        with self.assertRaises(ValueError):
            validate_inventory(entries[:-1])
        entries[-1] = dict(entries[0])
        with self.assertRaises(ValueError):
            validate_inventory(entries)

    def test_mixed_executable_cannot_be_one_matrix(self):
        entries = copy.deepcopy(self.entries())
        entries[-1]["executable_sha256"] = "a different build"
        with self.assertRaises(ValueError):
            validate_inventory(entries)


if __name__ == "__main__":
    unittest.main()
