"""Typed exact-match controls through the canonical trace owner."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools import gpu_submission_join as gpu
from tools import lifecycle_trace_report as report
from tools.tests.test_renderer_submission_observation import capture, sample


def frame(identity, before, after, ordinal, *, epoch=3, gpu_source=None,
          begin=1200, end=1230, freq=10000, backend=gpu.D3D11):
    timestamp = identity * 100
    base = dict(pid=1, tid=7)
    scope = dict(id=identity, parent=identity)
    nested = sample(identity + 1, timestamp=timestamp + 3,
                    values=(epoch, before, after, ordinal))
    nested[0]["args"]["parent"] = identity
    events = [dict(base, ph="i", cat="task", ts=timestamp - 1, name=gpu.SERVICE,
                   args=dict(task=identity, parent=0, kind=2)),
              dict(base, ph="B", cat="task", ts=timestamp, name=gpu.SERVICE, args=scope),
              *nested]
    if gpu_source is not None:
        available = freq > 0 and begin >= 0 and end >= begin and (begin != 0 or end != 0)
        values = (freq, end - begin if available else -1, backend, after, ordinal + 1,
                  epoch, int(epoch > 0), gpu_source, begin, end, int(available))
        events.extend(dict(base, ph="C", cat="render", ts=timestamp + 14 + index,
                           name=name, args=dict(scope, value=value))
                      for index, (name, value) in enumerate(zip(gpu.FIELDS, values)))
    events.append(dict(base, ph="E", cat="task", ts=timestamp + 30, name=gpu.SERVICE,
                       args=dict(scope, ns=30000, state=0)))
    return events


def analyze(*frames, **kwargs):
    return report.analyze_trace(capture(*frames), [gpu.SERVICE], gpu_submission_joins=True, **kwargs)


def counter(events, name):
    return next(event for event in events if event["ph"] == "C" and event["name"] == name)


class GpuSubmissionJoinTests(unittest.TestCase):
    def test_lagged_sample_joins_exact_prior_serial_and_latch_is_not_new_work(self):
        joined = analyze(frame(10, 0, 3, 0), frame(20, 3, 4, 1, gpu_source=3),
                         frame(30, 4, 5, 2, gpu_source=3),
                         frame(40, 5, 6, 3, gpu_source=5))
        result = joined["gpu_submission_joins"]
        self.assertEqual(result["counts"]["matched"], 2)
        self.assertEqual(result["counts"]["repeated"], 1)
        self.assertEqual(result["first"]["source_advance"]["scope_id"], 11)
        self.assertEqual(result["first"]["snapshot"]["current_advance_scope_id"], 21)
        self.assertEqual(result["last"]["source_advance"]["scope_id"], 31)
        self.assertNotIn("renderer_submissions", joined)
        self.assertEqual([result[key] for key in ("gpu_completion", "os_present", "dwm")],
                         ["UNAVAILABLE"] * 3)

    def test_valid_source_zero_and_same_serial_in_separate_epochs(self):
        result = analyze(frame(10, 0, 0, 0, epoch=3, gpu_source=0, begin=0, end=1),
                         frame(20, 0, 0, 0, epoch=(1 << 53) + 1, gpu_source=0)
                         )["gpu_submission_joins"]
        self.assertEqual(result["counts"]["matched"], 2)
        self.assertEqual(result["first"]["source_advance"]["values"]["backend_serial_after"], 0)
        self.assertEqual(result["last"]["snapshot"]["values"]["renderer.device_epoch"], str((1 << 53) + 1))

    def test_pre_capture_source_unknown_epoch_and_unsupported_backend_counted_separately(self):
        result = analyze(frame(10, 50, 51, 49, gpu_source=50),
                         frame(20, 51, 52, 50, gpu_source=52),
                         frame(30, 0, 0, 0, epoch=-1, gpu_source=0),
                         frame(40, 0, 1, 0, epoch=4, gpu_source=1, backend=3),
                         frame(50, 1, 2, 1, epoch=4, gpu_source=2, begin=0, end=0)
                         )["gpu_submission_joins"]
        self.assertEqual(result["counts"], dict(matched=1, repeated=0, interval_unavailable=1,
                                                backend_unverified=1, epoch_unknown=1, source_missing=1))

    def test_gate_requires_at_least_one_complete_match(self):
        with self.assertRaisesRegex(ValueError, "no complete exact"):
            analyze(frame(10, 0, 3, 0))

    def test_explicit_gate_requires_actual_match_and_default_omits_report(self):
        for events in (frame(10, 0, 3, 0), frame(10, 0, 3, 0, gpu_source=2),
                       frame(10, 0, 3, 0, gpu_source=3, backend=3)):
            with self.subTest(events=events[-2]["name"]):
                self.assertNotIn("gpu_submission_joins", report.analyze_trace(capture(events), [gpu.SERVICE]))
                with self.assertRaisesRegex(ValueError, "no complete exact"):
                    analyze(events)
        with self.assertRaisesRegex(ValueError, "explicit bool"):
            report.analyze_trace(capture(frame(10, 0, 0, 0)), [gpu.SERVICE], gpu_submission_joins=1)

    def test_incomplete_out_of_order_or_wrong_snapshot_ownership_is_refused(self):
        for field in ("missing", "duplicate", "owner", "thread", "category", "unrelated_child"):
            events = frame(10, 0, 3, 0, gpu_source=3)
            event = counter(events, "gpu.timer_frequency")
            if field == "missing":
                events.remove(event)
            elif field == "duplicate":
                events.insert(events.index(event), event.copy())
            elif field == "owner":
                event["args"]["parent"] = 8
            elif field == "thread":
                event["tid"] = 9
            elif field == "category":
                event["cat"] = "core"
            else:
                events[2]["args"]["parent"] = 0
            with self.subTest(field=field), self.assertRaises(ValueError):
                analyze(events)

    def test_invalid_identity_signed_ticks_and_malformed_time_are_refused(self):
        for name, value in (("renderer.host_submission_ordinal", 0),
                            ("renderer.api_frame_number", 7), ("renderer.device_epoch", 9),
                            ("gpu.source_frame_number", 4), ("gpu.source_frame_number", False),
                            ("gpu.raw_begin_ticks", -1), ("gpu.raw_end_ticks", 3),
                            ("gpu.timer_frequency", 0), ("gpu.timer_frequency", 10000.0),
                            ("gpu.reported_frame_ticks", 3), ("gpu.interval_available", 2)):
            events = frame(10, 0, 3, 0, gpu_source=3)
            counter(events, name)["args"]["value"] = value
            with self.subTest(name=name, value=value), self.assertRaises(ValueError):
                analyze(events)
        for timestamp in (0, -1, float("nan")):
            events = frame(10, 0, 3, 0, gpu_source=3)
            counter(events, "gpu.timer_frequency")["ts"] = timestamp
            with self.subTest(timestamp=timestamp), self.assertRaises(ValueError):
                analyze(events)

    def test_serial_ordinal_wrap_gap_epoch_reentry_and_conflicting_latch_refused(self):
        first = frame(10, 0, 3, 0, gpu_source=3)
        for events in (frame(20, gpu.UINT32_MAX, 0, 1),
                       frame(20, 3, 4, gpu.UINT32_MAX),
                       frame(20, 3, 4, 2),
                       frame(20, 7, 8, 1),
                       frame(20, 3, 4, 1, gpu_source=3, end=1231)):
            with self.subTest(events=events[4:8]), self.assertRaises(ValueError):
                analyze(first, events)
        with self.assertRaisesRegex(ValueError, "epoch reappeared"):
            analyze(first, frame(20, 0, 0, 0, epoch=4), frame(30, 3, 4, 1))

    def test_memory_stream_cli_match_and_full_stream_limits_remain_authority(self):
        frames = (frame(10, 0, 3, 0), frame(20, 3, 4, 1, gpu_source=3))
        document = capture(*frames)
        expected = analyze(*frames)["gpu_submission_joins"]
        work = Path(__file__).resolve().parents[2] / "build/tests/gpu-submission-join"
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="cpu-", dir=work) as directory:
            path, out = (Path(directory) / name for name in ("trace.json", "report.json"))
            path.write_text(json.dumps(document), encoding="utf-8")
            result = report.analyze_trace_file(path, [gpu.SERVICE], gpu_submission_joins=True, chunk_bytes=13)
            self.assertEqual(result["gpu_submission_joins"], expected)
            self.assertEqual(result["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
            call = [sys.executable, "-B", report.__file__, str(path), "--out", str(out),
                    "--require", gpu.SERVICE, "--gpu-submission-joins"]
            run = subprocess.run(call, capture_output=True, text=True, timeout=20, check=False)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(json.loads(out.read_text(encoding="utf-8"))["gpu_submission_joins"], expected)
            with self.assertRaisesRegex(ValueError, "source identity limit"):
                path.write_text(json.dumps(capture(*frames, frame(30, 4, 5, 2))),
                                encoding="utf-8")
                report.analyze_trace_file(path, [gpu.SERVICE], gpu_submission_joins=True, max_active=2)
            path.write_text(json.dumps(document) + "tail", encoding="utf-8")
            with self.assertRaises(ValueError):
                report.analyze_trace_file(path, [gpu.SERVICE], gpu_submission_joins=True)
        document["jceTrace"]["lost"] = document["jceTrace"]["cursor_lost"] = 1
        with self.assertRaisesRegex(ValueError, "capture is incomplete"):
            report.analyze_trace(document, [gpu.SERVICE], gpu_submission_joins=True)

    def test_ordinary_unwired_analyzer_fails_missing_match_control(self):
        source = Path(report.__file__).read_text(encoding="utf-8")
        wire = '            self.gpu_joins = GpuSubmissionJoin(limits["active"] if limits else 8192)'
        self.assertEqual(source.count(wire), 1)
        owner = types.ModuleType("tools.lifecycle_old_unwired_gpu_join")
        owner.__package__ = "tools"
        exec(compile(source.replace(wire, "            self.gpu_joins = None"),
                     report.__file__, "exec"), owner.__dict__)
        with mock.patch.object(report, "_TraceAnalysis", owner._TraceAnalysis):
            with self.assertRaisesRegex(AssertionError, "ValueError not raised"):
                self.test_gate_requires_at_least_one_complete_match()


if __name__ == "__main__":
    unittest.main()
