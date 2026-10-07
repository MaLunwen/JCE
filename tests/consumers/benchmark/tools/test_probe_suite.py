"""Fail-closed controls for project-specific CPU sample parsing."""
import unittest
import tempfile
from pathlib import Path
from unittest import mock
from probe_suite import parse_samples, parse_waypoints, measure


class ProbeSamplesTest(unittest.TestCase):
    def log(self, **changes):
        values = dict(probe="jobs", round=0, count=1000, completed=1000,
                      concurrency=1, depth=1, ticks=500, frequency=1000000,
                      checksum=123, correct=1)
        values.update(changes)
        return ("benchmark-probe: name={probe} round={round} count={count} completed={completed} "
                "concurrency={concurrency} depth={depth} ticks={ticks} frequency={frequency} "
                "checksum={checksum} correct={correct}\nbenchmark-probe: completed name=jobs\n").format(**values)

    def parse(self, text):
        return parse_samples(text, probe="jobs", count=1000, rounds=1, concurrency=1, depth=1)

    def test_monotonic_ticks_are_converted_without_frame_clock(self):
        self.assertEqual(self.parse(self.log())[0]["ms"], 0.5)

    def test_missing_work_wrong_settings_and_invalid_clocks_fail(self):
        for changes in (dict(completed=0), dict(correct=0), dict(count=999),
                        dict(concurrency=2), dict(depth=32), dict(probe="raycast"),
                        dict(ticks=0), dict(frequency=0), dict(round=1)):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.parse(self.log(**changes))

    def test_duplicate_and_missing_samples_fail(self):
        for text in (self.log() + self.log(), "benchmark-probe: completed name=jobs",
                     self.log().split("\n")[0]):
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.parse(text)

    def test_errors_do_not_become_success(self):
        with self.assertRaises(ValueError):
            self.parse("] ERROR - actual failure\n" + self.log())

    def test_stream_route_requires_every_monotonic_waypoint(self):
        first = "benchmark-stream-point: round=0 chunk=0 ticks=500 frequency=1000000\n"
        second = first.replace("chunk=0", "chunk=1")
        self.assertEqual(parse_waypoints(first + second, count=2, rounds=1)[0]["ms"], 0.5)
        for text in (first, first + first, second + first,
                     first.replace("ticks=500", "ticks=0") + second,
                     first.replace("frequency=1000000", "frequency=500000") + second):
            with self.subTest(text=text), self.assertRaises(ValueError):
                parse_waypoints(text, count=2, rounds=1)

    def test_failed_waypoint_validation_cannot_leave_a_pass_status(self):
        config = dict(probe="world-stream", count=2, concurrency=1, depth=1)
        log = self.log(probe="world-stream", count=2, completed=2).replace(
            "completed name=jobs", "completed name=world-stream")
        process = dict(timed_out=False, returncode=0, stdout=log, stderr="")
        with tempfile.TemporaryDirectory() as temporary, mock.patch(
                "probe_suite.run_process", return_value=process):
            result = measure(config, exe=Path(temporary)/"unused", directory=Path(temporary)/"trial",
                             rounds=1, timeout=2)
        self.assertEqual(result["status"], "FAIL")
        self.assertNotIn("median_ms", result)


if __name__ == "__main__":
    unittest.main()
