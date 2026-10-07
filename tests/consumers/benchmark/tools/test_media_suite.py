"""Playback clocks and current seek pictures must agree with retained evidence."""
import unittest

from media_suite import validate, validate_seek_trace


class MediaEvidenceTests(unittest.TestCase):
    def test_seek_trace_requires_real_generation_ready_and_stages(self):
        trace = ("seek-trace event=queue_ready generation=2 request_pts=5.000000 "
                 "exact=1 keyframe_known=1 keyframe_pts=3.000000 reused=0 pictures=121 "
                 "preroll_pictures=120 index_ms=0 flush_ms=2 decode_ms=300 "
                 "worker_elapsed_ms=303 picture_pts=5.000000")
        logger_line = trace + " at jce_video.cpp:2349"
        self.assertEqual(validate_seek_trace(logger_line, 5)["records"][0]["raw"],
                         logger_line.removeprefix("seek-trace "))
        colored = trace + "\x1b[0m \x1b[96mat jce_video.cpp:2349\x1b[0m"
        self.assertEqual(validate_seek_trace(colored, 5)["records"][0]["raw"],
                         colored.removeprefix("seek-trace "))
        for tail in (" unexpected", " at file.cpp:not-a-line", " arbitrary=value",
                     "\x1b[2J", "\x1b[notSGRm"):
            with self.assertRaises(ValueError):
                validate_seek_trace(trace + tail, 5)
        rows = validate_seek_trace(trace, 5)["records"]
        self.assertEqual(rows[0]["pictures"], 121)
        self.assertEqual(rows[0]["decode_ms"], 300)
        for bad in ("", trace.replace("queue_ready", "cancel"), trace + "\n" + trace,
                    trace.replace("generation=2", "generation=0"),
                    trace.replace("preroll_pictures=120", "preroll_pictures=122"),
                    trace.replace("picture_pts=5.000000", "picture_pts=3.000000"),
                    trace.replace("index_ms=0", "index_ms=-1"),
                    trace.replace("decode_ms=300 ", "")):
            with self.assertRaises(ValueError):
                validate_seek_trace(bad, 5)
        unknown = validate_seek_trace(trace.replace("keyframe_known=1", "keyframe_known=0"), 5)
        self.assertIsNone(unknown["records"][0]["keyframe_pts"])
        canceled = trace.replace("queue_ready", "cancel").replace("generation=2", "generation=1")
        self.assertEqual(validate_seek_trace(canceled + "\n" + trace, 5)["records"][0]["event"], "cancel")

    def fixture(self):
        lines = ["probe paused preview frame=1 clock=0.000000000"]
        lines += ["timeline wall=%.3f media=%.3f picture=%.3f audio=0.000 displayed=%u dropped=0 queue=8" %
                  (second, second, max(0, second - 0.016), 1 + second * 60) for second in range(6)]
        lines += ["probe av01 1920x1080 60.0fps elapsed=6001 ms media=6.001 dec=380 disp=360 drop=20 decode=1.234 ms convert=0.123 ms pop=0.456 ms queue=20",
                  "probe seek 5.000s call=0 ms", "probe seek frame=361 wait=18 ms pts=5.000",
                  "probe input size=1000000000 backend_reads=120 bytes_read=30000000 owned=65632"]
        return "\n".join(lines)

    def test_complete_timeline_and_seek_positive(self):
        result = validate(self.fixture(), 6, 16)
        self.assertEqual(result["seek_ready_ms"], 18)
        self.assertEqual(result["source_owned_bytes"], 65632)
        self.assertEqual(len(result["timeline"]), 6)

    def test_missing_and_regressing_picture_are_rejected(self):
        for replacement in ("picture=-1.000", "picture=0.000"):
            with self.assertRaises(ValueError):
                validate(self.fixture().replace("picture=4.984", replacement), 6, 16)

    def test_wall_clock_lag_cannot_be_reported_as_correct_playback(self):
        log = self.fixture().replace("wall=4.000 media=4.000 picture=3.984",
                                     "wall=4.000 media=3.600 picture=3.584")
        with self.assertRaises(ValueError):
            validate(log, 6, 16)

    def test_stale_seek_and_whole_file_copy_are_rejected(self):
        for original, replacement in (("pts=5.000", "pts=1.000"), ("owned=65632", "owned=1000000000")):
            with self.assertRaises(ValueError):
                validate(self.fixture().replace(original, replacement), 6, 16)

    def test_inactive_audio_is_not_real_device_clock_evidence(self):
        with self.assertRaises(ValueError):
            validate(self.fixture(), 6, 16, audio_device=True)

    def test_empty_or_truncated_observation_is_not_a_pass(self):
        for log in ("", self.fixture().split("probe seek")[0]):
            with self.assertRaises(ValueError):
                validate(log, 6, 16)

    def resume_fixture(self):
        log = self.fixture()
        for second in range(6):
            log = log.replace("picture=%.3f audio=0.000" % max(0, second - 0.016),
                              "picture=%.3f audio=%.3f" % (max(0, second - 0.016), second))
        log += "\nprobe audio device=48000Hz channels=2 duration=10.000\n"
        log += "\n".join("resume wall=%.3f media=%.3f picture=%.3f audio=%.3f frame=%u" %
                         (i * 0.05, 5 + i * 0.05, 5 + i * 0.05, 5 + i * 0.05, 361 + i * 3)
                         for i in range(40))
        return log + "\nprobe resumed seconds=2 start=5.000000 end=7.000000\n"

    def test_post_seek_device_resume_requires_live_progress(self):
        result = validate(self.resume_fixture(), 6, 16, True, 2)
        self.assertEqual(result["resume_max_picture_lag_s"], 0)
        self.assertEqual(len(result["resume_timeline"]), 40)
        for original, replacement in (("picture=6.000", "picture=1.000"),
                                      ("media=6.000", "media=5.500"),
                                      ("audio=6.000", "audio=1.000"),
                                      ("end=7.000000", "end=5.000000")):
            with self.assertRaises(ValueError):
                validate(self.resume_fixture().replace(original, replacement), 6, 16, True, 2)
        with self.assertRaises(ValueError):
            validate(self.fixture(), 6, 16, False, 2)

    def test_fractional_seek_keeps_submillisecond_precision(self):
        log = self.resume_fixture().replace("probe seek 5.000s", "probe seek 5.000333333s")
        log = log.replace("start=5.000000", "start=5.000333")
        self.assertEqual(len(validate(log, 6, 16, True, 2)["resume_timeline"]), 40)
        with self.assertRaises(ValueError):
            validate(log.replace("start=5.000333", "start=5.000000"), 6, 16, True, 2)


if __name__ == "__main__":
    unittest.main()
