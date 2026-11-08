"""Ordinary synthetic validator controls. No Engine/native process or GPU."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

from depth_order_oracle import CASES, COMBINATIONS, project, projected_mask, scene, validate

SIZE = (320, 180)


def synthetic():
    masks = {name: projected_mask(name, SIZE) for name in "BWF"}
    images = {}
    for case in CASES:
        image = np.full((SIZE[1], SIZE[0], 3), 25, dtype="uint8")
        kind = case.split("-")[0]
        if kind != "empty":
            if "B" in kind:
                image[masks["B"]] = (220, 20, 15)
            if "W" in kind:
                image[masks["W"]] = ((image[masks["W"]].astype("uint16") +
                                        np.array([10, 30, 220])) // 2).astype("uint8")
            if "F" in kind:
                image[masks["F"]] = (220, 190, 10)
        images[case] = image
    return images


class DepthOrderPixels(unittest.TestCase):
    def test_analytical_projection_known_points(self):
        self.assertEqual(project((0, 0, 0), SIZE), (160, 90))
        self.assertEqual(project((6, 0, 0), SIZE), (250, 90))
        self.assertAlmostEqual(project((0, 1, 0), SIZE)[1], 79.3933982822)
        self.assertAlmostEqual(project((0, 0, 1), SIZE)[1], 100.6066017178)

    def test_static_scene_is_public_and_order_preserves_entities(self):
        normal, reverse = scene("BWF"), scene("BWF-order")
        self.assertEqual(normal["entities"], list(reversed(reverse["entities"])))
        self.assertEqual(scene("BWF-repeat"), normal)
        self.assertEqual([e["name"] for e in normal["entities"]], ["Camera", "B", "W", "F"])
        water = normal["entities"][2]["components"][1]
        self.assertEqual(water["waterMode"], 0)
        self.assertEqual(water["waveCount"], 0)
        self.assertEqual(water["transparency"], .5)
        self.assertFalse(water["depthWrite"])
        self.assertFalse(normal["rendering"]["fog"]["enabled"])
        with self.assertRaises(ValueError):
            scene("unregistered")

    def test_synthetic_positive_and_all_roi_support(self):
        result = validate(synthetic(), SIZE)
        self.assertEqual(len(result["stability"]), 8)
        self.assertEqual(len(result["checks"]), 5)
        for check in result["checks"].values():
            self.assertGreater(check["pixels"], 64)

    def reject(self, images):
        with self.assertRaises(ValueError):
            validate(images, SIZE)

    def test_old_water_erased_by_late_opaque_is_rejected(self):
        images = synthetic()
        for case in ("BW", "BWF"):
            for suffix in ("", "-repeat", "-order"):
                images[case + suffix] = images["B" if case == "BW" else "BF"].copy()
        self.reject(images)

    def test_water_without_depth_test_covering_foreground_is_rejected(self):
        images = synthetic()
        mask = projected_mask("F", SIZE)
        for suffix in ("", "-repeat", "-order"):
            images["BWF" + suffix][mask] = images["BW"][mask]
        self.reject(images)

    def test_fully_opaque_water_is_not_transparency_pass(self):
        images = synthetic()
        mask = projected_mask("W", SIZE)
        foreground = projected_mask("F", SIZE)
        for case in ("BW", "BWF"):
            roi = mask & (~foreground if case == "BWF" else True)
            for suffix in ("", "-repeat", "-order"):
                images[case + suffix][roi] = images["W"][roi]
        self.reject(images)

    def test_missing_and_wrong_shape_and_wrong_dtype_refuse(self):
        for alteration in ("missing", "shape", "dtype"):
            images = synthetic()
            if alteration == "missing":
                del images["W"]
            elif alteration == "shape":
                images["W"] = images["W"][:-1]
            else:
                images["W"] = images["W"].astype("float32")
            self.reject(images)

    def test_empty_masks_cannot_pass(self):
        images = {case: np.full((180, 320, 3), 25, dtype="uint8") for case in CASES}
        self.reject(images)

    def test_shifted_single_object_cannot_redefine_projection(self):
        images = synthetic()
        images["W"] = np.roll(images["W"], 20, axis=1)
        self.reject(images)

    def test_repeat_and_creation_order_must_retain_same_pixels(self):
        for suffix in ("-repeat", "-order"):
            images = synthetic()
            images["BWF" + suffix][90, 160] = (0, 0, 0)
            self.reject(images)

    def test_all_four_combinations_have_repeat_and_permutation(self):
        for name in COMBINATIONS:
            self.assertTrue({name, name + "-repeat", name + "-order"}.issubset(CASES))

    def test_default_preparation_never_launches_an_application(self):
        import check_depth_order_pixels as runner
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            exe = root / "cpu-only-identity.bin"
            exe.write_bytes(b"not an executable; preparation must never launch it")
            with patch.object(runner, "run_visual_witness") as run:
                result = runner.execute(exe, root / "prepared")
            run.assert_not_called()
            self.assertEqual(result["status"], "PREPARED_NOT_EXECUTED")
            self.assertFalse(result["actual_readback_qualified"])
            self.assertFalse(result["native_executed"])
            self.assertEqual(len(result["cases"]), 15)
            self.assertEqual(json.loads((root / "prepared/result.json").read_text())["status"],
                             "PREPARED_NOT_EXECUTED")
            with self.assertRaises(ValueError):
                runner.execute(exe, root / "prepared")

    def test_failed_child_preserves_failure_and_never_claims_pixels(self):
        import check_depth_order_pixels as runner
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            exe = root / "cpu-only-identity.bin"
            exe.write_bytes(b"a mock child is not a native or GPU test")
            with patch.object(runner, "run_visual_witness", side_effect=RuntimeError("ordinary child refusal")):
                with self.assertRaisesRegex(RuntimeError, "ordinary child refusal"):
                    runner.execute(exe, root / "failure", launch=True)
            result = json.loads((root / "failure/result.json").read_text())
            self.assertEqual(result["status"], "FAIL")
            self.assertFalse(result["actual_readback_qualified"])
            self.assertIn("ordinary child refusal", result["error"])

    def test_four_combination_preparation_is_explicitly_incomplete(self):
        import check_depth_order_pixels as runner
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            exe = root / "cpu-only-identity.bin"
            exe.write_bytes(b"not a GPU fixture")
            with patch.object(runner, "run_visual_witness") as run:
                result = runner.execute(exe, root / "quick", cases=COMBINATIONS)
            run.assert_not_called()
            self.assertFalse(result["all_required_cases"])
            self.assertFalse(result["actual_readback_qualified"])
            self.assertEqual(set(result["cases"]), set(COMBINATIONS))
            for invalid in ([], ["B", "B"], ["unregistered"]):
                with self.assertRaises(ValueError):
                    runner.execute(exe, root / "invalid", cases=invalid)


if __name__ == "__main__":
    unittest.main()
