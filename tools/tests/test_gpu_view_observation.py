"""Typed GPU-view CPU controls through the actual canonical memory/stream owner."""
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from contextlib import redirect_stdout
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools import gpu_view_observation as gpu
from tools import lifecycle_trace_report as reporter


def view(view_id=4, **changes):
    values = dict(device_epoch=7, api_frame=60, submission_ordinal=59, source_frame=48,
                  begin_ticks=100, end_ticks=300, timer_frequency=1000000,
                  interval_available=1, reported_ticks=200)
    values.update(changes)
    return view_id, values


def sample(identity=1, tid=7, timestamp=1000, views=None):
    views = [view()] if views is None else views
    base = dict(pid=1, tid=tid)
    events = [dict(base, ph="i", cat="task", ts=timestamp - 1, name=gpu.SERVICE,
                   args=dict(task=identity, parent=0, kind=2)),
              dict(base, ph="B", cat="task", ts=timestamp, name=gpu.SERVICE,
                   args=dict(id=identity, parent=identity))]
    for view_id, values in views:
        for field in gpu.FIELDS:
            events.append(dict(base, ph="C", cat="render", ts=timestamp + len(events) - 1,
                name=gpu.PREFIX + str(view_id) + "." + field,
                args=dict(id=identity, parent=identity, value=values[field])))
    events.append(dict(base, ph="E", cat="task", ts=timestamp + len(events) + 1,
        name=gpu.SERVICE, args=dict(id=identity, parent=identity, ns=10000, state=0)))
    return events


def capture(*groups):
    events = [event for group in groups for event in group]
    return dict(jceTrace=dict(recorded=len(events), captured=len(events), overwritten_before_capture=0,
                             cursor_lost=0, lost=0, capture_generation_stable=True), traceEvents=events)


