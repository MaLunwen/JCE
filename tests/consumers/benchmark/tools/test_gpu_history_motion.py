"""Continuous camera controls must reject inert, truncated and stale history."""
import math
import unittest

from gpu_history_suite import expected_survivors, validate_camera_motion, validate_occluder_motion
from gpu_history_suite import MOTION_FRAMES, MOTION_COUNTER_FRAMES, validate_history, validate_motion
from gpu_history_suite import peak_revealed_cells


def observations():
    lines = []
    for frame in (20, *range(50, 83), 90):
        wave = math.sin((frame - 50) * math.pi / 32) if 51 <= frame <= 80 else 0
        offset, yaw = .2 * wave, .03 * wave
        lines.append("benchmark-camera: frame=%u offset=%.7f yaw=%.7f "
                     "pos=%.7f,0.0000000,10.0000000 forward=%.7f,0.0000000,%.7f\n" %
                     (frame, offset, yaw, offset, math.sin(yaw), -math.cos(yaw)))
    return lines


class CameraHistoryTests(unittest.TestCase):
    def test_peak_is_one_discrete_frame_without_adjacent_pose_plateau(self):
        lines = observations()
        poses = [float(line.split("offset=")[1].split()[0]) for line in lines]
        frame_positions = dict(zip((20, *range(50, 83), 90), poses))
        self.assertEqual(frame_positions[66], .2)
        for frame in range(51, 82):
            self.assertNotEqual(frame_positions[frame - 1], frame_positions[frame])
        legacy = .2 * math.sin(15 * math.pi / 31)
        self.assertAlmostEqual(legacy, .2 * math.sin(16 * math.pi / 31))

    def test_moving_wall_pixels_cannot_impersonate_exposed_models(self):
        width, height, count = 1280, 720, 128
        empty = (width, height, 3, bytes(width * height * 3))
        wall_only = bytearray(empty[3])
        for y in range(259, 461):
            for x in range(559, 738):
                wall_only[(y * width + x) * 3] = 200
        with self.assertRaises(ValueError):
            peak_revealed_cells(empty, (width, height, 3, wall_only), count)
        focal = height / (2 * math.tan(math.radians(30))) / 15
        for row in range(8):
            for column in range(6):
                x = round(width / 2 + (-3.4 + 1.8 * column / 7) * focal)
                y = round(height / 2 - (-1.8 + 3.6 * row / 7) * focal)
                wall_only[(y * width + x) * 3] = 200
        cells = peak_revealed_cells(empty, (width, height, 3, wall_only), count)
        self.assertEqual(len(cells), 48)
        self.assertEqual([cell["index"] for cell in cells],
                         [row * 16 + column * 2 for row in range(8) for column in range(6)])

    def test_empty_control_never_creates_an_occluder(self):
        self.assertEqual(validate_motion("empty control", "occluder", False), [])
        with self.assertRaises(ValueError):
            validate_motion("benchmark-occluder: frame=20", "occluder", False)
        with self.assertRaises(ValueError):
            validate_motion("empty control", "occluder", True)
        with self.assertRaises(ValueError):
            validate_motion("empty control", "camera", False)
        validate_motion("".join(observations()), "camera", False)

    def test_first_move_and_peak_cannot_be_omitted_from_pixel_history(self):
        history = {str(frame): 0 for frame in MOTION_FRAMES}
        validate_history(history, MOTION_FRAMES)
        for frame in (51, 66):
            missing = dict(history)
            missing.pop(str(frame))
            with self.assertRaises(ValueError):
                validate_history(missing, MOTION_FRAMES)
            changed = dict(history)
            changed[str(frame)] = .001
            with self.assertRaises(ValueError):
                validate_history(changed, MOTION_FRAMES)
        self.assertEqual(MOTION_COUNTER_FRAMES, (20, 50, 51, 66, 79, 80, 81, 82, 90))

    def test_occluder_pose_is_observed_and_must_move(self):
        lines = []
        for frame in (20, *range(50, 83), 90):
            wave = math.sin((frame - 50) * math.pi / 32) if 51 <= frame <= 80 else 0
            offset = 1.8 * wave
            lines.append("benchmark-occluder: frame=%u offset=%.7f pos=%.7f,0.0000000,0.0000000\n" %
                         (frame, offset, -1.67 + offset))
        validate_occluder_motion("".join(lines))
        for invalid in (lines[:-1], lines + [lines[-1]], list(reversed(lines))):
            with self.assertRaises(ValueError):
                validate_occluder_motion("".join(invalid))
        inert = list(lines)
        prefix, _ = inert[5].split("pos=")
        inert[5] = prefix + "pos=-1.6700000,0.0000000,0.0000000\n"
        with self.assertRaises(ValueError):
            validate_occluder_motion("".join(inert))

    def test_all_sdk_pose_observations_required(self):
        lines = observations()
        validate_camera_motion("".join(lines))
        for invalid in (lines[:-1], lines + [lines[-1]], lines[:6] + lines[7:], list(reversed(lines))):
            with self.assertRaises(ValueError):
                validate_camera_motion("".join(invalid))

    def test_command_marker_cannot_hide_inert_actual_camera(self):
        lines = observations()
        for field in ("pos", "forward"):
            altered = list(lines)
            prefix, remainder = altered[5].split(field + "=")
            actual = remainder.split(" ")[0]
            replacement = "0.0000000,0.0000000,10.0000000" if field == "pos" else "0.0000000,0.0000000,-1.0000000"
            altered[5] = prefix + field + "=" + remainder.replace(actual.rstrip(), replacement, 1)
            with self.assertRaises(ValueError):
                validate_camera_motion("".join(altered))

    def test_changed_camera_cannot_reuse_prior_wall_depth(self):
        self.assertEqual([expected_survivors(128, frame, False, True)
                          for frame in (20, 50, 79, 80, 81, 82, 90)],
                         [64, 64, 128, 128, 128, 64, 64])
        self.assertEqual(expected_survivors(128, 90, True), 128)
        self.assertEqual(expected_survivors(128, 90, False), 64)


if __name__ == "__main__":
    unittest.main()
