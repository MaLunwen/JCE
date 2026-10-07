"""Light visual reports reject incomplete, altered and mixed evidence."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from light_suite import COUNT, HEIGHT, ORTHO_HEIGHT, WIDTH, case_specs, roi, scene, write
from report_lights import checked, load, matrix, sha256_file
from test_light_suite import synthetic


class ReportControls(unittest.TestCase):
    def test_incomplete_and_replaced_executable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            exe = root / "consumer"
            exe.write_bytes(b"tested")
            value = {"status": "LIGHT_PATCH_CONTROLS_PASS", "backend": "d3d11",
                     "executable": str(exe), "executable_sha256": sha256_file(exe), "cases": {}}
            write(root / "suite.json", value)
            with self.assertRaisesRegex(ValueError, "complete 24-case"):
                load(root / "suite.json")
            exe.write_bytes(b"replacement")
            with self.assertRaisesRegex(ValueError, "raw input changed"):
                load(root / "suite.json")
            value["status"] = "FAIL"
            write(root / "suite.json", value)
            with self.assertRaisesRegex(ValueError, "did not pass"):
                load(root / "suite.json")

    def test_mixed_backends_and_binaries(self):
        rows = [({"backend": b, "executable_sha256": "same"}, {})
                for b in ("d3d11", "d3d12", "vulkan", "gl")]
        self.assertEqual(len(matrix(rows)), 4)
        with self.assertRaisesRegex(ValueError, "four distinct"):
            matrix(rows[:3] + [rows[0]])
        rows[3][0]["executable_sha256"] = "other"
        with self.assertRaisesRegex(ValueError, "mixed executable"):
            matrix(rows)

    def test_raw_capture_hash_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory) / "capture.png"
            p.write_bytes(b"original capture")
            digest = sha256_file(p)
            checked(p, digest)
            p.write_bytes(b"edited capture")
            with self.assertRaisesRegex(ValueError, "raw input changed"):
                checked(p, digest)

    def test_actual_matrix_recomputes_and_rejects_metric_or_backend_mutation(self):
        # Pixel oracle stays real; mocked PNG decoding avoids 24 redundant
        # temporary full-resolution image encodes in this provenance test.
        images = synthetic()
        from light_suite import validate
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            exe = root / "consumer"
            exe.write_bytes(b"tested")
            value = {"status": "LIGHT_PATCH_CONTROLS_PASS", "backend": "d3d11",
                "executable": str(exe), "executable_sha256": sha256_file(exe), "cases": {},
                "projection": {"width": WIDTH, "height": HEIGHT, "ortho_height": ORTHO_HEIGHT,
                               "rois": [list(roi(i)) for i in range(COUNT)]}}
            for kind in ("point", "spot"):
                value[kind] = validate(images)
                for label, active, first, geometry in case_specs():
                    d = root / kind / label
                    d.mkdir(parents=True)
                    write(d / "scene.json", scene(kind, active, first=first, geometry=geometry))
                    (d / "engine.log").write_text("jce_renderer: renderer: Direct3D 11 (graphics tier stable)\n"
                        "JCE_FORWARDPLUS set -> r.forwardplus ON\n", encoding="utf-8")
                    (d / "target.png").write_bytes(label.encode())
                    value["cases"][kind + "/" + label] = {
                        "process": {"process": {"returncode": 0, "timed_out": False},
                                    "backend_actual": "Direct3D 11"},
                        "environment": {"JCE_BACKEND": "d3d11", "JCE_FORWARDPLUS": "1"},
                        "files": {name: {"path": str(d/name), "sha256": sha256_file(d/name)}
                                  for name in ("scene.json", "engine.log", "target.png")}}
            p = root / "suite.json"
            write(p, value)
            with patch("report_lights.image", side_effect=lambda path: images[path.parent.name]):
                self.assertEqual(load(p)[1]["point"], validate(images))
                value["point"]["hundred"][99] = 0
                write(p, value)
                with self.assertRaisesRegex(ValueError, "stored patch metrics"):
                    load(p)
                value["point"] = validate(images)
                value["cases"]["point/hundred"]["process"]["backend_actual"] = "Vulkan"
                write(p, value)
                with self.assertRaisesRegex(ValueError, "backend"):
                    load(p)
                value["cases"]["point/hundred"]["process"]["backend_actual"] = "Direct3D 11"
                write(p, value)
                (root/"point"/"hundred"/"target.png").write_bytes(b"edited")
                with self.assertRaisesRegex(ValueError, "raw input changed"):
                    load(p)


if __name__ == "__main__":
    unittest.main()
