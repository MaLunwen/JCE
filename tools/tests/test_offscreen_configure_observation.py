"""Focused offscreen controls through the canonical memory/stream/CLI reader."""
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
from tools import lifecycle_trace_report as reporter
from tools import offscreen_configure_observation as offscreen


def sample(scope=101, serial=(1 << 63) + 9, tid=7, timestamp=1000, state=0,
           values=Ellipsis, parent=0, service=offscreen.SERVICE, **changes):
    if values is Ellipsis:
        values = dict(creation_serial=serial if serial < 1 << 63 else serial - (1 << 64),
            origin_capture_generation=5, current_capture_generation=5,
            origin_observed=1, origin_same_capture=1, origin_capture_gap=0,
            view_id=30, width=64, height=48, color_format=71, depth_format=88,
            samples=1, fbo_handle=0, color_handle=0, depth_handle=1,
            logical_bytes=64 * 48 * 12, requested_logical_bytes=64 * 48 * 12,
            retired_ordinal=0, resource_reason=0, forwarded_rect_width=64,
            forwarded_rect_height=48, submission_ordinal=0)
    values = dict(values or {}); values.update(changes)
    base = dict(pid=1, tid=tid)
    events = [dict(base, ph="i", cat="task", ts=timestamp - 1, name=service,
                   args=dict(task=scope, parent=parent, kind=2)),
              dict(base, ph="B", cat="task", ts=timestamp, name=service,
                   args=dict(id=scope, parent=scope))]
    events.extend(dict(base, ph="C", cat="render", ts=timestamp + 1 + n,
        name="offscreen." + field, args=dict(id=serial, parent=scope, value=value))
        for n, (field, value) in enumerate(values.items()))
    events.append(dict(base, ph="E", cat="task", ts=timestamp + 30, name=service,
        args=dict(id=scope, parent=scope, ns=30000, state=state)))
    return events


def capture(*groups, init_tid=7):
    init = [dict(pid=1, tid=init_tid, ph="i", cat="task", ts=1, name="app.init",
                 args=dict(task=1, parent=0, kind=2)),
            dict(pid=1, tid=init_tid, ph="B", cat="task", ts=2, name="app.init",
                 args=dict(id=1, parent=1)),
            dict(pid=1, tid=init_tid, ph="E", cat="task", ts=3, name="app.init",
                 args=dict(id=1, parent=1, ns=1000, state=0))]
    events = init + [event for group in groups for event in group]
    return dict(jceTrace=dict(recorded=len(events), captured=len(events),
        overwritten_before_capture=0, cursor_lost=0, lost=0,
        capture_generation_stable=True), traceEvents=events)


