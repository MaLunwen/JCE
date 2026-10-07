"""Fail-closed controls for the imported-character witness."""
import unittest

from check_asset_pixels import (cells_pass, cell_pixel_changes, chromatic_fraction,
                                full_pixel_difference, fixture,
                                pixels_pass, validate_markers, validate_population,
                                validate_still_controls)


def record(frame, time):
    return ("benchmark-asset: frame=%u meshes=1 animators=1 vertices=1728 "
            "primitives=1 joints=24 clips=3 player=1 time=%.6f gpu_bytes=1000\n" % (frame, time))


class AssetWitnessTests(unittest.TestCase):
    def test_every_projected_cell_must_change(self):
        a = (128, 72, 3, bytes([0, 0, 0] * (128 * 72)))
        b = (128, 72, 3, bytes([255, 255, 255] * (128 * 72)))
        changed = cell_pixel_changes(a, b, 4)
        self.assertTrue(cells_pass(changed, 4))
        self.assertFalse(cells_pass(changed, 5))
        self.assertFalse(cells_pass([changed[0], 0, changed[2], changed[3]], 4))
        self.assertFalse(cells_pass(cell_pixel_changes(a, a, 4), 4))

    def test_all_characters_must_advance_with_the_expected_layers(self):
        def log(advanced, mask=3, total=250):
            return "".join("benchmark-asset-population: frame=%u advanced=%u min=0.25 max=0.25 layers=%u layer_sum=%u\n"
                           % (frame, advanced, mask, total) for frame in (20, 50))
        validate_population(log(500), 500, (0, 1), True)
        for invalid in (log(256), log(500, 1), log(500, 3, 249), ""):
            with self.assertRaises(ValueError):
                validate_population(invalid, 500, (0, 1), True)
        with self.assertRaises(ValueError):
            validate_population(log(0), 500, (0, 1), False)

    def test_temporal_controls_must_actually_be_disabled(self):
        marker = "benchmark-asset: still controls taa=0 ssao=0 ssr=0 motion_blur=0"
        validate_still_controls(marker)
        for log in ("", marker * 2, marker + "\ntaa resolve: on",
                    marker + "\nanim instance cache full (256)"):
            with self.assertRaises(ValueError):
                validate_still_controls(log)

    def test_small_pixel_changes_are_not_skipped_by_sampling(self):
        a = (2, 1, 3, bytes([10, 20, 30, 10, 20, 30]))
        b = (2, 1, 4, bytes([10, 20, 30, 255, 100, 20, 30, 255]))
        self.assertEqual(full_pixel_difference(a, b), 0.5)
        self.assertEqual(full_pixel_difference(a, a), 0)
        with self.assertRaises(ValueError):
            full_pixel_difference(a, (3, 1, 3, b""))

    def test_crowd_fixture_spreads_rows_and_preserves_layer_policy(self):
        doc = fixture("subject.glb", 500, "Walk", 1, 0.01, (0, 1), 1)
        camera = doc["entities"][0]["components"]
        self.assertEqual(camera[0]["rotX"], -45)
        self.assertTrue(camera[1]["orthographic"])
        self.assertEqual(doc["entities"][1]["components"][1]["lightLayerMask"], 1)
        self.assertEqual([item["layer"] for item in doc["entities"][2:]], [0, 1] * 250)

    def test_white_silhouette_is_not_colored_material(self):
        self.assertEqual(chromatic_fraction((2, 1, 3, bytes([255, 255, 255, 100, 100, 100]))), 0)
        self.assertEqual(chromatic_fraction((2, 1, 3, bytes([200, 100, 0, 100, 100, 100]))), 0.5)
        with self.assertRaises(ValueError):
            chromatic_fraction((2, 1, 3, b""))
    def test_clock_controls(self):
        validate_markers(record(20, 0.1) + record(50, 0.6), 1, 1728, 24, 3, True)
        validate_markers(record(20, 0) + record(50, 0), 1, 1728, 24, 3, False)
        with self.assertRaises(ValueError):
            validate_markers(record(20, 0) + record(50, 0), 1, 1728, 24, 3, True)
        with self.assertRaises(ValueError):
            validate_markers(record(20, 0.1) + record(50, 0.6), 1, 1728, 24, 3, False)

    def test_missing_and_wrong_population_fail(self):
        for text in (record(20, 0.1), record(20, 0.1) + record(50, 0.6),
                     record(20, 0.1) * 2 + record(50, 0.6)):
            with self.assertRaises(ValueError):
                validate_markers(text, 2, 1728, 24, 3, True)

    def test_pixel_controls(self):
        good = {"asset_coverage": 0.05, "frozen_clock_difference": 0,
                "moving_clock_difference": 0.01, "repeat_noise": 0}
        self.assertTrue(pixels_pass(good))
        for key, value in (("asset_coverage", 0), ("frozen_clock_difference", 0.01),
                           ("moving_clock_difference", 0), ("repeat_noise", 0.01),
                           ("repeat_noise", float("nan"))):
            self.assertFalse(pixels_pass({**good, key: value}))
        self.assertFalse(pixels_pass({}))


if __name__ == "__main__":
    unittest.main()
