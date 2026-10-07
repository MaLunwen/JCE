"""Independent color witness must reject corruption, dimensions and alpha."""
import unittest

import numpy as np

from media_pixel_suite import compare_rgba, reference_yuv420


class ColorWitnessTests(unittest.TestCase):
    def picture(self):
        return np.full((4, 8, 4), 255, dtype=np.uint8)

    def test_rounding_budget_and_identity(self):
        native, reference = self.picture(), self.picture()
        self.assertEqual(compare_rgba(native, reference)["max_rgb_error"], 0)
        native[:, :, :3] -= 3
        self.assertEqual(compare_rgba(native, reference)["changed_fraction"], 0)

    def test_green_corruption_is_not_accepted(self):
        native, reference = self.picture(), self.picture()
        native[:, :, 1] = 200
        result = compare_rgba(native, reference)
        self.assertGreater(result["changed_fraction"], 0.001)
        self.assertEqual(result["max_rgb_error"], 55)

    def test_alpha_and_size_are_checked(self):
        native, reference = self.picture(), self.picture()
        native[0, 0, 3] = 254
        with self.assertRaises(ValueError):
            compare_rgba(native, reference)
        with self.assertRaises(ValueError):
            compare_rgba(reference[:1], reference)

    def test_independent_inverse_range_and_odd_chroma(self):
        metadata = dict(pix_fmt="yuv420p", color_space="bt470bg", color_range="tv")
        # Three luma pixels have TWO chroma samples; the last must survive.
        pixels = reference_yuv420(bytes((16, 235, 81, 128, 90, 128, 240)), 3, 1, metadata)
        np.testing.assert_array_equal(pixels[0], ((0, 0, 0, 255), (255, 255, 255, 255), (254, 0, 0, 255)))
        metadata["color_range"] = "pc"
        pixels = reference_yuv420(bytes((16, 128, 128)), 1, 1, metadata)
        np.testing.assert_array_equal(pixels[0, 0], (16, 16, 16, 255))

    def test_reference_rejects_wrong_planes_or_unknown_format(self):
        for metadata, data in ((dict(pix_fmt="yuv420p"), b"short"),
                               (dict(pix_fmt="yuv420p10le"), bytes((16, 128, 128))),
                               (dict(pix_fmt="yuv420p", color_space="ictcp"), bytes((16, 128, 128)))):
            with self.assertRaises(ValueError):
                reference_yuv420(data, 1, 1, metadata)


if __name__ == "__main__":
    unittest.main()
