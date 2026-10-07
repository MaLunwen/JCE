"""Timing must reject stale or incomplete imported-character witnesses."""
import json
from pathlib import Path
import tempfile
import unittest

from time_asset import sha256_file, timing_environment, trial_schedule, witness_input


class AssetTimingTests(unittest.TestCase):
    def test_durability_controls_are_interleaved_and_labelled(self):
        self.assertEqual(trial_schedule(3, 0, 60),
                         [(0, 0), (0, 60), (1, 60), (1, 0), (2, 0), (2, 60)])
        self.assertEqual(trial_schedule(2, 0), [(0, 0), (1, 0)])
        with self.assertRaises(ValueError):
            trial_schedule(2, 60, 60)

    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        root = Path(self.scratch.name)
        self.exe, self.scene, self.image, self.pak = (
            root / name for name in ("player", "scene.json", "image.png", "fixture.pak"))
        for path in (self.exe, self.scene, self.image, self.pak):
            path.write_bytes(path.name.encode("ascii"))
        self.path = root / "result.json"
        self.doc = {"status": "LIVENESS_PASS", "executable_sha256": sha256_file(self.exe),
                    "pak_sha256": sha256_file(self.pak), "minimum_chromatic_coverage": 0.002,
                    "differences": {"asset_coverage": 0.05, "frozen_clock_difference": 0,
                                    "moving_clock_difference": 0.01, "repeat_noise": 0,
                                    "chromatic_coverage": 0.01},
                    "per_cell_changed_pixels": {"coverage": [10, 10], "motion": [2, 3]},
                    "results": {"moving50": {"live_samples": [[50, 2]],
                        "image": str(self.image), "image_sha256": sha256_file(self.image),
                        "scene_sha256": sha256_file(self.scene),
                        "environment": {"JCE_SCENE": str(self.scene),
                                        "JCE_BENCH_ASSET_PAK": str(self.pak)}}}}

    def check(self):
        self.path.write_text(json.dumps(self.doc), encoding="utf-8")
        return witness_input(self.path, self.exe)

    def test_complete_witness_is_accepted(self):
        _, env, count = self.check()
        self.assertEqual(count, 2)
        self.assertEqual(env["JCE_SCENE"], str(self.scene))

    def test_each_identity_must_still_match(self):
        for path in (self.exe, self.image, self.scene, self.pak):
            original = path.read_bytes()
            path.write_bytes(original + b"changed")
            with self.assertRaises(ValueError):
                self.check()
            path.write_bytes(original)

    def test_partial_motion_cannot_be_timed_as_complete_work(self):
        self.doc["per_cell_changed_pixels"]["motion"] = [2, 0]
        with self.assertRaises(ValueError):
            self.check()
        del self.doc["per_cell_changed_pixels"]
        with self.assertRaises(ValueError):
            self.check()

    def test_invalid_material_or_pixel_controls_fail(self):
        for value in (0, float("nan"), float("inf")):
            self.doc["differences"]["chromatic_coverage"] = value
            with self.assertRaises(ValueError):
                self.check()
        self.doc["differences"]["chromatic_coverage"] = 0.01
        self.doc["differences"]["moving_clock_difference"] = 0
        with self.assertRaises(ValueError):
            self.check()

    def test_capture_and_clock_overrides_do_not_leak_into_timings(self):
        original = {"JCE_BENCH_PLAYER_SHOT": "old.png", "JCE_KPI_GAME_SHOTS": "0|old.png",
                    "JCE_MAX_FRAMES": "20", "JCE_KPI_FRAME_LOG": "old.csv",
                    "JCE_SCENE": "fixture.json", "JCE_BENCH_TEXT_CAPTURE": "1"}
        env = timing_environment(original, Path(self.scratch.name))
        self.assertFalse(any(key in env for key in (
            "JCE_BENCH_PLAYER_SHOT", "JCE_KPI_GAME_SHOTS", "JCE_MAX_FRAMES", "JCE_KPI_FRAME_LOG")))
        self.assertEqual(env["JCE_SCENE"], "fixture.json")
        self.assertEqual(env["JCE_BENCH_TEXT_CAPTURE"], "1")
        self.assertEqual(env["JCE_BENCH_COUNTERS"], str(Path(self.scratch.name) / "engine_counters.jsonl"))


if __name__ == "__main__":
    unittest.main()
