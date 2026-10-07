"""Distance-route evidence must include real, ordered mesh-only selections."""
import unittest

from lod_suite import camera_z, expected_route, validate


def marker_lines():
    return ["benchmark-lod: frame=%u enabled=1 levels=3 picks=%u,%u,%u "
            "culled=0 pos=0.000000,20.000000,%.6f" %
            (frame, *counts, camera_z(frame))
            for frame, counts in enumerate(expected_route(), 1)]


class LodEvidence(unittest.TestCase):
    def test_complete_route_and_non_mesh_negative_control(self):
        lines = marker_lines()
        rows = validate("\n".join(lines), True)
        self.assertEqual(rows[0][3:6], [64, 0, 0])
        self.assertEqual(rows[79][3:6], [0, 0, 64])
        self.assertEqual(rows[159][3:6], [64, 0, 0])
        for wrong in (lines[1:], lines + [lines[-1]],
                      [lines[0].replace("picks=64,0,0", "picks=66,0,0")] + lines[1:],
                      [lines[0].replace("pos=0.000000", "pos=1.000000")] + lines[1:]):
            with self.assertRaises(ValueError):
                validate("\n".join(wrong), True)


if __name__ == "__main__":
    unittest.main()
