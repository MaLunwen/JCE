"""Negative controls for raw-derived media color reports."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from culling_suite import sha256_file
from report_media_pixels import load_picture, main
from media_pixel_suite import reference_yuv420


class ReportPictureTests(unittest.TestCase):
    def setUp(self):
        scratch = Path(__file__).resolve().parents[4] / "build/tests/benchmark-tool-regressions"
        scratch.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name in ("native.rgba", "reference.rgba"):
            (self.root / name).write_bytes(bytes((20, 50, 80, 255)))
        asset = self.root / "asset.mp4"
        asset.write_bytes(b"local report negative-control input")
        self.row = dict(directory=str(self.root), file=str(asset), asset_sha256=sha256_file(asset),
                        width=1, height=1, native_pts=1.0, reference_pts=1.0,
                        raw_sha256=sha256_file(self.root / "native.rgba"),
                        reference_sha256=sha256_file(self.root / "reference.rgba"),
                        changed_fraction=0.0, mean_rgb_error=0.0, max_rgb_error=0, status="PASS")
        log = "probe seek picture=1x1 pts=1.000000000 bytes=4\n"
        (self.root / "stdout.log").write_text(log, encoding="utf-8", newline="\n")
        self.row["process"] = dict(stdout_path=str(self.root / "stdout.log"), stdout=log,
                                   returncode=0, timed_out=False)
        frames = [dict(best_effort_timestamp_time="0.0", width=1, height=1),
                  dict(best_effort_timestamp_time="1.0", width=1, height=1)]
        (self.root / "reference-frames.json").write_text(json.dumps(dict(frames=frames)), encoding="utf-8")
        self.row["reference_frame_index"] = 1

    def test_valid_raw_identity(self):
        self.assertEqual(load_picture(self.row)["changed_fraction"], 0)

    def test_mutated_raw_cannot_be_published(self):
        (self.root / "native.rgba").write_bytes(bytes((20, 20, 80, 255)))
        with self.assertRaises(ValueError):
            load_picture(self.row)

    def test_forged_metrics_or_acceptance_are_rejected(self):
        for key, value in (("mean_rgb_error", 1.0), ("status", "FAILED"), ("reference_pts", 1.5)):
            changed = json.loads(json.dumps(self.row))
            changed[key] = value
            with self.assertRaises(ValueError):
                load_picture(changed)

    def test_forging_both_pts_does_not_override_actual_records(self):
        self.row.update(native_pts=1.5, reference_pts=1.5)
        with self.assertRaises(ValueError):
            load_picture(self.row)

    def test_rehashed_yuv_mutation_cannot_override_reference_equations(self):
        frame = dict(best_effort_timestamp_time="1.0", width=1, height=1,
                     pix_fmt="yuv420p", color_space="bt709", color_range="tv")
        frames = [dict(frame, best_effort_timestamp_time="0.0"), frame]
        (self.root / "reference-frames.json").write_text(json.dumps(dict(frames=frames)), encoding="utf-8")
        yuv = self.root / "reference.yuv"
        yuv.write_bytes(bytes((16, 128, 128)))
        pixels = reference_yuv420(yuv.read_bytes(), 1, 1, frame).tobytes()
        for name in ("native.rgba", "reference.rgba"):
            (self.root / name).write_bytes(pixels)
        self.row.update(oracle="nearest-yuv", reference_yuv_sha256=sha256_file(yuv),
                        raw_sha256=sha256_file(self.root / "native.rgba"),
                        reference_sha256=sha256_file(self.root / "reference.rgba"))
        self.assertEqual(load_picture(self.row)["max_rgb_error"], 0)
        yuv.write_bytes(bytes((235, 128, 128)))
        self.row["reference_yuv_sha256"] = sha256_file(yuv)
        with self.assertRaises(ValueError):
            load_picture(self.row)

    def test_comparison_cannot_mix_reference_policies(self):
        paths = []
        for oracle in ("ffmpeg", "nearest-yuv"):
            path = self.root / (oracle + ".json")
            path.write_text(json.dumps(dict(oracle=oracle, runs=[self.row], executables={})), encoding="utf-8")
            paths.append(str(path))
        with patch("sys.argv", ["report_media_pixels", "--suite", paths[0], "--label", "before",
                                "--suite", paths[1], "--label", "after",
                                "--out", str(self.root / "mixed-report")]):
            with self.assertRaisesRegex(ValueError, "different chroma reference policies"):
                main()
        import matplotlib.pyplot as plt
        plt.close("all")


if __name__ == "__main__":
    unittest.main()
