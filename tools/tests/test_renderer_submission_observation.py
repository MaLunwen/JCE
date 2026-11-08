"""Typed submission controls through the actual canonical memory/stream owner."""
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
from tools import renderer_submission_observation as submission


def sample(identity=1, tid=7, timestamp=1000, values=None):
    values = values or (submission.INT64_MAX, submission.UINT32_MAX, 0, 0)
    base = dict(pid=1, tid=tid)
    events = [dict(base, ph="i", cat="task", ts=timestamp - 1, name=submission.SERVICE,
                   args=dict(task=identity, parent=0, kind=2)),
              dict(base, ph="B", cat="task", ts=timestamp, name=submission.SERVICE,
                   args=dict(id=identity, parent=identity))]
    events.extend(dict(base, ph="C", cat="render", ts=timestamp + 1 + index,
                       name=submission.PREFIX + field,
                       args=dict(id=identity, parent=identity, value=value))
                  for index, (field, value) in enumerate(zip(submission.FIELDS, values)))
    events.append(dict(base, ph="E", cat="task", ts=timestamp + 10, name=submission.SERVICE,
                       args=dict(id=identity, parent=identity, ns=10000, state=0)))
    return events


def capture(*groups):
    events = [event for group in groups for event in group]
    return dict(jceTrace=dict(recorded=len(events), captured=len(events), overwritten_before_capture=0,
                             cursor_lost=0, lost=0, capture_generation_stable=True), traceEvents=events)


def clock_sample(values=(10_000_000, 10_000, 10_009)):
    events = sample()
    events[-1:-1] = [dict(pid=1, tid=7, ph="C", cat="render", ts=1005 + index,
        name=submission.CLOCK_PREFIX + field, args=dict(id=1, parent=1, value=value))
        for index, (field, value) in enumerate(zip(submission.CLOCK_FIELDS, values))]
    return events


def tail_sample(tail_values=(10_000_000, 5_010, 16_000)):
    def span(name, ident, parent, begin, end):
        return [
            dict(pid=1, tid=7, ph="i", cat="task", ts=begin - 1, name=name,
                 args=dict(task=ident, parent=parent, kind=2)),
            dict(pid=1, tid=7, ph="B", cat="task", ts=begin, name=name,
                 args=dict(id=ident, parent=ident)),
            dict(pid=1, tid=7, ph="E", cat="task", ts=end, name=name,
                 args=dict(id=ident, parent=ident, ns=(end - begin) * 1000, state=0)),
        ]
    tail = span(submission.TAIL, 11, 0, 500, 1800)
    wrapper = span(submission.WRAPPER, 12, 11, 600, 1601)
    inner = clock_sample()
    inner[0]["args"]["parent"] = 12
    clocks = [dict(pid=1, tid=7, ph="C", cat="render", ts=1700 + index,
        name=submission.TAIL_CLOCK_PREFIX + field, args=dict(id=11, parent=11, value=value))
        for index, (field, value) in enumerate(zip(submission.CLOCK_FIELDS, tail_values))]
    return tail[:2] + wrapper[:2] + inner + wrapper[2:] + clocks + tail[2:]


