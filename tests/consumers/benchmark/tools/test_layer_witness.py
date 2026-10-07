"""Negative controls for submission and light-layer comparisons."""
import unittest

from check_layer_witness import submission_difference, validate_layer_deltas


class LayerWitnessTests(unittest.TestCase):
    def test_only_masked_receivers_may_change(self):
        validate_layer_deltas([0, 10] * 50)
        for values in ([0, 10] * 49, [1, 10] * 50, [0, 0] * 50, [0, -1] * 50):
            with self.assertRaises(ValueError):
                validate_layer_deltas(values)

    def test_parity_retains_small_differences_and_rejects_wrong_pixels(self):
        a = (2, 1, 3, bytes([10, 20, 30, 10, 20, 30]))
        b = (2, 1, 3, bytes([11, 20, 30, 10, 20, 30]))
        result = submission_difference(a, b)
        self.assertFalse(result["exact_rgb_equal"])
        self.assertEqual(result["maximum_channel_difference"], 1)
        self.assertEqual(result["nonidentical_channels"], 1)
        with self.assertRaises(ValueError):
            submission_difference(a, (2, 1, 3, bytes([255] * 6)))


if __name__ == "__main__":
    unittest.main()
