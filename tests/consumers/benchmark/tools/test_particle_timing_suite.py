import unittest
import json
from pathlib import Path
import tempfile
from unittest.mock import patch
import numpy as np
from particle_timing_suite import timing_records, validate_process, correctness
from particle_suite import particle_asset, scene
from gbuffer_asset_suite import sha256_file


class TimingCutoffTests(unittest.TestCase):
    def test_real_nested_process_schema(self):
        good = {"frame": 1.2, "backend_actual": "Direct3D 11",
                "process": {"returncode": 0, "timed_out": False, "elapsed_s": 2.0}}
        validate_process(good, "d3d11")
        for bad in ({"returncode": 0, "backend_actual": "Direct3D 11"},
                    {**good, "process": {"returncode": 1, "timed_out": False}},
                    {**good, "process": {"returncode": 0, "timed_out": True}},
                    {**good, "process": {"returncode": 0}},
                    {**good, "process": {"returncode": False, "timed_out": False}},
                    {**good, "backend_actual": "Vulkan"}):
            with self.assertRaises(ValueError):
                validate_process(bad, "d3d11")

    def test_cutoff_and_population(self):
        log = ("benchmark-particle-requests: source_frame=50 count=1\n"
               "benchmark-particle-alive: source_frame=50 completed_frame=53 ready_engine_frame=54 "
               "index=0 name=particles.gpu_alive_00 rgba=000100ff alive=256\n"
               "benchmark-particle-stats-disabled: frame=64 requested=1 completed=1")
        self.assertEqual(timing_records(log, 1, False, 256)["disabled_frame"], 64)
        self.assertEqual(timing_records(log+"\nWARN - unrelated warning", 1, False, 256)["disabled_frame"], 64)
        for bad in (log.replace("frame=64", "frame=120"), log.replace("completed=1", "completed=0"),
                    log+"\n"+log.splitlines()[-1], log+"\nWARN - CPU particle upload unavailable", log.replace("rgba=000100ff", "rgba=00010000"),
                    log.replace("alive=256", "alive=255"), log.split("benchmark-particle-stats-disabled")[0]):
            with self.assertRaises(ValueError):
                timing_records(bad, 1, False, 256)
        with self.assertRaises(ValueError):
            timing_records(log, 1, False, 512)

    def test_live_gpu_timing_cannot_hide_unequal_distribution(self):
        def log_for(counts):
            text = "benchmark-particle-requests: source_frame=50 count=8\n"
            for index, count in enumerate(counts):
                rgba = count.to_bytes(3, "little").hex() + "ff"
                text += "benchmark-particle-alive: source_frame=50 completed_frame=53 ready_engine_frame=54 index=%u name=particles.gpu_alive_%02u rgba=%s alive=%u\n" % (index, index, rgba, count)
            return text + "benchmark-particle-stats-disabled: frame=64 requested=8 completed=8"
        self.assertEqual(timing_records(log_for([32] * 8), 8, False, 256)["disabled_frame"], 64)
        with self.assertRaises(ValueError):
            timing_records(log_for([0, 64] + [32] * 6), 8, False, 256)


class ReopenedCorrectnessControls(unittest.TestCase):
    def test_rehash_is_not_a_substitute_for_complete_process_and_population(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            executable = directory / "application.exe"
            executable.write_bytes(b"test-only artifact identity")
            exe_hash = sha256_file(executable)
            suite_path = directory / "suite.json"
            suite = {"status": "CPU_ALIVE_CONTROLS_PASS", "executable": str(executable),
                "executable_sha256": exe_hash, "backend": "d3d11", "path": "cpu",
                "particles": 256, "size": .08, "emission_mode": "burst", "cases": {}}
            pixels = {}
            zero = np.zeros((720, 1280, 4), dtype=np.uint8)
            active, changed = zero.copy(), zero.copy()
            active[100:150, 100:150, 0] = 50
            changed[100:150, 100:150, 2] = 50
            pixels.update(zero=zero, active=active, changed=changed, repeat=active.copy())
            for label in ("zero", "active", "changed", "repeat"):
                case_dir = directory / label
                case_dir.mkdir()
                asset = case_dir / "fixture.particles.json"
                asset.write_text(json.dumps(particle_asset(1, label != "zero", label == "changed", "burst", 256, .08)), encoding="utf-8")
                (case_dir / "scene.json").write_text(json.dumps(scene(asset, 1)), encoding="utf-8")
                count = 0 if label == "zero" else 256
                rgba = count.to_bytes(3, "little").hex() + "ff"
                log = ("benchmark-particle-backend: reported=Direct3D 11\n"
                    "benchmark-particles: frame=50 components=1 loaded=1 cpu_handles=1 gpu_requested=0\n"
                    "benchmark-particle-requests: source_frame=50 count=2\n")
                for index, name in enumerate(("particles.cpu_alive_total", "particles.cpu_drawn_total")):
                    log += "benchmark-particle-alive: source_frame=50 completed_frame=53 ready_engine_frame=54 index=%u name=%s rgba=%s alive=%u\n" % (index, name, rgba, count)
                (case_dir / "engine.log").write_text(log, encoding="utf-8")
                (case_dir / "frames.csv").write_text("frame_index,phase,ms\n" + "".join(
                    "%u,frame_wall,1\n" % frame for frame in range(200)), encoding="utf-8")
                (case_dir / "target.png").write_bytes(b"test image is supplied by an explicit mock")
                suite["cases"]["1-" + label] = {"process": {"backend_actual": "Direct3D 11",
                    "process": {"returncode": 0, "timed_out": False}, "stdout": "", "stderr": ""},
                    "files": {name: {"path": str(case_dir / name), "sha256": sha256_file(case_dir / name)}
                        for name in ("fixture.particles.json", "scene.json", "engine.log", "frames.csv", "target.png")}}
            def save():
                suite_path.write_text(json.dumps(suite), encoding="utf-8")
            def check():
                return correctness(suite_path, exe_hash, "d3d11", "cpu", 256, .08, 1)
            def changed_record(label, name, text):
                record = suite["cases"]["1-" + label]["files"][name]
                Path(record["path"]).write_text(text, encoding="utf-8")
                record["sha256"] = sha256_file(Path(record["path"]))
                save()
            save()
            with patch("particle_timing_suite.image", side_effect=lambda path: pixels[path.parent.name]):
                self.assertEqual(check()["sha256"], sha256_file(suite_path))
                csv_path = directory / "active" / "frames.csv"
                original_csv = csv_path.read_text()
                changed_record("active", "frames.csv", "frame_index,phase,ms\n" + "".join(
                    "%u,frame_wall,1\n" % frame for frame in range(61)))
                with self.assertRaises(ValueError):
                    check()
                changed_record("active", "frames.csv", original_csv)
                log_path = directory / "active" / "engine.log"
                original_log = log_path.read_text()
                changed_record("active", "engine.log", "\n".join(line for line in original_log.splitlines()
                    if not line.startswith("benchmark-particles:")))
                with self.assertRaises(ValueError):
                    check()
                changed_record("active", "engine.log", original_log)
                suite["cases"]["1-active"]["process"]["stderr"] = "] ERROR - retained stderr failure"
                save()
                with self.assertRaises(ValueError):
                    check()


if __name__ == "__main__":
    unittest.main()
