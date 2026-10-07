"""Geometric oracle is independent of rendered shadow images."""
import unittest
import numpy as np
from shadow_projection import analyze, hull, oracle
from shadow_suite import HEIGHT, WIDTH, scene


class ProjectionControls(unittest.TestCase):
    def test_geometry_and_negative_pixels(self):
        expected, footprint, domain = oracle(scene("on"))
        self.assertGreater(expected.sum(), 1000)
        self.assertFalse((expected & footprint).any())
        off = np.full((HEIGHT, WIDTH, 4), 150, np.uint8)
        on = off.copy()
        on[expected, :3] = 80
        correct, _ = analyze(expected, domain, off, on, 2)
        self.assertEqual(correct["expected_covered_fraction"], 1)
        zero, _ = analyze(expected, domain, off, off, 2)
        self.assertEqual(zero["observed_pixels"], 0)
        flipped, _ = analyze(expected, domain, on, off, 2)
        self.assertEqual(flipped["observed_pixels"], 0)
        shifted = off.copy()
        shifted[np.roll(expected, 15, axis=1), :3] = 80
        wrong, _ = analyze(expected, domain, off, shifted, 2)
        self.assertLess(wrong["covered_with_tolerance_fraction"], 1)
        self.assertGreater(wrong["false_positive_pixels"], 0)
        self.assertEqual(len(hull([(0,0),(1,0),(1,1),(0,1),(.5,.5)])), 4)


if __name__ == "__main__":
    unittest.main()