class OffscreenConfigureTests(unittest.TestCase):
    def setUp(self):
        output = Path(__file__).resolve().parents[2] / "build/tests/offscreen-configure-observation"
        output.mkdir(parents=True, exist_ok=True)
        self.folder = tempfile.TemporaryDirectory(prefix="cpu-", dir=output)
        self.addCleanup(self.folder.cleanup)
        self.path = Path(self.folder.name) / "trace.json"

    def file(self, document, **kwargs):
        self.path.write_text(json.dumps(document, allow_nan=False), encoding="utf-8")
        return reporter.analyze_trace_file(self.path, ["app.init"],
            offscreen_configurations=True, chunk_bytes=7, **kwargs)

    def test_wide_serial_main_overlay_and_lifecycle_fields_keep_exact_ownership(self):
        main = sample()
        old = {e["name"][10:]: e["args"]["value"] for e in main[2:-1]
               if e["name"][10:] not in ("forwarded_rect_width", "forwarded_rect_height")}
        nested = sample(scope=201, timestamp=1001, parent=101,
            service="renderer.offscreen.create", values=old)
        for event in main[2:]: event["ts"] += 40
        document = capture(main[:2] + nested + main[2:],
            sample(scope=102, timestamp=2000, view_id=31, submission_ordinal=offscreen.UINT32_MAX),
            sample(scope=103, timestamp=3000, submission_ordinal=0))
        streamed = self.file(document)
        data = streamed["offscreen_configurations"]
        self.assertEqual(data, reporter.analyze_trace(document, ["app.init"],
            offscreen_configurations=True)["offscreen_configurations"])
        self.assertEqual((data["packets_count"], data["unclassified_scopes_count"]), (3, 0))
        self.assertEqual(data["first"]["values"]["creation_serial"], str((1 << 63) + 9))
        self.assertEqual(data["first"]["creation_serial_wire"], str(-(1 << 63) + 9))
        self.assertEqual(data["first"]["scope_id"], 101)
        self.assertEqual(data["first"]["values"]["fbo_handle"], 0)
        self.assertEqual(len(data["first"]["field_raw_indices"]), 22)
        self.assertEqual(data["last"]["values"]["submission_ordinal"], 0)
        self.assertFalse(data["gpu_completion_known"]); self.assertIsNone(data["gpu_completion"])
        self.assertTrue(streamed["validation_complete"])
        self.assertEqual(streamed["sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())
        for serial in ((1 << 53) + 1, (1 << 63) - 1, (1 << 64) - 1):
            witness = self.file(capture(sample(serial=serial)))["offscreen_configurations"]["first"]
            self.assertEqual(witness["values"]["creation_serial"], str(serial))

    def test_empty_shared_scopes_are_unclassified_and_never_all_offscreen_coverage(self):
        data = self.file(capture(sample(values=None), sample(scope=102, timestamp=2000, state=1, values=None),
            sample(scope=103, timestamp=3000, state=2, values=None),
            sample(scope=104, timestamp=4000)))["offscreen_configurations"]
        self.assertEqual((data["shared_view_scopes_count"], data["packets_count"],
            data["unclassified_scopes_count"]), (4, 1, 3))
        self.assertEqual((data["unclassified_succeeded_count"], data["unclassified_failed_count"],
            data["unclassified_cancelled_count"]), (1, 1, 1))
        self.assertIn("not all-offscreen coverage", data["coverage"])
        document = capture(sample(values=None), sample(scope=102, timestamp=2000, state=1, values=None))
        with self.assertRaises(ValueError):
            reporter.analyze_trace(document, [offscreen.SERVICE], offscreen_configurations=True)
        for group in (sample(values=None), sample(values=None, state=1)):
            with self.subTest(group=group), self.assertRaisesRegex(ValueError, "no complete successful packet"):
                self.file(capture(group))

    def test_explicit_request_without_complete_packets_is_not_acceptance(self):
        document = capture()
        self.assertNotIn("offscreen_configurations", reporter.analyze_trace(document, ["app.init"]))
        with self.assertRaisesRegex(ValueError, "no complete successful packet"):
            self.file(document)

    def test_default_mode_and_boolean_option_preserve_existing_reader(self):
        document = capture(sample()); document["traceEvents"].pop(6)
        document["jceTrace"]["recorded"] = document["jceTrace"]["captured"] = len(document["traceEvents"])
        self.assertNotIn("offscreen_configurations", reporter.analyze_trace(document, ["app.init"]))
        for value in (1, None, "yes"):
            with self.assertRaisesRegex(ValueError, "explicit bool"):
                reporter.analyze_trace(document, ["app.init"], offscreen_configurations=value)

    def test_each_field_missing_duplicate_unknown_and_reordered_is_rejected(self):
        self.assertEqual(len(sample()[2:-1]), 22)
        for field in range(22):
            for mode in ("missing", "duplicate"):
                events = sample()
                if mode == "missing": events.pop(2 + field)
                else: events.insert(2 + field, copy.deepcopy(events[2 + field]))
                with self.subTest(field=field, mode=mode), self.assertRaises(ValueError):
                    self.file(capture(events))
        for mode in ("unknown", "reordered"):
            events = sample()
            if mode == "unknown": events[2]["name"] = "offscreen.extra"
            else: events[2]["name"], events[3]["name"] = events[3]["name"], events[2]["name"]
            with self.subTest(mode=mode), self.assertRaises(ValueError): self.file(capture(events))

    def test_wire_integer_types_and_cross_field_invariants_are_strict(self):
        for field in range(22):
            for value in (True, 1.0, "1"):
                events = sample(); events[2 + field]["args"]["value"] = value
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    self.file(capture(events))
        invalid = dict(creation_serial=1 << 63, origin_capture_generation=1 << 32,
            current_capture_generation=-1, origin_observed=2, origin_same_capture=0,
            origin_capture_gap=1, view_id=1 << 16, width=15, height=8193,
            color_format=-1, depth_format=1 << 32, samples=2, fbo_handle=65535,
            color_handle=-1, depth_handle=65535, logical_bytes=0, requested_logical_bytes=1,
            retired_ordinal=1, resource_reason=5, forwarded_rect_width=63,
            forwarded_rect_height=47, submission_ordinal=1 << 32)
        for field, value in invalid.items():
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.file(capture(sample(**{field: value})))

    def test_generations_gap_flags_and_unsigned_ordinal_zero_wrap(self):
        document = capture(sample(origin_observed=0, origin_same_capture=0, origin_capture_gap=1),
            sample(scope=102, timestamp=2000, origin_capture_generation=3,
                origin_same_capture=0, origin_capture_gap=1, submission_ordinal=offscreen.UINT32_MAX),
            sample(scope=103, timestamp=3000, submission_ordinal=0))
        self.assertEqual(self.file(document)["offscreen_configurations"]["packets_count"], 3)
        with self.assertRaisesRegex(ValueError, "generation changed"):
            self.file(capture(sample(), sample(scope=102, timestamp=2000,
                current_capture_generation=6, origin_same_capture=0, origin_capture_gap=1)))
        zero = sample(origin_capture_generation=0, current_capture_generation=0)
        self.assertEqual(self.file(capture(zero))["offscreen_configurations"]["current_capture_generation"], 0)

    def test_wrong_serial_scope_parent_thread_category_and_orphan_fields_fail(self):
        for key, value in (("id", 0), ("id", 9), ("id", True), ("id", 1 << 64),
                           ("parent", 77), ("parent", 1.0)):
            events = sample(); events[2]["args"][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError): self.file(capture(events))
        for key, value in (("tid", 8), ("cat", "core")):
            events = sample(); events[2][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): self.file(capture(events))
        for field in (0, 19, 20):
            events = sample(); orphan = copy.deepcopy(events[2 + field]); orphan["ts"] = 1040
            events.append(orphan)
            with self.subTest(orphan=field), self.assertRaises(ValueError): self.file(capture(events))
        events = sample(); events[2], events[1] = events[1], events[2]
        with self.assertRaises(ValueError): self.file(capture(events))

    def test_failed_partial_or_unsubmitted_target_service_cannot_publish_a_packet(self):
        for mode in ("failed", "cancelled", "no-submit", "wrong-kind", "wrong-category", "unclosed", "submit-only"):
            events = sample()
            if mode in ("failed", "cancelled"): events[-1]["args"]["state"] = 1 if mode == "failed" else 2
            elif mode == "no-submit": events.pop(0)
            elif mode == "wrong-kind": events[0]["args"]["kind"] = 0
            elif mode == "wrong-category": events[1]["cat"] = events[-1]["cat"] = "render"
            elif mode == "unclosed": events.pop()
            else: events = events[:1]
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                self.file(capture(sample(scope=99, timestamp=100), events))

    def test_interleaved_threads_and_existing_canonical_state_limits(self):
        left, right = sample(), sample(scope=102, tid=8)
        events = left[:2] + right[:2]
        for a, b in zip(left[2:-1], right[2:-1]): events.extend((a, b))
        events.extend((left[-1], right[-1]))
        document = capture(events)
        self.assertEqual(self.file(document)["offscreen_configurations"]["packets_count"], 2)
        for limit in (dict(max_active=1), dict(max_history=1), dict(max_value_chars=8)):
            with self.subTest(limit=limit), self.assertRaises(ValueError): self.file(document, **limit)
        for limit in (0, -1, True):
            with self.assertRaises(ValueError): offscreen.OffscreenConfigureObservation(limit)

    def test_cooperative_tid_zero_retains_the_canonical_identity_gate(self):
        document = capture(sample(tid=0), init_tid=0)
        document["traceEvents"].insert(0, dict(ph="M", pid=1, tid=0, name="thread_name", args=dict(name="MAIN")))
        document["jceTrace"].update(mode="continuous", consumer_mode="cooperative", worker_available=False,
            write_ok=True, capture_complete=True, unexported=0, active_threads=0, active_work_items=0,
            active_waits=0, recorded=len(document["traceEvents"]), captured=len(document["traceEvents"]))
        self.assertEqual(self.file(document)["offscreen_configurations"]["first"]["thread"], 0)
        document["jceTrace"]["consumer_mode"] = "threaded"
        with self.assertRaises(ValueError): self.file(document)

    def test_complete_eof_loss_fingerprint_and_source_change_are_not_bypassed(self):
        for mode in ("lost", "reset", "count", "exhausted", "active"):
            document = capture(sample())
            if mode == "lost": document["jceTrace"].update(lost=1, cursor_lost=1)
            elif mode == "reset": document["jceTrace"]["capture_generation_stable"] = False
            elif mode == "count": document["jceTrace"]["recorded"] += 1
            elif mode == "exhausted": document["jceTrace"]["sequence_exhausted"] = True
            else: document["jceTrace"].update(mode="continuous", consumer_mode="threaded", worker_available=True,
                write_ok=True, capture_complete=True, unexported=0, active_threads=0, active_work_items=1, active_waits=0)
            with self.subTest(mode=mode), self.assertRaises(reporter.TraceFileError) as caught: self.file(document)
            self.assertEqual(caught.exception.evidence["sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())
        for suffix in (b" trailing", b"\xff"):
            self.path.write_bytes(json.dumps(capture(sample())).encode() + suffix)
            with self.assertRaises(ValueError):
                reporter.analyze_trace_file(self.path, ["app.init"], offscreen_configurations=True, chunk_bytes=1)
        original = reporter._JsonStream.read_envelope
        def changed(reader, feed):
            metadata = original(reader, feed)
            with self.path.open("ab") as stream: stream.write(b" ")
            return metadata
        with mock.patch.object(reporter._JsonStream, "read_envelope", changed):
            with self.assertRaisesRegex(reporter.TraceFileError, "fingerprint changed"): self.file(capture(sample()))

    def test_cli_tiny_chunks_and_normal_missing_hook_failure_use_the_same_owner(self):
        document = capture(sample()); expected = self.file(document)["offscreen_configurations"]
        for chunk in (1, 13, 1024):
            self.assertEqual(reporter.analyze_trace_file(self.path, ["app.init"],
                offscreen_configurations=True, chunk_bytes=chunk)["offscreen_configurations"], expected)
        output = self.path.parent / "summary.json"
        args = ["lifecycle_trace_report", str(self.path), "--require", "app.init",
                "--out", str(output), "--offscreen-configurations"]
        with mock.patch.object(sys, "argv", args), redirect_stdout(io.StringIO()): self.assertEqual(reporter.main(), 0)
        self.assertEqual(json.loads(output.read_text())["offscreen_configurations"], expected)
        self.path.write_text(json.dumps(capture(sample(values=None))), encoding="utf-8")
        with mock.patch.object(sys, "argv", args), redirect_stdout(io.StringIO()): self.assertEqual(reporter.main(), 1)
        failure = json.loads(output.read_text())
        self.assertFalse(failure["validation_complete"])
        self.assertEqual(failure["sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())
        result, raw = old_unwired_control()
        self.assertEqual((result.testsRun, len(result.failures), len(result.errors)), (1, 1, 0))
        self.assertIn("AssertionError: ValueError not raised", raw)


def old_unwired_control(stream=None):
    source = Path(reporter.__file__).read_text(encoding="utf-8")
    needle = '            self.offscreen = OffscreenConfigureObservation(limits["active"] if limits else 8192)'
    if source.count(needle) != 1:
        raise AssertionError("unique canonical offscreen admission wire required")
    owner = types.ModuleType("tools.lifecycle_old_unwired_offscreen")
    owner.__package__ = "tools"
    exec(compile(source.replace(needle, "            self.offscreen = None", 1),
                 reporter.__file__, "exec"), owner.__dict__)
    output = stream or io.StringIO()
    unchanged = OffscreenConfigureTests("test_explicit_request_without_complete_packets_is_not_acceptance")
    with mock.patch.object(reporter, "_TraceAnalysis", owner._TraceAnalysis):
        result = unittest.TextTestRunner(stream=output, verbosity=2).run(unittest.TestSuite([unchanged]))
    return result, output.getvalue() if hasattr(output, "getvalue") else ""


if __name__ == "__main__":
    if sys.argv[1:] == ["--ordinary-old-unwired"]:
        result, _ = old_unwired_control(sys.stdout)
        raise SystemExit(0 if result.wasSuccessful() else 1)
    unittest.main()
