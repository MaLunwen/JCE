"""GPU timings cannot reuse edited, failed or different-binary pixel evidence."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from gpu_time_suite import sha256_file, verify_witness, timing_environment
from test_gpu_model_suite import counters, model_log


class GpuTimingTests(unittest.TestCase):
    def test_capture_readback_cannot_leak_into_measured_runs(self):
        original = {"JCE_BACKEND": "vulkan", "JCE_GPU_SCENE": "force",
                    "JCE_BENCH_SHOT_FRAME": "50", "JCE_BENCH_TEXT_CAPTURE": "1",
                    "JCE_BENCH_PLAYER_SHOT": "pixels.png",
                    "JCE_BENCH_GPU_SURVIVORS": "counts.jsonl"}
        actual = timing_environment(original, Path("measurement"))
        self.assertEqual(actual["JCE_BACKEND"], "vulkan")
        self.assertEqual(actual["JCE_GPU_SCENE"], "force")
        self.assertEqual(actual["JCE_BENCH_COUNTERS"], str(Path("measurement/engine_counters.jsonl")))
        self.assertFalse(any(key in actual for key in original if key.startswith("JCE_BENCH_")))
        self.assertIn("JCE_BENCH_GPU_SURVIVORS", original)

    def fixture(self, root):
        exe = root / "player.exe"
        exe.write_bytes(b"actual-binary-fixture")
        for name in ("scene.json", "fixture.pak"):
            (root / name).write_bytes(name.encode("ascii"))
        data = {"status": "STATIC_WITNESS_PASS", "count": 6000, "vertices": 24,
                "executable_sha256": sha256_file(exe), "scene_sha256": sha256_file(root / "scene.json"),
                "pak_sha256": sha256_file(root / "fixture.pak"), "captures": {}}
        for label in ("empty", "cpu", "fixed", "gpu", "hiz", "repeat"):
            target = root / label
            target.mkdir()
            image = target / "frame.png"
            image.write_bytes(b"capture-fixture")
            mode = "hiz" if label == "repeat" else label
            log = model_log() + "benchmark-asset: indirect policy=%u\n" % (mode != "fixed")
            (target / "visual.log").write_text(log, encoding="utf-8", newline="\n")
            (target / "engine_counters.jsonl").write_text("raw-resource-fixture", encoding="ascii")
            data["captures"][label] = {"image": str(image), "image_sha256": sha256_file(image),
                                        "resources": counters(mode) if mode != "empty" else []}
        return exe, data

    def verify(self, root, exe, data):
        path = root / "suite.json"
        path.write_text(json.dumps(data), encoding="utf-8", newline="\n")
        with mock.patch("gpu_time_suite.read_png", return_value=(1, 1, 3, bytes(3))), \
                mock.patch("gpu_time_suite.front_cells", side_effect=[[1]] + [[0]] * 4), \
                mock.patch("gpu_time_suite.full_pixel_difference", return_value=0), \
                mock.patch("gpu_time_suite.player_counters", side_effect=[
                    counters(mode) for mode in ("cpu", "fixed", "gpu", "hiz", "hiz")]):
            return verify_witness(path, exe)

    def test_actual_hashes_status_and_resource_log_must_agree(self):
        scratch = Path(__file__).resolve().parents[4] / "build/tests/benchmark-tool-regressions"
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as temporary:
            root = Path(temporary)
            exe, data = self.fixture(root)
            self.assertEqual(self.verify(root, exe, data)["count"], 6000)
            data["status"] = "FAILED"
            with self.assertRaises(ValueError):
                self.verify(root, exe, data)
            data["status"] = "STATIC_WITNESS_PASS"
            exe.write_bytes(b"different-binary")
            with self.assertRaises(ValueError):
                self.verify(root, exe, data)
            exe.write_bytes(b"actual-binary-fixture")
            pak = root / "fixture.pak"
            pak.write_bytes(b"edited-pak")
            with self.assertRaises(ValueError):
                self.verify(root, exe, data)
            pak.write_bytes(b"fixture.pak")
            image = root / "fixed/frame.png"
            image.write_bytes(b"edited-capture")
            with self.assertRaises(ValueError):
                self.verify(root, exe, data)
            image.write_bytes(b"capture-fixture")
            data["captures"]["gpu"]["resources"][-1]["model_gpu_driven"]["input_records"] = 6001
            with self.assertRaises(ValueError):
                self.verify(root, exe, data)


if __name__ == "__main__":
    unittest.main()
