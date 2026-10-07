"""Startup immutable input and clean camera-only controls."""
from pathlib import Path
import tempfile
import unittest

from startup_suite import read_execution_log, sha256_file, validate_empty, validate_trial_identity, validate_runtime_profile


class StartupControls(unittest.TestCase):
    def test_startup_uses_fresh_capture_when_active_log_rotated(self):
        root = Path(__file__).resolve().parents[4] / "build/tests/startup-controls"
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as temporary:
            directory = Path(temporary)
            log, stderr = directory / "engine.log", directory / "stderr.log"
            log.write_text("shutdown tail\n")
            (directory / "engine.log.1.zst").write_bytes(b"old ] ERROR - failure")
            marker = "benchmark-player: initialized viewport=1280x720"
            stderr.write_text("\x1b[96m" + marker + "\x1b[0m\n")
            rows = [{"event": "initialized", "engine": {"entity_count": 1},
                     "model_gpu_driven": {"active": False}},
                    {"event": "exit", "engine": {"entity_count": 1},
                     "model_gpu_driven": {"active": False}}]
            process = {"stderr_path": str(stderr)}
            validate_empty(read_execution_log(log, process), rows)
            for text in (marker + "\n" + marker, marker + "\n[MAIN] ERROR - failure"):
                stderr.write_text(text)
                with self.assertRaises(ValueError):
                    validate_empty(read_execution_log(log, process), rows)

    def test_each_changed_input_rejected(self):
        root = Path(__file__).resolve().parents[4] / "build/tests/startup-controls"
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as temporary:
            paths = [Path(temporary) / name for name in ("exe", "scene", "profile")]
            for path in paths:
                path.write_bytes(b"original")
            identity = {key + "_sha256": sha256_file(path)
                        for key, path in zip(("executable", "scene", "profile"), paths)}
            validate_trial_identity(identity, *paths)
            for path in paths:
                path.write_bytes(b"changed")
                with self.assertRaisesRegex(ValueError, "changed during"):
                    validate_trial_identity(identity, *paths)
                path.write_bytes(b"original")

    def test_canonical_runtime_preserves_intended_controls(self):
        root = Path(__file__).resolve().parents[4] / "build/tests/startup-controls"
        root.mkdir(parents=True, exist_ok=True)
        before = "[renderer]\nvsync=false\n[performance]\nmachine_class=low\njob_workers=1\n[graphics]\nquality=low\n"
        with tempfile.TemporaryDirectory(dir=root) as temporary:
            authored, runtime = (Path(temporary)/name for name in ("before.ini", "runtime.ini"))
            authored.write_text(before)
            canonical = before+"[window]\nwidth = 1280\n"
            canonical = canonical.replace("vsync=false", "vsync = false\nbackend_resolved = d3d11\nbackend = auto")
            runtime.write_text(canonical)
            validate_runtime_profile(authored, runtime, "d3d11")
            for bad in (canonical.replace("vsync = false", "vsync = true"),
                        canonical.replace("job_workers=1", "job_workers=2"),
                        canonical.replace("quality=low", "quality=high"),
                        canonical.replace("quality=low\n", ""),
                        canonical.replace("width = 1280", "width = 1920"),
                        canonical.replace("backend_resolved = d3d11", "backend_resolved = vulkan")):
                runtime.write_text(bad)
                with self.assertRaises(ValueError):
                    validate_runtime_profile(authored, runtime, "d3d11")
            authored.write_text(before+"unknown=true\n")
            runtime.write_text(canonical)
            with self.assertRaises(ValueError):
                validate_runtime_profile(authored, runtime, "d3d11")

    def test_clean_exit_scene_and_error_negative_controls(self):
        log = "benchmark-player: initialized viewport=1280x720"
        rows = [{"event": "initialized", "engine": {"entity_count": 1},
                 "model_gpu_driven": {"active": False}},
                {"event": "exit", "engine": {"entity_count": 1},
                 "model_gpu_driven": {"active": False}}]
        validate_empty(log, rows)
        for bad in (log + " ] ERROR - failed", log + log):
            with self.assertRaises(ValueError):
                validate_empty(bad, rows)
        rows[-1]["event"] = "frame"
        with self.assertRaises(ValueError):
            validate_empty(log, rows)


if __name__ == "__main__":
    unittest.main()
