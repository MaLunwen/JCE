import unittest
from unittest.mock import patch
from pathlib import Path
from report_particle_capacity import matrix, load, checked
from particle_capacity_suite import CASES, asset_for, validate_counts
from particle_suite import scene
from gbuffer_asset_suite import sha256_file
from PIL import Image
import json
import tempfile


class ReportControls(unittest.TestCase):
    def test_matrix_binary_backend_negative_controls(self):
        rows = [({"backend": b,"executable_sha256":"same"},{}) for b in ("d3d11","d3d12","vulkan","gl")]
        matrix(rows)
        with self.assertRaises(ValueError): matrix(rows[:3])
        rows[-1][0]["executable_sha256"] = "changed"
        with self.assertRaises(ValueError): matrix(rows)
        rows[-1][0]["executable_sha256"] = "same"
        rows[-1][0]["backend"] = "d3d11"
        with self.assertRaises(ValueError): matrix(rows)

    def test_raw_hash_tampering_rejected(self):
        with patch("report_lights.sha256_file", return_value="changed"):
            with self.assertRaisesRegex(ValueError, "raw input changed"):
                checked(Path("capture.png"), "original")

    def test_fail_and_incomplete_suite_rejected(self):
        import json
        for value in ({"status":"FAIL"}, {"status":"GPU_CAPACITY_CONTROLS_PASS", "cases": {}}):
            with patch.object(Path,"read_text",return_value=json.dumps(value)):
                with self.assertRaises(ValueError): load(Path("suite.json"))

    def test_dist_owned_backend_and_complete_csv_are_rechecked(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            executable = directory / "application.exe"
            executable.write_bytes(b"metadata-only test binary")
            suite_path = directory / "suite.json"
            value = {"status": "GPU_CAPACITY_CONTROLS_PASS", "backend": "d3d11",
                "executable": str(executable), "executable_sha256": sha256_file(executable), "cases": {}}
            for label, config in CASES.items():
                emitters, _, _, _, _, expected = config
                case = directory / label
                case.mkdir()
                asset = case / "fixture.particles.json"
                asset.write_text(json.dumps(asset_for(label)), encoding="utf-8")
                (case / "scene.json").write_text(json.dumps(scene(asset, emitters, True)), encoding="utf-8")
                log = ("benchmark-particle-backend: reported=Direct3D 11\n"
                    "benchmark-particles: frame=50 components=%u loaded=0 cpu_handles=0 gpu_requested=%u\n"
                    "benchmark-particle-requests: source_frame=50 count=%u\n") % (emitters, emitters, emitters)
                rgba = expected.to_bytes(3, "little").hex() + "ff"
                for index in range(emitters):
                    log += "benchmark-particle-alive: source_frame=50 completed_frame=53 ready_engine_frame=52 index=%u name=particles.gpu_alive_%02u rgba=%s alive=%u\n" % (index, index, rgba, expected)
                (case / "engine.log").write_text(log, encoding="utf-8")
                (case / "frames.csv").write_text("frame_index,phase,ms\n" + "".join(
                    "%u,frame_wall,1\n" % frame for frame in range(200)), encoding="utf-8")
                Image.new("RGBA", (1280, 720)).save(case / "target.png")
                files = {name: {"path": str(case / name), "sha256": sha256_file(case / name)}
                    for name in ("fixture.particles.json", "scene.json", "engine.log", "target.png", "frames.csv")}
                env = {"JCE_BACKEND": "d3d11", "JCE_GPU_TIER": "high", "JCE_SCENE": files["scene.json"]["path"],
                    "JCE_BENCH_PARTICLE_OBSERVE": "1", "JCE_GPU_PARTICLES_FORCE": "1", "JCE_BENCH_ASSET_STILL": "1",
                    "JCE_BENCH_SHOT_FRAME": "50", "JCE_BENCH_PLAYER_SHOT": files["target.png"]["path"],
                    "JCE_BENCH_PARTICLE_GPU_STATS": "1"}
                value["cases"][label] = {"process": {"backend_actual": "Direct3D 11",
                    "process": {"returncode": 0, "timed_out": False}, "stdout": "", "stderr": ""},
                    "environment": env, "files": files, "actual_gpu_alive": validate_counts(label, log),
                    "expected_per_emitter": expected}
            def save():
                suite_path.write_text(json.dumps(value), encoding="utf-8")
            save()
            self.assertEqual(set(load(suite_path)[1]), set(CASES))
            raw = value["cases"]["zero"]["files"]["frames.csv"]
            Path(raw["path"]).write_text("frame_index,phase,ms\n" + "".join(
                "%u,frame_wall,1\n" % frame for frame in range(61)), encoding="utf-8")
            raw["sha256"] = sha256_file(Path(raw["path"]))
            save()
            with self.assertRaisesRegex(ValueError, "complete declared run"):
                load(suite_path)


if __name__ == "__main__": unittest.main()
