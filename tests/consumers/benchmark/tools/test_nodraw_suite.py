import unittest
import numpy as np
from nodraw_suite import scene, MODES, validate_markers, validate_pixels

class NoDrawTests(unittest.TestCase):
    def test_fixture_is_original_transform_only_target(self):
        target = [row for row in scene()["entities"] if row["name"] == "TransitionLine"]
        self.assertEqual(len(target), 1)
        self.assertEqual([part["type"] for part in target[0]["components"]], ["Transform"])

    def test_population_markers_require_actual_add_remove(self):
        text = "\n".join("benchmark-nodraw: mode=cycle frame=%d added=%d removed=%d lines=%d target=123 present=%d flags=1" %
            (frame, frame == 30, frame == 70, 30 <= frame < 70, 30 <= frame < 70) for frame in (1,30,50,70,90))
        validate_markers(text, "cycle")
        for broken in (text.replace("removed=1", "removed=0"), text+"\n"+text.splitlines()[0], text.replace("frame=90", "frame=89")):
            with self.assertRaises(ValueError): validate_markers(broken, "cycle")

    def test_pixels_reject_empty_added_and_stale_removed(self):
        empty = np.zeros((720,1280,4), dtype=np.uint8)
        line = empty.copy(); line[300:310,500:520,:3] = 255
        pictures = {(cache, mode): (line.copy() if mode in ("steady", "add") else empty.copy()) for cache in ("on", "off") for mode in MODES}
        validate_pixels(pictures)
        for key, bad in ((("on", "add"), empty), (("off", "cycle"), line)):
            changed = dict(pictures); changed[key] = bad
            with self.assertRaises(ValueError): validate_pixels(changed)

if __name__ == "__main__": unittest.main()
