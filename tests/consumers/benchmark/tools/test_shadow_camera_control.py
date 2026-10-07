import unittest
import numpy as np
from shadow_camera_control import MARKERS, centers, fixture, WIDTH, HEIGHT


class CameraControls(unittest.TestCase):
    def test_asymmetric_positions_and_marker_detection(self):
        self.assertEqual(len(fixture()["entities"]), 5)
        pixels = np.zeros((HEIGHT, WIDTH, 4), np.uint8)
        for name, x, z, channel in MARKERS:
            px = round(WIDTH/2+x*HEIGHT/28-.5)
            py = round(HEIGHT/2+z*HEIGHT/28-.5)
            pixels[py-10:py+11,px-10:px+11,channel] = 255
        for row in centers(pixels):
            self.assertLess(abs(row["observed_x"]-row["canonical_expected_x"]), .51)
            self.assertLess(abs(row["observed_y"]-row["canonical_expected_y"]), .51)
        with self.assertRaises(ValueError):
            centers(np.zeros_like(pixels))


if __name__ == "__main__":
    unittest.main()
