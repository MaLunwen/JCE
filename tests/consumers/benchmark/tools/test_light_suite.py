"""Negative controls for projected independent light evidence."""
import unittest
import numpy as np
from light_suite import COUNT, HEIGHT, WIDTH, case_specs, position, roi, scene, validate


def synthetic():
    result = {}
    for label, active, first, geometry in case_specs():
        pixels = np.zeros((HEIGHT, WIDTH, 4), dtype=np.uint8)
        pixels[:, :, 3] = 255
        for index in range(COUNT):
            if geometry or index in active:
                x0, y0, x1, y1 = roi(index)
                pixels[y0:y1, x0:x1, :3] = 80 if geometry else 30
        result[label] = pixels
    return result


class LightControls(unittest.TestCase):
    def test_projection_unique_and_physical_isolation(self):
        regions = [roi(i) for i in range(COUNT)]
        for i, (x0, y0, x1, y1) in enumerate(regions):
            self.assertTrue(0 <= x0 < x1 < WIDTH and 0 <= y0 < y1 < HEIGHT)
            for j in range(i):
                a, b, c, d = regions[j]
                self.assertTrue(x1 <= a or c <= x0 or y1 <= b or d <= y0)
            for j in range(COUNT):
                if j != i:
                    x, y = position(i)
                    a, b = position(j)
                    self.assertGreater(((x-a)**2 + (y-b)**2 + 1.15**2)**.5, 1.8)

    def test_scene_selection_and_reordering_preserve_identity(self):
        for kind in ("point", "spot"):
            value = scene(kind, (99,), first=99)
            lights = value["entities"][102:]
            self.assertEqual(len(lights), COUNT)
            self.assertEqual(lights[0]["name"], "Light99")
            self.assertEqual(sum(x["components"][1]["intensity"] > 0 for x in lights), 1)
            self.assertTrue(all(not x["components"][1]["castsShadow"] for x in lights))
        with self.assertRaises(ValueError):
            scene("point", (100,))

    def test_actual_participation_not_aggregate_brightness(self):
        images = synthetic()
        self.assertEqual(len(validate(images)["hundred"]), COUNT)
        for label, index in (("hundred", 99), ("geometry", 64), ("late-64", 64)):
            wrong = {k: v.copy() for k, v in images.items()}
            x0, y0, x1, y1 = roi(index)
            wrong[label][y0:y1, x0:x1, :3] = 0
            with self.assertRaises(ValueError):
                validate(wrong)
        wrong = {k: v.copy() for k, v in images.items()}
        wrong["omit-last"] = wrong["hundred"].copy()
        with self.assertRaises(ValueError):
            validate(wrong)
        wrong = {k: v.copy() for k, v in images.items()}
        wrong["first-99"] = wrong["zero"].copy()
        with self.assertRaises(ValueError):
            validate(wrong)
        wrong = {k: v.copy() for k, v in images.items()}
        wrong["repeat"] = wrong["zero"].copy()
        with self.assertRaises(ValueError):
            validate(wrong)
        wrong = {k: v.copy() for k, v in images.items()}
        wrong["late-16"] = np.flipud(wrong["late-16"])
        with self.assertRaises(ValueError):
            validate(wrong)
        wrong = {k: v.copy() for k, v in images.items()}
        x0, y0, x1, y1 = roi(90)
        wrong["ten"][y0:y1, x0:x1, :3] = 30
        with self.assertRaises(ValueError):
            validate(wrong)
        wrong = dict(images, zero=np.zeros((1, 1, 4), dtype=np.uint8))
        with self.assertRaises(ValueError):
            validate(wrong)


if __name__ == "__main__":
    unittest.main()
