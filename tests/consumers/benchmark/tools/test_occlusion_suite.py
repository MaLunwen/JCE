"""Inactive queries, missing work and false visible-object culls must fail."""
import unittest
from occlusion_suite import REVEAL_FRAMES, cell_changes, validate_counts, validate_history


def records(count=128, enabled=True, reveal=False):
    text = "benchmark-player: entered Play\n" \
        "benchmark-asset: still controls taa=0 ssao=0 ssr=0 motion_blur=0\n"
    text += "benchmark-occlusion: initialized count=%u hidden=%u visible=%u enabled=%u reveal=%u\n" \
        % (count, (count + 1) // 2, count // 2, enabled, reveal)
    if enabled:
        text += "occlusion culler created (entities=1024, queries=256/256, share=1, view=252)\n"
    for frame in [20, 50] + (list(range(79, 91)) if reveal else []):
        total = count + 3 + (0 if reveal and frame >= 80 else 1) if enabled else 0
        occluded = (count + 1) // 2 if enabled and frame < 80 else 0
        text += "benchmark-occlusion: frame=%u enabled=%u total=%u visible=%u occluded=%u warm=0 pending=0 unqueried=0\n" \
            % (frame, enabled, total, total - occluded, occluded)
    if reveal:
        text += "benchmark-occlusion: reveal frame=80\n"
    return text


class OcclusionTests(unittest.TestCase):
    def test_object_cells_detect_a_missing_half_even_when_global_tolerance_is_small(self):
        width, height = 128, 72
        empty = (width, height, 3, bytes(width * height * 3))
        bright = (width, height, 3, bytes([255]) * (width * height * 3))
        self.assertTrue(all(cell_changes(empty, bright, 128)))
        self.assertFalse(any(cell_changes(bright, bright, 128)))
        half = (width, height, 3, bytes(255 if x >= width // 2 else 0
                for _y in range(height) for x in range(width) for _channel in range(3)))
        changes = cell_changes(bright, half, 128)
        self.assertTrue(all(changes[::2]))
        self.assertFalse(any(changes[1::2]))

    def test_first_reveal_frame_is_required_and_may_not_be_filtered_out(self):
        good = {str(frame): 0.0 for frame in REVEAL_FRAMES}
        validate_history(good)
        for value in (0.0001, -1, float("nan"), float("inf")):
            bad = dict(good, **{"80": value})
            with self.assertRaises(ValueError):
                validate_history(bad)
        with self.assertRaises(ValueError):
            validate_history({"90": 0})

    def test_known_population_and_reveal_samples(self):
        for active in (True, False):
            for reveal in (True, False):
                validate_counts(records(enabled=active, reveal=reveal), 128, active, reveal)

    def test_missing_inactive_wrong_and_false_culling_fail(self):
        good = records()
        for bad in ("", good.replace("frame=50", "frame=20"),
                    good.replace("occluded=64", "occluded=65").replace("visible=68", "visible=67"),
                    good.replace("visible=68", "visible=67"),
                    good.replace("occluded=64", "occluded=0").replace("visible=68", "visible=132"),
                    records(enabled=False), good.replace("queries=256/256", "queries=0/256"),
                    good.replace("queries=256/256", "queries=256/0"), good + "] ERROR - failed",
                    good + "UNAVAILABLE hardware queries"):
            with self.assertRaises(ValueError):
                validate_counts(bad, 128, True, False)
        with self.assertRaises(ValueError):
            validate_counts(records(reveal=True).replace("reveal frame=80", "reveal frame=81"),
                            128, True, True)

    def test_budget_overflow_is_retained_not_invented_query_coverage(self):
        text = records(512).replace("visible=260 occluded=256 warm=0 pending=0 unqueried=0",
                                   "visible=128 occluded=128 warm=1 pending=0 unqueried=259")
        rows = validate_counts(text, 512, True, False)
        self.assertTrue(all(row[-1] == 259 for row in rows))


if __name__ == "__main__":
    unittest.main()