class RendererSubmissionTests(unittest.TestCase):
    def setUp(self):
        output = Path(__file__).resolve().parents[2] / "build/tests/renderer-submission-observation"
        output.mkdir(parents=True, exist_ok=True)
        self.folder = tempfile.TemporaryDirectory(prefix="cpu-", dir=output)
        self.addCleanup(self.folder.cleanup)
        self.path = Path(self.folder.name) / "trace.json"

    def file(self, document, **kwargs):
        self.path.write_text(json.dumps(document, allow_nan=False), encoding="utf-8")
        return reporter.analyze_trace_file(self.path, [submission.SERVICE], renderer_submissions=True,
                                           chunk_bytes=7, **kwargs)

    def test_exact_wide_values_unknown_generation_zero_and_wrap(self):
        document = capture(sample((1 << 63) + 9, (1 << 53) + 7),
                           sample(2, timestamp=2000, values=(-1, 0, 0, submission.UINT32_MAX)),
                           sample(3, timestamp=3000, values=(9, 7, 7, 0)))
        memory = reporter.analyze_trace(document, [submission.SERVICE], renderer_submissions=True)
        streamed = self.file(document)
        data = streamed["renderer_submissions"]
        self.assertEqual(data, memory["renderer_submissions"])
        self.assertEqual((data["packets_count"], data["context_generation_known_count"],
                          data["context_generation_unknown_count"], data["backend_serial_decreased_count"]), (3, 2, 1, 1))
        self.assertEqual(data["first"]["values"]["context_generation"], str(submission.INT64_MAX))
        self.assertEqual(data["first"]["scope_id"], str((1 << 63) + 9))
        self.assertEqual(data["first"]["thread"], str((1 << 53) + 7))
        self.assertEqual(data["first"]["backend_serial_delta_modulo_uint32"], 1)
        self.assertEqual(data["last"]["backend_serial_delta_modulo_uint32"], 0)
        self.assertTrue(data["first"]["context_generation_known"])
        self.assertEqual(data["first"]["field_raw_indices"], dict(zip(submission.FIELDS, range(2, 6))))
        self.assertEqual(streamed["sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())
        self.assertTrue(streamed["validation_complete"])
        self.assertIn("no GPU completion", data["scope"])
        unknown = self.file(capture(sample(values=(-1, 0, 0, 0))))["renderer_submissions"]["first"]
        self.assertFalse(unknown["context_generation_known"])
        self.assertEqual(unknown["values"]["context_generation"], -1)

    def test_optional_raw_edges_keep_the_existing_packet_identity(self):
        self.assertNotIn("performance_counter", self.file(capture(sample()))["renderer_submissions"]["first"])
        raw = (10_000_000, (1 << 53) + 9, (1 << 53) + 9)
        result = self.file(capture(clock_sample(raw), sample(2, timestamp=2000)))["renderer_submissions"]
        self.assertEqual(result["performance_counter_packet_count"], 1)
        self.assertEqual(result["first"]["performance_counter"], dict(domain="jce-performance-counter",
            frequency_hz=raw[0], begin=str(raw[1]), end=str(raw[2]),
            field_raw_indices=dict(zip(submission.CLOCK_FIELDS, range(6, 9)))))
        self.assertNotIn("performance_counter", result["last"])

    def test_raw_edges_reject_incomplete_misplaced_and_invalid_records(self):
        for mode in ("missing", "duplicate", "reordered", "before-identity", "unknown",
                     "thread", "wrong-scope", "zero-frequency", "reversed", "wide", "string"):
            events = clock_sample()
            if mode == "missing": events.pop(7)
            elif mode == "duplicate": events.insert(7, copy.deepcopy(events[6]))
            elif mode == "reordered": events[6], events[7] = events[7], events[6]
            elif mode == "before-identity": events.insert(2, events.pop(6))
            elif mode == "unknown": events[6]["name"] = submission.CLOCK_PREFIX + "bad"
            elif mode == "thread": events[6]["tid"] = 8
            elif mode == "wrong-scope": events[6]["args"].update(id=2, parent=2)
            elif mode == "zero-frequency": events[6]["args"]["value"] = 0
            elif mode == "reversed": events[8]["args"]["value"] = 9_999
            elif mode == "wide": events[8]["args"]["value"] = 1 << 63
            elif mode == "string": events[7]["args"]["value"] = "10000"
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                self.file(capture(events))

    def test_engine_tail_joins_only_its_submitted_wrapper_and_api(self):
        document = capture(tail_sample())
        report = self.file(document)["renderer_submissions"]
        memory = reporter.analyze_trace(document, [submission.SERVICE], renderer_submissions=True)
        self.assertEqual(report, memory["renderer_submissions"])
        tails = report["engine_renderer_tails"]
        self.assertEqual((tails["observed_count"], tails["clock_joined_count"]), (1, 1))
        tail = tails["first"]
        self.assertTrue(tail["api_interval_available"])
        self.assertEqual((tail["scope_id"], tail["thread"], tail["state"]), (11, 7, 0))
        self.assertEqual(tail["api_advance"], report["first"])
        self.assertEqual((tail["wrapper_scope_id"], tail["wrapper_state"]), (12, 0))
        events = tail_sample()
        events[4]["args"]["parent"] = 0
        tails = self.file(capture(events))["renderer_submissions"]["engine_renderer_tails"]
        self.assertEqual(tails["clock_joined_count"], 0)
        self.assertFalse(tails["first"]["api_interval_available"])

    def test_engine_tail_rejects_mismatched_edges_and_partial_packets(self):
        for values in ((1_000_000, 5_010, 16_000), (10_000_000, 10_001, 16_000),
                       (10_000_000, 5_010, 10_008), (10_000_000, 5_010)):
            with self.subTest(values=values), self.assertRaises(ValueError):
                self.file(capture(tail_sample(values)))

    def test_engine_tail_rejects_early_wrapper_closure(self):
        events = tail_sample()
        wrap_end = next(i for i,e in enumerate(events)
                        if e["ph"] == "E" and e["name"] == submission.WRAPPER)
        advance_end = wrap_end - 1
        events[advance_end]["ts"], events[wrap_end]["ts"] = events[wrap_end]["ts"], events[advance_end]["ts"]
        events[advance_end], events[wrap_end] = events[wrap_end], events[advance_end]
        with self.assertRaisesRegex(ValueError, "wrapper closed before"):
            self.file(capture(events))

    def test_failed_engine_tail_does_not_invent_an_api_interval(self):
        events = tail_sample()
        tail_only = events[:2] + events[-4:]
        tail_only[-1]["args"]["state"] = 1
        tails = self.file(capture(tail_only, sample(2, timestamp=2000)))["renderer_submissions"]["engine_renderer_tails"]
        self.assertEqual(tails["clock_joined_count"], 0)
        self.assertEqual(tails["first"]["state"], 1)
        self.assertFalse(tails["first"]["api_interval_available"])
        self.assertIsNone(tails["first"]["api_advance"])

    def test_requested_missing_packets_is_not_a_prefix_pass(self):
        document = capture(sample())
        for event in document["traceEvents"]:
            if event["name"] == submission.SERVICE:
                event["name"] = "app.init"
        document = capture([event for event in document["traceEvents"] if event["ph"] != "C"])
        self.assertNotIn("renderer_submissions", reporter.analyze_trace(document, ["app.init"]))
        with self.assertRaisesRegex(ValueError, "no renderer submission packets"):
            reporter.analyze_trace(document, ["app.init"], renderer_submissions=True)

    def test_default_mode_ignores_packet_schema_and_option_requires_bool(self):
        document = capture(sample())
        document["traceEvents"][2]["args"]["value"] = 0
        self.assertNotIn("renderer_submissions", reporter.analyze_trace(document, [submission.SERVICE]))
        with self.assertRaisesRegex(ValueError, "explicit bool"):
            reporter.analyze_trace(document, [submission.SERVICE], renderer_submissions=1)

    def test_fields_are_unique_complete_and_in_actual_producer_order(self):
        for mode in ("missing", "duplicate", "unknown", "reordered", "all-missing"):
            events = sample()
            if mode == "missing": events.pop(3)
            elif mode == "duplicate": events.insert(3, copy.deepcopy(events[2]))
            elif mode == "unknown": events[2]["name"] = submission.PREFIX + "extra"
            elif mode == "reordered": events[2], events[3] = events[3], events[2]
            else: events = [event for event in events if event["ph"] != "C"]
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                self.file(capture(events))

    def test_typed_values_reject_coercion_and_out_of_range(self):
        invalid = ((0, 0), (0, -2), (0, 1 << 63), (0, True), (0, 1.0), (0, "1"),
                   (1, -1), (1, 1 << 32), (1, False), (1, .5), (2, 1 << 32), (3, -1))
        for field, value in invalid:
            events = sample(); events[2 + field]["args"]["value"] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                self.file(capture(events))

    def test_wrong_id_parent_thread_category_or_identity_type_is_rejected(self):
        for key, value in (("id", 2), ("parent", 2), ("id", True), ("parent", 1.0),
                           ("id", 1 << 64), ("id", 0)):
            events = sample(); events[2]["args"][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                self.file(capture(events))
        for key, value in (("tid", 8), ("cat", "core")):
            events = sample(); events[2][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): self.file(capture(events))

    def test_counters_before_begin_after_end_and_outside_wall_interval_fail(self):
        for mode in ("before-begin", "after-end", "before-wall", "after-wall", "reverse-wall"):
            events = sample()
            if mode == "before-begin": events.insert(1, events.pop(2))
            elif mode == "after-end": events.append(events.pop(5))
            elif mode == "before-wall": events[2]["ts"] = 999
            elif mode == "after-wall": events[5]["ts"] = 1011
            else: events[3]["ts"] = 1000
            with self.subTest(mode=mode), self.assertRaises(ValueError): self.file(capture(events))

    def test_pairing_uses_real_per_thread_scope_identity_not_global_lifo(self):
        left, right = sample(), sample(2, tid=8)
        events = left[:2] + right[:2]
        for a, b in zip(left[2:6], right[2:6]): events.extend((a, b))
        events.extend((left[-1], right[-1]))
        report = self.file(capture(events))["renderer_submissions"]
        self.assertEqual(report["packets_count"], 2)
        self.assertEqual((report["first"]["thread"], report["last"]["thread"]), (7, 8))
        with self.assertRaisesRegex(ValueError, "active scope limit"):
            self.file(capture(events), max_active=1)

    def test_scope_must_be_submitted_successful_and_closed(self):
        for mode in ("no-submit", "wrong-kind", "wrong-category", "wrong-parent", "failed", "cancelled", "unclosed"):
            events = sample()
            if mode == "no-submit": events.pop(0)
            elif mode == "wrong-kind": events[0]["args"]["kind"] = 0
            elif mode == "wrong-category": events[1]["cat"] = events[-1]["cat"] = "render"
            elif mode == "wrong-parent": events[1]["args"]["parent"] = events[-1]["args"]["parent"] = 0
            elif mode in ("failed", "cancelled"): events[-1]["args"]["state"] = 1 if mode == "failed" else 2
            else: events.pop()
            with self.subTest(mode=mode), self.assertRaises(ValueError): self.file(capture(events))

    def test_named_cooperative_zero_thread_keeps_canonical_authority(self):
        document = capture(sample(tid=0))
        document["traceEvents"].insert(0, dict(ph="M", pid=1, tid=0, name="thread_name", args=dict(name="MAIN")))
        document["jceTrace"].update(mode="continuous", consumer_mode="cooperative", worker_available=False,
            write_ok=True, capture_complete=True, unexported=0, active_threads=0, active_work_items=0,
            active_waits=0, recorded=8, captured=8)
        self.assertEqual(self.file(document)["renderer_submissions"]["first"]["thread"], 0)
        document["jceTrace"]["consumer_mode"] = "threaded"
        with self.assertRaises(ValueError): self.file(document)

    def test_eof_loss_counts_and_complete_hash_are_not_bypassed(self):
        for mode in ("lost", "reset", "count", "unclosed"):
            document = capture(sample())
            if mode == "lost": document["jceTrace"].update(lost=1, cursor_lost=1)
            elif mode == "reset": document["jceTrace"]["capture_generation_stable"] = False
            elif mode == "count": document["jceTrace"]["recorded"] += 1
            else: document = capture(document["traceEvents"][:-1])
            with self.subTest(mode=mode), self.assertRaises(reporter.TraceFileError) as caught: self.file(document)
            self.assertEqual(caught.exception.evidence["sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())
        for suffix in (b" trailing", b"\xff"):
            self.path.write_bytes(json.dumps(capture(sample())).encode() + suffix)
            with self.assertRaises(ValueError):
                reporter.analyze_trace_file(self.path, [submission.SERVICE], renderer_submissions=True, chunk_bytes=1)

    def test_cli_option_and_tiny_chunks_consume_the_same_packets(self):
        document = capture(sample()); expected = self.file(document)["renderer_submissions"]
        for chunk in (1, 13, 1024):
            report = reporter.analyze_trace_file(self.path, [submission.SERVICE], renderer_submissions=True, chunk_bytes=chunk)
            self.assertEqual(report["renderer_submissions"], expected)
        output = Path(self.folder.name) / "summary.json"
        args = ["lifecycle_trace_report", str(self.path), "--require", submission.SERVICE,
                "--out", str(output), "--renderer-submissions"]
        with mock.patch.object(sys, "argv", args), redirect_stdout(io.StringIO()):
            self.assertEqual(reporter.main(), 0)
        self.assertEqual(json.loads(output.read_text())["renderer_submissions"], expected)
        document["traceEvents"].pop(2); document = capture(document["traceEvents"])
        self.path.write_text(json.dumps(document), encoding="utf-8")
        with mock.patch.object(sys, "argv", args), redirect_stdout(io.StringIO()):
            self.assertEqual(reporter.main(), 1)
        failure = json.loads(output.read_text())
        self.assertEqual(failure["status"], "INVALID_OR_INCOMPLETE_CAPTURE")
        self.assertFalse(failure["validation_complete"])
        self.assertEqual(failure["sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())

    def test_ordinary_old_unwired_owner_fails_unchanged_missing_packet_assertion(self):
        result, raw = old_unwired_control()
        self.assertEqual((result.testsRun, len(result.failures), len(result.errors)), (1, 1, 0))
        self.assertIn("AssertionError: ValueError not raised", raw)


def old_unwired_control(stream=None):
    source = Path(reporter.__file__).read_text(encoding="utf-8")
    needle = '            self.submissions = RendererSubmissionObservation(limits["active"] if limits else 8192)'
    if source.count(needle) != 1:
        raise AssertionError("unique canonical submission admission wire required")
    owner = types.ModuleType("tools.lifecycle_old_unwired_submission")
    owner.__package__ = "tools"
    exec(compile(source.replace(needle, "            self.submissions = None", 1), reporter.__file__, "exec"), owner.__dict__)
    output = stream or io.StringIO()
    unchanged = RendererSubmissionTests("test_requested_missing_packets_is_not_a_prefix_pass")
    with mock.patch.object(reporter, "_TraceAnalysis", owner._TraceAnalysis):
        result = unittest.TextTestRunner(stream=output, verbosity=2).run(unittest.TestSuite([unchanged]))
    return result, output.getvalue() if hasattr(output, "getvalue") else ""


if __name__ == "__main__":
    if sys.argv[1:] == ["--ordinary-old-unwired"]:
        result, _ = old_unwired_control(sys.stdout)
        raise SystemExit(0 if result.wasSuccessful() else 1)
    unittest.main()
