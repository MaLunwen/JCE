"""Matte-clear, rig-clock and unstable repeat captures must fail."""
import unittest
import numpy as np
from gbuffer_asset_suite import validate_box, validate_skin


class NormalControls(unittest.TestCase):
    def test_unit_box_requires_exact_nonblank_rgba(self):
        normal = np.full((10, 10, 4), 128, dtype=np.uint8)
        self.assertTrue(validate_box(normal, normal)["exact_rgba_match"])
        blank = normal.copy()
        blank[:, :, 3] = 255
        altered = normal.copy()
        altered[0, 0, 0] += 1
        for wrong in (blank, altered):
            with self.assertRaises(ValueError):
                validate_box(normal, wrong)

    def test_animation_requires_empty_frozen_and_repeat_controls(self):
        normal = np.full((10, 10, 4), 128, dtype=np.uint8)
        moved = normal.copy()
        moved[0, 0, 0] = 180
        empty = normal.copy()
        empty[:, :, 3] = 255
        images = {"empty": empty, "frozen20": normal, "frozen50": normal,
                  "moving20": normal, "moving50": moved, "repeat50": moved}
        self.assertEqual(validate_skin(images)["moving_delta"], .01)
        for key, wrong in (("empty", normal), ("frozen50", moved),
                           ("moving50", normal), ("repeat50", normal)):
            invalid = dict(images)
            invalid[key] = wrong
            with self.assertRaises(ValueError):
                validate_skin(invalid)


if __name__ == "__main__":
    unittest.main()
