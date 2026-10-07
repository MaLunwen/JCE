"""Relocation witness failure controls without launching an editor."""
from pathlib import Path
import sys
import tempfile
import unittest
import argparse
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from editor_portability_probe import frame_count, probe_environment, validate_frames, validate_image, validate_process


class PortabilityControls(unittest.TestCase):
    def test_complete_frame_stream_rejects_truncation_and_wrong_origin(self):
        directory = Path(__file__).resolve().parents[2] / "build/tests/portability"
        directory.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=directory) as temporary:
            path = Path(temporary) / "frames.csv"
            for count, start in ((240, 0), (239, 0), (240, 1)):
                path.write_text("frame_index,phase,ms\n" +
                                "".join(f"{i},frame_wall,1\n" for i in range(start, start + count)),
                                encoding="utf-8")
                if count == 240 and start == 0:
                    self.assertEqual(validate_frames(path), 240)
                else:
                    with self.assertRaises(ValueError):
                        validate_frames(path)

    def test_extended_frames_require_complete_zero_origin_stream(self):
        directory = Path(__file__).resolve().parents[2] / "build/tests/portability"
        directory.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=directory) as temporary:
            path = Path(temporary) / "extended.csv"
            for count, origin in ((480, 0), (479, 0), (480, 1), (481, 0)):
                path.write_text("frame_index,phase,ms\n" +
                    "".join(f"{i},frame_wall,1\n" for i in range(origin, origin + count)), encoding="utf-8")
                if count == 480 and origin == 0:
                    self.assertEqual(validate_frames(path, 480), 480)
                else:
                    with self.assertRaises(ValueError):
                        validate_frames(path, 480)

    def test_frames_are_bounded_and_low_memory_is_explicit(self):
        self.assertEqual(frame_count("240"), 240)
        self.assertEqual(frame_count(100000), 100000)
        for value in (239, 100001, "240.5", "junk", True, -1):
            with self.assertRaises(argparse.ArgumentTypeError):
                frame_count(value)
        with mock.patch.dict("os.environ", {"JCE_LOW_MEM": "1", "JCE_MAX_FRAMES": "9"}):
            default = probe_environment(Path("scene"), Path("out"), 240, False)
            low = probe_environment(Path("scene"), Path("out"), 480, True)
        self.assertNotIn("JCE_LOW_MEM", default)
        self.assertEqual(default["JCE_MAX_FRAMES"], "240")
        self.assertEqual(low["JCE_LOW_MEM"], "1")
        self.assertEqual(low["JCE_MAX_FRAMES"], "480")
        self.assertEqual(low["JCE_WINCAP_FRAME"], "120")

    def test_normal_exit_and_timeout_controls(self):
        validate_process({"returncode": 0, "timed_out": False})
        for result in ({"returncode": 1, "timed_out": False},
                       {"returncode": 0, "timed_out": True}):
            with self.assertRaises(ValueError):
                validate_process(result)

    def test_complete_nonblank_window_and_negative_controls(self):
        pixels = bytes([0, 0, 0, 255, 255, 255]) * (320 * 240 // 2)
        self.assertEqual(validate_image((320, 240, 3, pixels), "synthetic"), [320, 240])
        for image in ((320, 240, 3, pixels[:-1]), (1, 1, 3, b"abc")):
            with self.assertRaises(ValueError):
                validate_image(image, "synthetic")
        with self.assertRaises(RuntimeError):
            validate_image((320, 240, 3, bytes(320 * 240 * 3)), "synthetic")


if __name__ == "__main__":
    unittest.main()