class GpuViewTests(unittest.TestCase):
    def setUp(self):
        output = Path(__file__).resolve().parents[2] / "build/tests/gpu-view-observation"
        output.mkdir(parents=True, exist_ok=True)
        folder = tempfile.TemporaryDirectory(prefix="cpu-", dir=output)
        self.addCleanup(folder.cleanup)
        self.path = Path(folder.name) / "trace.json"

    def file(self, document, **kwargs):
        self.path.write_text(json.dumps(document, allow_nan=False), encoding="utf-8")
        return reporter.analyze_trace_file(self.path, [gpu.SERVICE], gpu_view_observations=True,
                                           chunk_bytes=7, **kwargs)

    def test_actual_nine_field_packets_preserve_per_view_source_and_wide_values(self):
        high = (1 << 53) + 19
        document = capture(sample((1 << 63) + 9, (1 << 53) + 7, views=[
            view(0, device_epoch=gpu.INT64_MAX, begin_ticks=high, end_ticks=high + 200),
            view(gpu.UINT16_MAX, device_epoch=gpu.INT64_MAX, source_frame=3,
                 begin_ticks=high, end_ticks=high + 200)]))
        streamed = self.file(document)
        memory = reporter.analyze_trace(document, [gpu.SERVICE], gpu_view_observations=True)
        data = streamed["gpu_view_observations"]
        self.assertEqual(data, memory["gpu_view_observations"])
        self.assertEqual(data["samples_count"], 2)
        self.assertEqual([r["last_signature"]["source_frame"] for r in data["views"]], [48, 3])
        self.assertEqual(data["first"]["values"]["begin_ticks"], str(high))
        self.assertEqual(data["first"]["scope_id"], str((1 << 63) + 9))
        self.assertEqual(data["first"]["thread"], str((1 << 53) + 7))
        self.assertEqual(data["views"][0]["device_epoch"], str(gpu.INT64_MAX))
        self.assertEqual(data["views"][0]["duration_ms_mean"], .2)
        self.assertEqual(data["first"]["field_raw_indices"], dict(zip(gpu.FIELDS, range(2, 11))))
        self.assertEqual(streamed["sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())
        self.assertTrue(streamed["validation_complete"])
        self.assertIn("no CPU/CSV frame join", data["scope"])
        self.assertIn("GPU completion", data["scope"])

    def test_requested_no_view_data_cannot_pass_and_legacy_default_is_unchanged(self):
        document = capture(sample(views=[]))
        self.assertNotIn("gpu_view_observations", reporter.analyze_trace(document, [gpu.SERVICE]))
        with self.assertRaisesRegex(ValueError, "no view packets"):
            reporter.analyze_trace(document, [gpu.SERVICE], gpu_view_observations=True)
        malformed = capture(sample()); malformed["traceEvents"][2]["args"]["value"] = 0.0
        self.assertNotIn("gpu_view_observations", reporter.analyze_trace(malformed, [gpu.SERVICE]))
        for value in (1, 0, None, "true"):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "explicit bool"):
                reporter.analyze_trace(document, [gpu.SERVICE], gpu_view_observations=value)

    def test_complete_fields_actual_order_addresses_and_duplicates_are_required(self):
        for mode in ("partial", "duplicate", "unknown-field", "reordered", "zero-padded", "overflow-id", "negative-id", "double-id"):
            events = sample()
            if mode == "partial": events.pop(5)
            elif mode == "duplicate": events.insert(3, copy.deepcopy(events[2]))
            elif mode == "unknown-field": events[2]["name"] = "gpu.view.4.extra"
            elif mode == "reordered": events[2], events[3] = events[3], events[2]
            elif mode == "zero-padded": events[2]["name"] = "gpu.view.04.device_epoch"
            elif mode == "overflow-id": events[2]["name"] = "gpu.view.65536.device_epoch"
            elif mode == "negative-id": events[2]["name"] = "gpu.view.-1.device_epoch"
            else: events[2]["name"] = "gpu.view.4.0.device_epoch"
            with self.subTest(mode=mode), self.assertRaises(ValueError): self.file(capture(events))
        with self.assertRaises(ValueError): self.file(capture(sample(views=[view(), view()])))

    def test_exact_types_and_int64_uint32_ranges_are_not_coerced(self):
        invalid = {"device_epoch": (0, -2, 1 << 63), "api_frame": (-1, 1 << 32),
                   "submission_ordinal": (-1, 1 << 32), "source_frame": (-1, 1 << 32),
                   "begin_ticks": (gpu.INT64_MIN - 1, 1 << 63),
                   "end_ticks": (gpu.INT64_MIN - 1, 1 << 63),
                   "timer_frequency": (gpu.INT64_MIN - 1, 1 << 63),
                   "interval_available": (-1, 2), "reported_ticks": (gpu.INT64_MIN - 1, 1 << 63)}
        for field in gpu.FIELDS:
            for value in (*invalid[field], True, 1.0, "1", None):
                events = sample(); events[2 + gpu.FIELDS.index(field)]["args"]["value"] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError): self.file(capture(events))

    def test_formula_distinguishes_valid_zero_from_unavailable_and_preserves_raw_clock(self):
        cases = [(10, 10, 1, 1, 0), (0, 0, 1, 0, -1), (0, 2, 1, 1, 2),
                 (-1, 5, 1, 0, -1), (5, 4, 1, 0, -1), (0, 2, 0, 0, -1),
                 (gpu.INT64_MIN, gpu.INT64_MAX, -1, 0, -1)]
        for begin, end, freq, available, ticks in cases:
            record = view(begin_ticks=begin, end_ticks=end, timer_frequency=freq,
                          interval_available=available, reported_ticks=ticks)
            with self.subTest(begin=begin, end=end, freq=freq):
                data = self.file(capture(sample(views=[record])))["gpu_view_observations"]
                self.assertEqual(data["available_samples_count"], available)
                self.assertEqual(data["zero_duration_samples_count"], int(available and ticks == 0))
                self.assertEqual(data["first"]["duration_ms"], ticks * 1000.0 / freq if available else None)
                self.assertEqual(data["views"][0]["duration_ms_mean"], ticks * 1000.0 / freq if available else None)
                self.assertEqual(data["first"]["values"]["begin_ticks"], str(begin) if abs(begin) >= 1 << 53 else begin)
                for field, bad in (("interval_available", 1 - available), ("reported_ticks", ticks + 1)):
                    broken = copy.deepcopy(record); broken[1][field] = bad
                    with self.assertRaisesRegex(ValueError, "clock formula"): self.file(capture(sample(views=[broken])))
        unavailable = self.file(capture(sample(views=[view(begin_ticks=0, end_ticks=0,
            interval_available=0, reported_ticks=-1)])))["gpu_view_observations"]
        self.assertEqual(unavailable["status"], "GPU_VIEW_TIMING_UNAVAILABLE")
        self.assertEqual(unavailable["views"][0]["duration_samples_count"], 0)
        self.assertIsNone(unavailable["views"][0]["duration_ms_sum"])

    def test_latched_repeats_use_epoch_view_source_and_raw_ticks_not_cpu_frame(self):
        records = [view(), view(api_frame=120, submission_ordinal=119),
                   view(api_frame=180, begin_ticks=150, end_ticks=350),
                   view(api_frame=240), view(api_frame=300, source_frame=49),
                   view(api_frame=360, device_epoch=8), view(api_frame=420, device_epoch=-1),
                   view(api_frame=480, source_frame=49)]
        data = self.file(capture(*[sample(i + 1, timestamp=1000 * (i + 1), views=[r])
                                  for i, r in enumerate(records)]))["gpu_view_observations"]
        self.assertEqual(data["samples_count"], 8)
        self.assertEqual(data["adjacent_repeated_known_view_samples_count"], 1)
        self.assertEqual(data["unknown_epoch_samples_count"], 1)
        self.assertEqual(len(data["views"]), 2)
        self.assertEqual(data["views"][0]["duration_samples_count"], 5)
        self.assertEqual(data["views"][0]["samples_count"], 6)
        self.assertIn("no global uniqueness", data["repeat_scope"])
        # The same duration from a different raw interval is not stale; nor is
        # returning to an earlier signature a claim of global uniqueness.
        self.assertEqual(data["views"][0]["adjacent_repeated_samples_count"], 1)

    def test_uint32_zero_wrap_is_valid_and_unknown_epochs_never_form_identity_groups(self):
        data = self.file(capture(sample(views=[view(api_frame=gpu.UINT32_MAX,
            source_frame=gpu.UINT32_MAX, submission_ordinal=gpu.UINT32_MAX)]),
            sample(2, timestamp=2000, views=[view(api_frame=0, source_frame=0, submission_ordinal=0)])))["gpu_view_observations"]
        self.assertEqual(data["last"]["values"]["source_frame"], 0)
        self.assertEqual(data["views"][0]["duration_samples_count"], 2)
        unknown = self.file(capture(sample(views=[view(device_epoch=-1)]),
            sample(2, timestamp=2000, views=[view(device_epoch=-1)])))["gpu_view_observations"]
        self.assertEqual(unknown["views"], [])
        self.assertEqual(unknown["unknown_epoch_samples_count"], 2)
        self.assertEqual(unknown["adjacent_repeated_known_view_samples_count"], 0)

    def test_wrong_owner_thread_category_identity_and_scope_outcomes_fail(self):
        for field, value in (("id", 2), ("parent", 0), ("id", True), ("parent", 1.0), ("id", 1 << 64)):
            events = sample(); events[2]["args"][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError): self.file(capture(events))
        for field, value in (("tid", 8), ("cat", "core")):
            events = sample(); events[2][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): self.file(capture(events))
        for mode in ("no-submit", "wrong-kind", "wrong-name", "wrong-category", "wrong-parent", "failed", "cancelled", "unclosed"):
            events = sample()
            if mode == "no-submit": events.pop(0)
            elif mode == "wrong-kind": events[0]["args"]["kind"] = 0
            elif mode == "wrong-name": events[0]["name"] = "app.init"
            elif mode == "wrong-category": events[1]["cat"] = events[-1]["cat"] = "render"
            elif mode == "wrong-parent": events[1]["args"]["parent"] = events[-1]["args"]["parent"] = 0
            elif mode in ("failed", "cancelled"): events[-1]["args"]["state"] = 1 if mode == "failed" else 2
            else: events.pop()
            with self.subTest(mode=mode), self.assertRaises(ValueError): self.file(capture(events))

    def test_counter_interval_and_shared_snapshot_checks_do_not_require_equal_source_frames(self):
        for mode in ("before-begin", "after-end", "reverse-clock", "past-end"):
            events = sample()
            if mode == "before-begin": events.insert(1, events.pop(2))
            elif mode == "after-end": events.append(events.pop(10))
            elif mode == "reverse-clock": events[3]["ts"] = 999
            else: events[10]["ts"] = events[-1]["ts"] + 1
            with self.subTest(mode=mode), self.assertRaises(ValueError): self.file(capture(events))
        self.file(capture(sample(views=[view(4, source_frame=0), view(5, source_frame=50)])))
        for field in ("device_epoch", "api_frame", "submission_ordinal", "timer_frequency"):
            a, b = view(4), view(5); b[1][field] += 1
            with self.subTest(field=field), self.assertRaises(ValueError): self.file(capture(sample(views=[a, b])))

    def test_actual_per_thread_pairing_and_explicit_record_group_and_reader_limits(self):
        left, right = sample(), sample(2, tid=8)
        events = left[:2] + right[:2]
        for a, b in zip(left[2:-1], right[2:-1]): events.extend((a, b))
        events.extend((left[-1], right[-1]))
        data = self.file(capture(events))["gpu_view_observations"]
        self.assertEqual(data["samples_count"], 2)
        self.assertEqual((data["first"]["thread"], data["last"]["thread"]), (7, 8))
        with self.assertRaises(ValueError): self.file(capture(events), max_active=1)
        with self.assertRaisesRegex(ValueError, "active record limit"):
            self.file(capture(sample(views=[view(4), view(5)])), max_active=1)
        with self.assertRaisesRegex(ValueError, "aggregate group limit"):
            self.file(capture(sample(views=[view(4), view(5)])), max_names=1)
        for value in (0, -1, True):
            with self.assertRaises(ValueError): gpu.GpuViewObservation(max_groups=value)
        with self.assertRaises(ValueError): self.file(capture(sample()), max_value_chars=8)

    def test_empty_cadence_scopes_are_not_missing_fields_and_named_zero_thread_is_canonical(self):
        data = self.file(capture(sample(views=[]), sample(2, timestamp=2000)))["gpu_view_observations"]
        self.assertEqual((data["samples_count"], data["submission_scopes_without_view_packets"]), (1, 1))
        document = capture(sample(tid=0))
        document["traceEvents"].insert(0, dict(ph="M", pid=1, tid=0, name="thread_name", args=dict(name="MAIN")))
        document["jceTrace"].update(mode="continuous", consumer_mode="cooperative", worker_available=False,
            write_ok=True, capture_complete=True, unexported=0, active_threads=0, active_work_items=0,
            active_waits=0, recorded=len(document["traceEvents"]), captured=len(document["traceEvents"]))
        self.assertEqual(self.file(document)["gpu_view_observations"]["first"]["thread"], 0)
        document["jceTrace"]["consumer_mode"] = "threaded"
        with self.assertRaises(ValueError): self.file(document)

    def test_canonical_eof_loss_hash_and_cli_option_are_not_bypassed(self):
        for mode in ("lost", "reset", "count", "unclosed"):
            document = capture(sample())
            if mode == "lost": document["jceTrace"].update(lost=1, cursor_lost=1)
            elif mode == "reset": document["jceTrace"]["capture_generation_stable"] = False
            elif mode == "count": document["jceTrace"]["recorded"] += 1
            else: document = capture(document["traceEvents"][:-1])
            with self.subTest(mode=mode), self.assertRaises(reporter.TraceFileError) as caught: self.file(document)
            self.assertEqual(caught.exception.evidence["sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())
        expected = self.file(capture(sample()))["gpu_view_observations"]
        for chunk in (1, 13, 1024):
            self.assertEqual(reporter.analyze_trace_file(self.path, [gpu.SERVICE],
                gpu_view_observations=True, chunk_bytes=chunk)["gpu_view_observations"], expected)
        output = self.path.parent / "summary.json"
        args = ["lifecycle_trace_report", str(self.path), "--require", gpu.SERVICE,
                "--out", str(output), "--gpu-view-observations"]
        with mock.patch.object(sys, "argv", args), redirect_stdout(io.StringIO()): self.assertEqual(reporter.main(), 0)
        self.assertEqual(json.loads(output.read_text())["gpu_view_observations"], expected)
        document = capture(sample()); document["traceEvents"].pop(3); document = capture(document["traceEvents"])
        self.path.write_text(json.dumps(document), encoding="utf-8")
        with mock.patch.object(sys, "argv", args), redirect_stdout(io.StringIO()): self.assertEqual(reporter.main(), 1)
        self.assertFalse(json.loads(output.read_text())["validation_complete"])
        for suffix in (b" trailing", b"\xff"):
            self.path.write_bytes(json.dumps(capture(sample())).encode() + suffix)
            with self.assertRaises(ValueError):
                reporter.analyze_trace_file(self.path, [gpu.SERVICE], gpu_view_observations=True, chunk_bytes=1)

    def test_ordinary_missing_wire_fails_unchanged_missing_view_assertion(self):
        result, raw = old_unwired_control()
        self.assertEqual((result.testsRun, len(result.failures), len(result.errors)), (1, 1, 0))
        self.assertIn("AssertionError: ValueError not raised", raw)


def old_unwired_control(stream=None):
    source = Path(reporter.__file__).read_text(encoding="utf-8")
    needle = '            self.gpu_views = GpuViewObservation(limits["active"] if limits else 8192, limits["names"] if limits else 4096)'
    if source.count(needle) != 1:
        raise AssertionError("unique canonical GPU view admission wire required")
    owner = types.ModuleType("tools.lifecycle_old_unwired_gpu_view")
    owner.__package__ = "tools"
    exec(compile(source.replace(needle, "            self.gpu_views = None", 1), reporter.__file__, "exec"), owner.__dict__)
    output = stream or io.StringIO()
    unchanged = GpuViewTests("test_requested_no_view_data_cannot_pass_and_legacy_default_is_unchanged")
    with mock.patch.object(reporter, "_TraceAnalysis", owner._TraceAnalysis):
        result = unittest.TextTestRunner(stream=output, verbosity=2).run(unittest.TestSuite([unchanged]))
    return result, output.getvalue() if hasattr(output, "getvalue") else ""


if __name__ == "__main__":
    if sys.argv[1:] == ["--ordinary-old-unwired"]:
        result, _ = old_unwired_control(sys.stdout)
        raise SystemExit(0 if result.wasSuccessful() else 1)
    unittest.main()
