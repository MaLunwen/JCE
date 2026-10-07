"""Recomputed shadow reports fail closed on changed evidence."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from gbuffer_asset_suite import sha256_file, write
from report_shadows import load
from shadow_suite import CASES, scene, validate
from test_shadow_suite import synthetic


class ReportControls(unittest.TestCase):
    def test_provenance_and_real_pixel_controls(self):
        pixels = synthetic()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            exe = root / "consumer"
            exe.write_bytes(b"immutable")
            value = {"status": "SHADOW_PIXEL_LIVENESS_PASS", "backend": "d3d11",
                "executable": str(exe), "executable_sha256": sha256_file(exe),
                "controls": {"camera": "orthographic", "distinct_casters": False,
                    "primitive_instancing": None, "diagnostics": False,
                    "sun_direction": [-.6, -1, -.2], "receiver_tilt_deg": 0},
                "cases": {}, "metrics": validate(pixels)}
            for label in CASES:
                folder = root / label
                folder.mkdir()
                write(folder / "scene.json", scene(label))
                (folder / "engine.log").write_text("jce_renderer: renderer: Direct3D 11", encoding="utf-8")
                (folder / "target.png").write_bytes(b"pixel surrogate")
                value["cases"][label] = {"camera": scene(label)["entities"][0]["components"],
                    "files": {name: {"path": str(folder / name), "sha256": sha256_file(folder / name)}
                        for name in ("scene.json", "engine.log", "target.png")},
                    "process": {"backend_actual": "Direct3D 11", "process": {"returncode": 0}},
                    "environment": {"JCE_BACKEND": "d3d11", "JCE_GPU_TIER": "high",
                        "JCE_SCENE": str(folder / "scene.json"), "JCE_BENCH_ASSET_STILL": "1",
                        "JCE_BENCH_SHOT_FRAME": "50", "JCE_BENCH_PLAYER_SHOT": str(folder / "target.png")}}
            suite = root / "suite.json"
            write(suite, value)
            with patch("report_shadows.image", side_effect=lambda p: pixels[p.parent.name]):
                load(suite)
                broken = json.loads(suite.read_text())
                del broken["cases"]["repeat"]
                write(suite, broken)
                with self.assertRaisesRegex(ValueError, "incomplete"):
                    load(suite)
                write(suite, value)
                pixels["repeat"] = pixels["off"].copy()
                with self.assertRaisesRegex(ValueError, "control failed"):
                    load(suite)
                pixels = synthetic()
                value["cases"]["on"]["targets"] = [{"index": 0}]
                write(suite, value)
                with self.assertRaisesRegex(ValueError, "targets differ"):
                    load(suite)
                value["cases"]["on"]["targets"] = []
                entry = value["cases"]["on"]["files"]["scene.json"]
                altered = scene("on")
                altered["rendering"]["shadows"]["resolution"] = 512
                write(Path(entry["path"]), altered)
                entry["sha256"] = sha256_file(Path(entry["path"]))
                write(suite, value)
                with self.assertRaisesRegex(ValueError, "regenerated"):
                    load(suite)
                write(Path(entry["path"]), scene("on"))
                entry["sha256"] = sha256_file(Path(entry["path"]))
                value["backend"] = "gl"
                write(suite, value)
                with self.assertRaises(Exception):
                    load(suite)
                value["backend"] = "d3d11"
                write(suite, value)
                (root / "on" / "target.png").write_bytes(b"changed")
                with self.assertRaisesRegex(ValueError, "raw input changed"):
                    load(suite)


if __name__ == "__main__":
    unittest.main()
