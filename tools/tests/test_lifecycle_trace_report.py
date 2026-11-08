"""Incomplete, failed and malformed traces must not pass lifecycle acceptance."""
import copy
import hashlib
import io
import inspect
import json
import tempfile
from contextlib import redirect_stderr, redirect_stdout
from unittest import mock
import unittest
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools import lifecycle_trace_report as reporter
from tools.lifecycle_trace_report import analyze_trace, analyze_trace_file, TraceFileError


def capture():
    return {"jceTrace": {"recorded": 3, "captured": 3, "overwritten_before_capture": 0,
        "cursor_lost": 0, "lost": 0, "capture_generation_stable": True},
        "traceEvents": [
            {"ph": "i", "cat": "task", "pid": 1, "tid": 1, "ts": 900, "name": "app.init", "args": {"task": 1, "parent": 0, "kind": 2}},
            {"ph": "B", "cat": "task", "pid": 1, "tid": 1, "ts": 1000, "name": "app.init", "args": {"id": 1, "parent": 1}},
            {"ph": "E", "cat": "task", "pid": 1, "tid": 1, "ts": 2000, "name": "app.init", "args": {"id": 1, "parent": 1, "ns": 999000, "state": 0}}]}


def wait_capture():
    doc = capture()
    doc["traceEvents"][2:2] = [
        {"ph": "B", "cat": "wait", "pid": 1, "tid": 1, "ts": 1100,
         "name": "trace.capture.join", "args": {"id": 9, "parent": 1}},
        {"ph": "E", "cat": "wait", "pid": 1, "tid": 1, "ts": 1200,
         "name": "trace.capture.join", "args": {"id": 9, "parent": 1, "ns": 100000}}]
    doc["jceTrace"].update(recorded=5, captured=5)
    return doc


class WriterDiagnosticsTests(unittest.TestCase):
    def document(self):
        doc = capture()
        doc["jceTrace"].update(mode="continuous", consumer_mode="threaded",
            worker_available=True, write_ok=True, capture_complete=True,
            unexported=0, active_threads=0, active_work_items=0, active_waits=0,
            writer_append_calls=2, writer_payload_bytes=32000,
            writer_buffer_capacity=65536, writer_buffer_limit=1572865,
            writer_max_append_ns=2000000,
            writer_diagnostics=dict(version=1, operation="payload-write", flush_delay_ns=16000000,
                size_counts=[0, 1, 1, 0, 0, 0], wall_ns_counts=[1, 0, 1, 0, 0, 0, 0],
                slowest_started_ns=100, slowest_finished_ns=2000100, slowest_requested_bytes=20000))
        return doc

    def test_optional_diagnostics_preserve_old_capture_and_both_consumers(self):
        doc = self.document()
        analyze_trace(doc, ["app.init"])
        with tempfile.TemporaryDirectory(prefix="jce-writer-diagnostics-") as folder:
            path = Path(folder) / "trace.json"
            path.write_text(json.dumps(doc), encoding="utf-8")
            report = analyze_trace_file(path, ["app.init"], chunk_bytes=7)
            self.assertEqual(report["capture"]["writer_diagnostics"], doc["jceTrace"]["writer_diagnostics"])
        doc["jceTrace"].update(consumer_mode="cooperative", worker_available=False)
        doc["jceTrace"]["writer_diagnostics"]["flush_delay_ns"] = 0
        analyze_trace(doc, ["app.init"])
        del doc["jceTrace"]["writer_diagnostics"]
        analyze_trace(doc, ["app.init"])

    def test_diagnostics_reject_wrong_types_bounds_and_counter_accounting(self):
        mutations = (
            ("version", True), ("version", 2), ("operation", "all-file-io"),
            ("flush_delay_ns", 0), ("size_counts", [0] * 6), ("size_counts", [False, 1, 1, 0, 0, 0]),
            ("wall_ns_counts", [1, 0, 1]), ("wall_ns_counts", [1, 0, 0, 1, 0, 0, 0]),
            ("slowest_started_ns", 2000101), ("slowest_finished_ns", 2000101),
            ("slowest_requested_bytes", 0), ("slowest_requested_bytes", 40000),
            ("slowest_requested_bytes", 100), ("slowest_started_ns", 1 << 64))
        for key, value in mutations:
            bad = self.document(); bad["jceTrace"]["writer_diagnostics"][key] = value
            with self.subTest(key=key, value=value), self.assertRaisesRegex(ValueError, "writer"):
                analyze_trace(bad, ["app.init"])
        for key, value in (("writer_append_calls", True), ("writer_payload_bytes", 1),
                           ("writer_payload_bytes", 1000000), ("writer_buffer_limit", 0),
                           ("writer_buffer_capacity", 1572866), ("writer_buffer_capacity", 0),
                           ("writer_buffer_capacity", 100), ("writer_max_append_ns", 1 << 64)):
            bad = self.document(); bad["jceTrace"][key] = value
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "writer"):
                analyze_trace(bad, ["app.init"])
        for key in self.document()["jceTrace"]["writer_diagnostics"]:
            bad = self.document(); del bad["jceTrace"]["writer_diagnostics"][key]
            with self.subTest(missing=key), self.assertRaisesRegex(ValueError, "writer"):
                analyze_trace(bad, ["app.init"])

    def test_no_payload_write_has_only_zero_observations(self):
        metadata = self.document()["jceTrace"]
        metadata.update(captured=0, writer_append_calls=0, writer_payload_bytes=0,
                        writer_buffer_capacity=0, writer_max_append_ns=0)
        metadata["writer_diagnostics"].update(size_counts=[0] * 6, wall_ns_counts=[0] * 7,
            slowest_started_ns=0, slowest_finished_ns=0, slowest_requested_bytes=0)
        reporter._writer_diagnostics(metadata)
        metadata["writer_diagnostics"]["slowest_requested_bytes"] = 1
        with self.assertRaisesRegex(ValueError, "writer"):
            reporter._writer_diagnostics(metadata)

    def test_clock_domain_is_explicit_and_legacy_stays_unknown(self):
        for domain in (None, "process-perf-ns"):
            doc = self.document()
            if domain is not None:
                doc["jceTrace"]["writer_diagnostics"]["clock_domain"] = domain
            expected = domain or "unknown"
            self.assertEqual(analyze_trace(doc, ["app.init"])["writer_clock_domain"], expected)
            with tempfile.TemporaryDirectory(prefix="jce-writer-clock-") as folder:
                path = Path(folder) / "trace.json"
                path.write_text(json.dumps(doc), encoding="utf-8")
                report = analyze_trace_file(path, ["app.init"], chunk_bytes=7)
                self.assertEqual(report["writer_clock_domain"], expected)
                self.assertEqual(report["capture"], doc["jceTrace"])
        del doc["jceTrace"]["writer_diagnostics"]
        self.assertEqual(analyze_trace(doc, ["app.init"])["writer_clock_domain"], "unknown")

    def test_clock_domain_rejects_unrecognized_or_mistyped_present_values(self):
        for domain in (None, False, 0, 1.0, [], {}, "unknown", "sdl-ticks-ns", ""):
            doc = self.document()
            doc["jceTrace"]["writer_diagnostics"]["clock_domain"] = domain
            with self.subTest(domain=domain), self.assertRaisesRegex(ValueError, "writer diagnostic clock"):
                analyze_trace(doc, ["app.init"])
            with tempfile.TemporaryDirectory(prefix="jce-writer-clock-invalid-") as folder:
                path = Path(folder) / "trace.json"
                path.write_text(json.dumps(doc), encoding="utf-8")
                with self.assertRaisesRegex(TraceFileError, "writer diagnostic clock"):
                    analyze_trace_file(path, ["app.init"], chunk_bytes=7)

    def test_ordinary_omitted_domain_gate_fails_same_invalid_clock_assertion(self):
        source = Path(reporter.__file__).read_text(encoding="utf-8")
        wire = ('    if "clock_domain" in diag and diag["clock_domain"] != "process-perf-ns":\n'
                '        raise ValueError("unknown writer diagnostic clock domain")\n')
        self.assertEqual(source.count(wire), 1)
        namespace = {"__name__": "tools.lifecycle_old_writer_clock_gate", "__package__": "tools", "__file__": reporter.__file__}
        exec(compile(source.replace(wire, "    # Old reader accepted any writer clock tag.\n", 1),
                     reporter.__file__, "exec"), namespace)
        doc = self.document()
        doc["jceTrace"]["writer_diagnostics"]["clock_domain"] = "sdl-ticks-ns"
        with self.assertRaisesRegex(ValueError, "writer diagnostic clock"):
            analyze_trace(doc, ["app.init"])
        with self.assertRaises(AssertionError):
            with self.assertRaises(ValueError):
                namespace["analyze_trace"](doc, ["app.init"])


class WaitRequirementTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(prefix="jce-lifecycle-wait-")
        self.addCleanup(self.folder.cleanup)
        self.path = Path(self.folder.name) / "capture.jsonl"

    def analyze_file(self, doc, *, required=("app.init",), require_wait=("trace.capture.join",)):
        self.path.write_text(json.dumps(doc, allow_nan=False), encoding="utf-8")
        return analyze_trace_file(self.path, required, require_wait=require_wait, chunk_bytes=7)

    def test_completed_wait_is_required_independently_of_successful_service(self):
        doc = wait_capture()
        more = copy.deepcopy(doc["traceEvents"][2:4])
        for event in more:
            event["ts"] += 200
            event["args"].update(id=10, parent=0)
        doc["traceEvents"][4:4] = more
        doc["jceTrace"].update(recorded=7, captured=7)
        for result in (analyze_trace(doc, ["app.init"], require_wait=["trace.capture.join"]),
                       self.analyze_file(doc)):
            self.assertEqual(result["required"], ["app.init"])
            self.assertEqual(result["required_wait"], ["trace.capture.join"])
            row = result["required_wait_aggregates"][0]
            self.assertEqual(row["category"], "wait")
            self.assertEqual(row["observed_count"], 2)
            self.assertIn("not GPU completion", row["scope"])

    def test_missing_wait_and_complete_wrong_category_cannot_satisfy_requirement(self):
        variants = {"missing": capture(), "same-name-service": capture(), "other-category": wait_capture()}
        for event in variants["same-name-service"]["traceEvents"]:
            event["name"] = "trace.capture.join"
        for event in variants["other-category"]["traceEvents"][2:4]:
            event["cat"] = "thread"
        for name, doc in variants.items():
            required = ["trace.capture.join"] if name == "same-name-service" else ["app.init"]
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "required wait missing"):
                self.analyze_file(doc, required=required)

    def test_unfinished_requested_or_unrequested_wait_rejects_whole_capture(self):
        for name in ("trace.capture.join", "other.wait"):
            doc = wait_capture()
            doc["traceEvents"][2]["name"] = name
            del doc["traceEvents"][3]
            doc["jceTrace"].update(recorded=4, captured=4)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "unfinished scopes"):
                self.analyze_file(doc)

    def test_malformed_wait_and_pair_category_mismatch_reject_whole_capture(self):
        for mutation in ("id", "owner", "thread", "category", "reversed", "duration", "name"):
            doc = wait_capture()
            ending = doc["traceEvents"][3]
            if mutation == "id": ending["args"]["id"] = True
            elif mutation == "owner": ending["args"]["parent"] = 99
            elif mutation == "thread": ending["tid"] = 2
            elif mutation == "category": ending["cat"] = "core"
            elif mutation == "reversed": ending["ts"] = 1000
            elif mutation == "duration": ending["args"]["ns"] = .5
            else: ending["name"] = "another.wait"
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.analyze_file(doc)

    def test_complete_wait_cannot_replace_successful_task_requirement(self):
        for state in (1, 2):
            doc = wait_capture()
            doc["traceEvents"][-1]["args"]["state"] = state
            with self.subTest(state=state), self.assertRaisesRegex(ValueError, "required phase missing or unsuccessful"):
                self.analyze_file(doc)
        with self.assertRaisesRegex(ValueError, "required phase missing or unsuccessful"):
            self.analyze_file(wait_capture(), required=["trace.capture.join"])

    def test_same_name_service_and_wait_each_require_their_own_category(self):
        doc = wait_capture()
        for event in doc["traceEvents"][2:4]:
            event["name"] = "app.init"
        result = self.analyze_file(doc, require_wait=["app.init"])
        self.assertEqual(result["required_phase_aggregates"][0]["observed_count"], 1)
        self.assertEqual(result["required_wait_aggregates"][0]["observed_count"], 1)
        doc["traceEvents"][-1]["args"]["state"] = 1
        with self.assertRaisesRegex(ValueError, "required phase missing or unsuccessful"):
            self.analyze_file(doc, require_wait=["app.init"])

    def test_satisfied_wait_does_not_skip_unselected_malformed_events_or_json_tail(self):
        for mutation in ("unpaired-tail", "invalid-json-tail"):
            doc = wait_capture()
            if mutation == "unpaired-tail":
                ending = copy.deepcopy(doc["traceEvents"][3])
                ending.update(name="other.wait", ts=3000)
                ending["args"]["id"] = 10
                doc["traceEvents"].append(ending)
                doc["jceTrace"].update(recorded=6, captured=6)
            raw = json.dumps(doc, allow_nan=False).encode("utf-8")
            if mutation == "invalid-json-tail": raw += b"INVALID_TAIL"
            self.path.write_bytes(raw)
            with self.subTest(mutation=mutation), self.assertRaises(TraceFileError) as failed:
                analyze_trace_file(self.path, ["app.init"], require_wait=["trace.capture.join"], chunk_bytes=7)
            self.assertEqual(failed.exception.evidence["sha256"], hashlib.sha256(raw).hexdigest())
            self.assertTrue(failed.exception.evidence["source_fingerprint_stable"])

    def test_wait_requirement_names_and_combined_count_are_bounded(self):
        for names in ("trace.capture.join", [""], [True], ["trace.capture.join", "trace.capture.join"]):
            with self.subTest(names=names), self.assertRaises(ValueError):
                self.analyze_file(wait_capture(), require_wait=names)
        with self.assertRaisesRegex(ValueError, "explicit requirement limit"):
            self.analyze_file(wait_capture(), require_wait=["wait." + str(index) for index in range(128)])

    def test_cli_accepts_task_and_wait_requirements_and_rejects_missing_wait(self):
        doc = wait_capture()
        additional = copy.deepcopy(doc["traceEvents"][2:4])
        for event in additional:
            event["name"] = "worker.join"
            event["ts"] += 200
            event["args"]["id"] = 10
        doc["traceEvents"][4:4] = additional
        doc["jceTrace"].update(recorded=7, captured=7)
        self.path.write_text(json.dumps(doc), encoding="utf-8")
        out = self.path.with_name("result.json")
        arguments = ["lifecycle_trace_report.py", str(self.path), "--require", "app.init",
                     "--require-wait", "trace.capture.join", "--require-wait", "worker.join", "--out", str(out)]
        with mock.patch.object(sys, "argv", arguments), redirect_stdout(io.StringIO()):
            try:
                code = reporter.main()
            except SystemExit as error:
                code = error.code
        self.assertEqual(code, 0)
        result = json.loads(out.read_text(encoding="utf-8"))
        self.assertTrue(result["validation_complete"])
        self.assertEqual(result["events_validated"], 7)
        self.assertEqual(result["required_wait"], ["trace.capture.join", "worker.join"])
        self.assertEqual([row["observed_count"] for row in result["required_wait_aggregates"]], [1, 1])
        self.path.write_text(json.dumps(capture()), encoding="utf-8")
        with mock.patch.object(sys, "argv", arguments), redirect_stdout(io.StringIO()):
            self.assertEqual(reporter.main(), 1)
        failed = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(failed["status"], "INVALID_OR_INCOMPLETE_CAPTURE")
        self.assertIn("required wait missing", failed["error"])
        self.assertTrue(failed["json_utf8_eof_validated"])
        self.assertTrue(failed["source_fingerprint_stable"])


class SequenceExhaustionTests(unittest.TestCase):
    def documents(self):
        for mode in ("resident", "threaded", "cooperative"):
            doc = capture()
            if mode != "resident":
                doc["jceTrace"].update(mode="continuous", consumer_mode=mode,
                    worker_available=mode == "threaded", write_ok=True,
                    capture_complete=True, unexported=0, active_threads=0,
                    active_work_items=0, active_waits=0)
            yield mode, doc

    def test_legacy_absence_and_explicit_valid_sequence_remain_distinct(self):
        for mode, doc in self.documents():
            for explicit in (False, True):
                with self.subTest(mode=mode, explicit=explicit):
                    if explicit:
                        doc["jceTrace"]["sequence_exhausted"] = False
                    result = analyze_trace(doc, ["app.init"])
                    self.assertEqual("sequence_exhausted" in result["capture"], explicit)
                    if explicit:
                        self.assertIs(result["capture"]["sequence_exhausted"], False)
                    with tempfile.TemporaryDirectory(prefix="jce-sequence-valid-") as folder:
                        path = Path(folder) / "trace.json"
                        path.write_text(json.dumps(doc), encoding="utf-8")
                        streamed = analyze_trace_file(path, ["app.init"], chunk_bytes=7)
                        self.assertEqual(streamed["capture"], result["capture"])

    def test_omitted_exhaustion_gate_fails_the_same_rejection_assertion(self):
        source = Path(reporter.__file__).read_text(encoding="utf-8")
        wire = ('    if "sequence_exhausted" in metadata and metadata["sequence_exhausted"] is not False:\n'
                '        raise ValueError("capture reports sequence exhaustion or an invalid exhaustion state")\n')
        self.assertEqual(source.count(wire), 1)
        namespace = {"__name__": "jce_lifecycle_old_sequence_gate", "__file__": reporter.__file__}
        exec(compile(source.replace(wire, "", 1), reporter.__file__, "exec"), namespace)
        for mode, doc in self.documents():
            doc["jceTrace"]["sequence_exhausted"] = True
            with self.subTest(mode=mode):
                old = namespace["analyze_trace"](doc, ["app.init"])
                self.assertEqual(old["status"], "REQUIRED_CPU_LIFECYCLE_SCOPES_OBSERVED")
                with self.assertRaises(AssertionError):
                    with self.assertRaisesRegex(ValueError, "sequence exhaustion"):
                        namespace["analyze_trace"](doc, ["app.init"])
                with tempfile.TemporaryDirectory(prefix="jce-sequence-old-") as folder:
                    path = Path(folder) / "trace.json"
                    path.write_text(json.dumps(doc), encoding="utf-8")
                    old_stream = namespace["analyze_trace_file"](path, ["app.init"], chunk_bytes=7)
                    self.assertTrue(old_stream["validation_complete"])
                    with self.assertRaises(AssertionError):
                        with self.assertRaisesRegex(ValueError, "sequence exhaustion"):
                            namespace["analyze_trace_file"](path, ["app.init"], chunk_bytes=7)

    def test_balanced_complete_capture_rejects_exhausted_or_mistyped_sequence(self):
        for mode, doc in self.documents():
            for value in (True, 0, 1, None, "false", [], {}):
                broken = copy.deepcopy(doc)
                broken["jceTrace"]["sequence_exhausted"] = value
                with self.subTest(mode=mode, value=value):
                    with self.assertRaisesRegex(ValueError, "sequence exhaustion"):
                        analyze_trace(broken, ["app.init"])
                    with tempfile.TemporaryDirectory(prefix="jce-sequence-invalid-") as folder:
                        path = Path(folder) / "trace.json"
                        path.write_text(json.dumps(broken), encoding="utf-8")
                        with self.assertRaisesRegex(TraceFileError, "sequence exhaustion"):
                            analyze_trace_file(path, ["app.init"], chunk_bytes=7)


class LifecycleTraceTests(unittest.TestCase):
    def test_single_cooperative_zero_thread_identity_is_named_and_preserved(self):
        doc = capture()
        for event in doc["traceEvents"]:
            event["tid"] = 0
        doc["traceEvents"].insert(0, {"ph": "M", "pid": 1, "tid": 0,
            "name": "thread_name", "args": {"name": "MAIN"}})
        doc["jceTrace"].update(mode="continuous", consumer_mode="cooperative",
            worker_available=False, write_ok=True, capture_complete=True,
            unexported=0, active_threads=0, active_work_items=0, active_waits=0,
            recorded=4, captured=4)
        result = analyze_trace(doc, ["app.init"])
        self.assertEqual(result["scopes"][0]["thread"], 0)
        self.assertEqual(result["thread_identity_scope"], "named_single_cooperative_thread")
        for mutation in ("threaded", "resident", "mixed", "unnamed", "ambiguous", "negative", "boolean", "pid"):
            bad = copy.deepcopy(doc)
            if mutation == "threaded":
                bad["jceTrace"].update(consumer_mode="threaded", worker_available=True)
            elif mutation == "resident":
                bad["jceTrace"]["mode"] = "resident"
            elif mutation == "mixed":
                bad["traceEvents"][1]["tid"] = 1
            elif mutation == "unnamed":
                bad["traceEvents"].pop(0)
                bad["jceTrace"].update(recorded=3, captured=3)
            elif mutation == "ambiguous":
                other = copy.deepcopy(bad["traceEvents"][0])
                other["args"]["name"] = "different-thread"
                bad["traceEvents"].append(other)
                bad["jceTrace"].update(recorded=5, captured=5)
            elif mutation == "negative":
                bad["traceEvents"][1]["tid"] = -1
            elif mutation == "boolean":
                bad["traceEvents"][1]["tid"] = False
            else:
                bad["traceEvents"][1]["pid"] = 0
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                analyze_trace(bad, ["app.init"])

    def test_completed_scope_has_measured_clock_and_explicit_limit(self):
        result = analyze_trace(capture(), ["app.init"])
        self.assertEqual(result["scopes"][0]["measured_ms"], .999)
        self.assertEqual(result["scopes"][0]["elapsed_ms"], 1)
        self.assertIn("not GPU", result["scope"])

    def test_required_service_timing_keeps_both_clocks_and_exact_integer_nanoseconds(self):
        doc = capture()
        measured = 9007199254740993
        doc["traceEvents"][-1]["args"]["ns"] = measured
        more = copy.deepcopy(doc["traceEvents"])
        for event in more:
            event["ts"] += 3000
            if "task" in event["args"]: event["args"]["task"] = 2
            if "id" in event["args"]: event["args"]["id"] = 2
            if "parent" in event["args"] and event["args"]["parent"]: event["args"]["parent"] = 2
        more[-1]["ts"] += 1000
        more[-1]["args"]["ns"] = measured + 1
        doc["traceEvents"] += more
        doc["jceTrace"].update(recorded=6, captured=6)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "exact-service-timing.json"
            path.write_text(json.dumps(doc), encoding="utf-8")
            row = analyze_trace_file(path, ["app.init"])["required_phase_aggregates"][0]
        self.assertEqual(row["observed_count"], 2)
        self.assertEqual((row["elapsed_ms_sum"], row["elapsed_ms_min"], row["elapsed_ms_max"]), (3, 1, 2))
        self.assertEqual(row["measured_ns_sum"], measured * 2 + 1)
        self.assertEqual((row["measured_ns_min"], row["measured_ns_max"]), (measured, measured + 1))
        self.assertIn("nested services overlap", row["timing_scope"])

    def test_nested_required_service_is_inclusive_and_keeps_one_owner_per_pair(self):
        doc = capture()
        outer = doc["traceEvents"]
        inner = copy.deepcopy(outer)
        inner[0]["args"].update(task=2, parent=1)
        inner[0]["ts"] = 1500
        inner[1]["args"].update(id=2, parent=2)
        inner[1]["ts"] = 2000
        inner[2]["args"].update(id=2, parent=2, ns=3000000)
        inner[2]["ts"] = 5000
        outer[2]["ts"] = 6000
        outer[2]["args"]["ns"] = 5000000
        doc["traceEvents"] = outer[:2] + inner + outer[2:]
        doc["jceTrace"].update(recorded=6, captured=6)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "nested-service-timing.json"
            path.write_text(json.dumps(doc), encoding="utf-8")
            result = analyze_trace_file(path, ["app.init"])
        row = result["required_phase_aggregates"][0]
        self.assertEqual(result["completed_scope_count"], 2)
        self.assertEqual(row["observed_count"], 2)
        self.assertEqual((row["elapsed_ms_sum"], row["elapsed_ms_min"], row["elapsed_ms_max"]), (8, 3, 5))
        self.assertEqual(row["measured_ns_sum"], 8000000)
        self.assertIn("nested services overlap", row["timing_scope"])

    def test_finite_intervals_cannot_overflow_required_elapsed_total(self):
        doc = capture()
        template = doc["traceEvents"]
        doc["traceEvents"] = []
        for identity in range(1, 1101):
            events = copy.deepcopy(template)
            for event in events:
                event["tid"] = identity  # Huge overlapping intervals need distinct producer clocks.
            events[0]["args"]["task"] = identity
            events[1]["args"].update(id=identity, parent=identity)
            events[2]["args"].update(id=identity, parent=identity)
            events[2]["ts"] = 1.7e308
            doc["traceEvents"] += events
        doc["jceTrace"].update(recorded=3300, captured=3300)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "finite-aggregate-overflow.json"
            path.write_text(json.dumps(doc), encoding="utf-8")
            with self.assertRaisesRegex(TraceFileError, "elapsed aggregate is not finite"):
                analyze_trace_file(path, ["app.init"])

    def test_continuous_capture_requires_clean_footer_and_consumer_identity(self):
        doc = capture()
        doc["jceTrace"].update(mode="continuous", consumer_mode="threaded",
            worker_available=True, write_ok=True, capture_complete=True,
            unexported=0, active_threads=0, active_work_items=0, active_waits=0)
        self.assertEqual(analyze_trace(doc, ["app.init"])["capture"]["mode"], "continuous")
        cooperative = copy.deepcopy(doc)
        cooperative["jceTrace"].update(consumer_mode="cooperative", worker_available=False)
        analyze_trace(cooperative, ["app.init"])
        for key, value in (("write_ok", False), ("capture_complete", False),
                           ("unexported", 1), ("active_threads", 1),
                           ("active_work_items", 1), ("active_waits", 1),
                           ("worker_available", False), ("consumer_mode", "unknown"),
                           ("mode", "invented")):
            broken = copy.deepcopy(doc)
            broken["jceTrace"][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                analyze_trace(broken, ["app.init"])
        for key in ("write_ok", "unexported", "capture_complete"):
            broken = copy.deepcopy(doc); del broken["jceTrace"][key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                analyze_trace(broken, ["app.init"])

    def test_nonrequired_failed_attempts_remain_diagnostic(self):
        doc = capture()
        for offset, state, name in ((1, 1, "optional.missing-file"), (2, 2, "optional.null-cleanup")):
            events = copy.deepcopy(capture()["traceEvents"])
            for event in events:
                event["name"] = name
                event["ts"] += offset * 3000
                for key in ("id", "task", "parent"):
                    if event["args"].get(key): event["args"][key] += offset
            events[-1]["args"]["state"] = state
            doc["traceEvents"].extend(events)
        doc["jceTrace"].update(recorded=9, captured=9)
        result = analyze_trace(doc, ["app.init"])
        self.assertEqual(result["nonrequired_task_outcomes"]["failed"], 1)
        self.assertEqual(result["nonrequired_task_outcomes"]["cancelled"], 1)
        self.assertEqual(len(result["scopes"]), 3)
        with self.assertRaises(ValueError): analyze_trace(doc, ["optional.missing-file"])
        with self.assertRaises(ValueError): analyze_trace(doc, ["optional.null-cleanup"])
        for flag in ("write_ok", "capture_complete"):
            bad = capture(); bad["jceTrace"][flag] = False
            with self.subTest(flag=flag), self.assertRaises(ValueError):
                analyze_trace(bad, ["app.init"])

    def test_ring_loss_and_reset_are_rejected(self):
        for changes in ({"recorded": 4, "overwritten_before_capture": 1, "lost": 1},
                        {"capture_generation_stable": False}, {"lost": 1}):
            doc = capture(); doc["jceTrace"].update(changes)
            with self.assertRaises(ValueError): analyze_trace(doc, ["app.init"])

    def test_missing_failed_cancelled_scope_cannot_pass(self):
        for state in (1, 2, None, True):
            doc = capture(); doc["traceEvents"][2]["args"]["state"] = state
            with self.assertRaises(ValueError): analyze_trace(doc, ["app.init"])
        with self.assertRaises(ValueError): analyze_trace(capture(), ["not.measured"])

    def test_unpaired_reversed_wrong_identity_and_nonfinite_are_rejected(self):
        variants = [capture() for _ in range(5)]
        variants[0]["traceEvents"].pop()
        variants[1]["traceEvents"][2]["ts"] = 0
        variants[2]["traceEvents"][2]["args"]["id"] = 2
        variants[3]["traceEvents"][2]["ts"] = float("nan")
        variants[4]["traceEvents"][2]["args"]["ns"] = -1
        for doc in variants:
            with self.assertRaises(ValueError): analyze_trace(doc, ["app.init"])

    def test_empty_or_unknown_capture_and_requirements_are_rejected(self):
        for doc in ({}, [], {"traceEvents": []}):
            with self.assertRaises(ValueError): analyze_trace(doc, ["app.init"])
        for required in ([], ["app.init", "app.init"]):
            with self.assertRaises(ValueError): analyze_trace(capture(), required)

    def test_same_name_scopes_on_different_threads_keep_identity(self):
        doc = capture(); other = copy.deepcopy(doc["traceEvents"])
        for event in other:
            event["tid"] = 2
            for name in ("id", "task", "parent"):
                if name in event["args"] and event["args"][name]: event["args"][name] += 1
        doc["traceEvents"].extend(other)
        doc["jceTrace"].update(recorded=6, captured=6)
        self.assertEqual(len(analyze_trace(doc, ["app.init"])["scopes"]), 2)

    def test_forged_count_thread_phase_owner_category_and_kind_are_rejected(self):
        variants = [capture() for _ in range(7)]
        variants[0]["jceTrace"].update(recorded=999, captured=999)
        for event in variants[1]["traceEvents"]: event.pop("tid")
        variants[2]["traceEvents"].append({"ph": "INVALID"})
        variants[2]["jceTrace"].update(recorded=4, captured=4)
        variants[3]["traceEvents"][2]["args"]["parent"] = 999
        for event in variants[4]["traceEvents"]: event["cat"] = "fake"
        variants[5]["traceEvents"][0]["args"]["kind"] = 0
        variants[6]["traceEvents"][0]["name"] = "another.service"
        for doc in variants:
            with self.assertRaises(ValueError): analyze_trace(doc, ["app.init"])

    def test_late_submission_fractional_duration_and_replayed_service_fail(self):
        variants = [capture() for _ in range(4)]
        variants[0]["traceEvents"][0], variants[0]["traceEvents"][1] = variants[0]["traceEvents"][1], variants[0]["traceEvents"][0]
        variants[1]["traceEvents"][0]["ts"] = 3000
        variants[2]["traceEvents"][2]["args"]["ns"] = .5
        variants[3]["traceEvents"].extend(copy.deepcopy(variants[3]["traceEvents"][1:]))
        variants[3]["jceTrace"].update(recorded=5, captured=5)
        for doc in variants:
            with self.assertRaises(ValueError): analyze_trace(doc, ["app.init"])


    def test_wait_owned_by_service_keeps_distinct_label_and_task_parent(self):
        doc = capture()
        wait_begin = {"ph": "B", "cat": "wait", "pid": 1, "tid": 1,
                      "ts": 1100, "name": "worker.join",
                      "args": {"id": 9, "parent": 1, "ns": 0}}
        wait_end = copy.deepcopy(wait_begin)
        wait_end.update(ph="E", ts=1200)
        wait_end["args"]["ns"] = 100000
        doc["traceEvents"][2:2] = [wait_begin, wait_end]
        doc["jceTrace"].update(recorded=5, captured=5)
        result = analyze_trace(doc, ["app.init"])
        waiting = next(scope for scope in result["scopes"] if scope["name"] == "worker.join")
        self.assertEqual(waiting["owner"], 1)
        service = next(scope for scope in result["scopes"] if scope["name"] == "app.init")
        self.assertEqual(service["submitted_parent"], 0)
        broken = copy.deepcopy(doc)
        broken["traceEvents"][3]["args"]["parent"] = 2
        with self.assertRaises(ValueError):
            analyze_trace(broken, ["app.init"])


def counter_capture(values):
    doc = capture()
    for index, value in enumerate(values):
        doc["traceEvents"].append({"ph": "C", "cat": "render", "pid": 1,
            "tid": 1, "ts": 3000 + index, "name": "budget.failures", "args": {"value": value}})
    doc["jceTrace"].update(recorded=len(doc["traceEvents"]), captured=len(doc["traceEvents"]))
    return doc


class CounterControlTests(unittest.TestCase):
    def test_all_zero_records_actual_indices_and_values(self):
        result = analyze_trace(counter_capture([0, 0.0]), ["app.init"],
                               require_counter_zero=["budget.failures"])
        row = result["counter_controls"]["all_zero"][0]
        self.assertEqual(row["observed_count"], 2)
        self.assertEqual(row["raw_indices"], [3, 4])
        self.assertEqual(row["last_value"], 0)

    def test_all_zero_rejects_nonzero_earlier_even_when_last_zero(self):
        with self.assertRaises(ValueError):
            analyze_trace(counter_capture([1, 0]), ["app.init"],
                          require_counter_zero=["budget.failures"])

    def test_last_zero_uses_stream_order_not_timestamp_sort(self):
        doc = counter_capture([1, 0])
        doc["traceEvents"][-1]["ts"] = 100
        doc["traceEvents"][-1]["tid"] = 2
        row = analyze_trace(doc, ["app.init"],
            require_counter_last_zero=["budget.failures"])["counter_controls"]["last_zero"][0]
        self.assertEqual(row["last_raw_index"], 4)
        with self.assertRaises(ValueError):
            analyze_trace(counter_capture([0, 1]), ["app.init"],
                          require_counter_last_zero=["budget.failures"])

    def test_missing_counter_value_and_non_counter_name_reject(self):
        for doc in (capture(), counter_capture([0])):
            if len(doc["traceEvents"]) > 3:
                del doc["traceEvents"][-1]["args"]["value"]
            with self.assertRaises(ValueError):
                analyze_trace(doc, ["app.init"], require_counter_zero=["budget.failures"])
        with self.assertRaises(ValueError):
            analyze_trace(capture(), ["app.init"], require_counter_last_zero=["app.init"])

    def test_invalid_counter_types_reject_only_when_requested(self):
        for value in (True, False, "0", None, float("nan"), float("inf"), -float("inf")):
            doc = counter_capture([value])
            analyze_trace(doc, ["app.init"])
            with self.subTest(value=value), self.assertRaises(ValueError):
                analyze_trace(doc, ["app.init"], require_counter_zero=["budget.failures"])

    def test_requested_names_must_be_unique_nonempty_strings(self):
        for names in ([""], [True], ["budget.failures", "budget.failures"]):
            with self.assertRaises(ValueError):
                analyze_trace(counter_capture([0]), ["app.init"], require_counter_zero=names)



class StreamingTraceTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(prefix="jce-lifecycle-stream-")
        self.addCleanup(self.folder.cleanup)
        self.path = Path(self.folder.name) / "capture.json"

    def run_capture(self, doc=None, *, raw=None, **kwargs):
        if raw is None:
            raw = json.dumps(doc if doc is not None else capture(), ensure_ascii=False, allow_nan=False).encode("utf-8")
        self.path.write_bytes(raw)
        return analyze_trace_file(self.path, ["app.init"], **kwargs)

    @staticmethod
    def repeated(count):
        doc = capture()
        events = []
        for identity in range(1, count + 1):
            batch = copy.deepcopy(doc["traceEvents"])
            for event in batch:
                event["ts"] += identity * 3000
                for field in ("id", "task", "parent"):
                    if event["args"].get(field): event["args"][field] = identity
            events.extend(batch)
        doc["traceEvents"] = events
        doc["jceTrace"].update(recorded=len(events), captured=len(events))
        return doc

    @staticmethod
    def diagnostic_capture(outcomes):
        doc = capture()
        for identity, (name, state, elapsed_us, measured_ns) in enumerate(outcomes, 2):
            events = copy.deepcopy(capture()["traceEvents"])
            for event in events:
                event.update(name=name, tid=1 + identity % 3)
            events[0].update(ts=identity * 3000)
            events[0]["args"]["task"] = identity
            events[1].update(ts=identity * 3000 + 100)
            events[1]["args"].update(id=identity, parent=identity)
            events[2].update(ts=events[1]["ts"] + elapsed_us)
            events[2]["args"].update(id=identity, parent=identity, ns=measured_ns, state=state)
            doc["traceEvents"].extend(events)
        count = len(doc["traceEvents"])
        doc["jceTrace"].update(recorded=count, captured=count)
        return doc

    def assert_diagnostic_counts(self, result, expected):
        rows = result["nonrequired_task_outcomes"].get("aggregates", [])
        self.assertEqual({(row["name"], row["state"]): row["observed_count"] for row in rows}, expected)

    def test_chunk_boundaries_unicode_numbers_and_metadata_orders(self):
        doc = capture()
        raw = json.dumps(doc, ensure_ascii=False, allow_nan=False).encode("utf-8")
        raw = b'{"extra":-1.25e+19,"unicode":"' + "字面😀".encode("utf-8") + b'",' + raw[1:]
        for chunk in (1, 2, 3, 5, 7, 13, 64, 65536):
            with self.subTest(chunk=chunk):
                result = self.run_capture(raw=raw + b" \r\n\t", chunk_bytes=chunk)
                self.assertTrue(result["json_utf8_eof_validated"])
                self.assertTrue(result["source_fingerprint_stable"])
                self.assertEqual(result["sha256"], hashlib.sha256(raw + b" \r\n\t").hexdigest())
                self.assertEqual(result["sha256"], result["source_sha256_after"])
                self.assertEqual(result["events_validated"], 3)
        backwards = {"traceEvents": doc["traceEvents"], "jceTrace": doc["jceTrace"]}
        self.assertEqual(self.run_capture(backwards, chunk_bytes=7)["completed_scope_count"], 1)

    def test_complete_nonselected_pairing_and_raw_counter_order(self):
        doc = counter_capture([0] * 4000)
        result = self.run_capture(doc, require_counter_zero=["budget.failures"])
        row = result["counter_controls"]["all_zero"][0]
        self.assertEqual(row["observed_count"], 4000)
        self.assertEqual(row["raw_indices_samples"], [3, 4002])
        self.assertEqual(row["values_samples"], [0, 0])
        self.assertFalse(row["samples_complete"])
        self.assertEqual(result["events_validated"], 4003)
        self.assertLess(len(json.dumps(result)), 7000)
        for mutation in ("early-counter", "unpaired-other", "invalid-unrequested", "late-replay"):
            bad = copy.deepcopy(doc)
            if mutation == "early-counter": bad["traceEvents"][9]["args"]["value"] = 1
            elif mutation == "unpaired-other":
                event = copy.deepcopy(bad["traceEvents"][1])
                event.update(name="unselected.wait", cat="wait")
                event["args"].update(id=99)
                bad["traceEvents"].append(event)
            elif mutation == "invalid-unrequested":
                bad["traceEvents"][-1].update(name="other.counter")
                bad["traceEvents"][-1]["args"]["value"] = "0"
            else: bad["traceEvents"].extend(copy.deepcopy(bad["traceEvents"][1:3]))
            count = len(bad["traceEvents"])
            bad["jceTrace"].update(recorded=count, captured=count)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.run_capture(bad, require_counter_zero=["budget.failures"])
        ordered = counter_capture([1, 0])
        ordered["traceEvents"][-1]["ts"] = 0
        ordered["traceEvents"][-1]["tid"] = 2
        row = self.run_capture(ordered, require_counter_last_zero=["budget.failures"])["counter_controls"]["last_zero"][0]
        self.assertEqual(row["last_raw_index"], 4)

    def test_aggregate_retains_every_required_outcome_without_scope_list(self):
        doc = self.repeated(300)
        result = self.run_capture(doc)
        self.assertEqual(result["historical_task_count"], 300)
        self.assertEqual(result["completed_scope_count"], 300)
        row = result["required_phase_aggregates"][0]
        self.assertEqual(row["observed_count"], 300)
        self.assertEqual(row["elapsed_ms_sum"], 300)
        self.assertEqual((row["elapsed_ms_min"], row["elapsed_ms_max"]), (1, 1))
        self.assertEqual(row["measured_ns_sum"], 300 * 999000)
        self.assertEqual(result["scope_aggregates"][0]["first"]["id"], 1)
        self.assertEqual(result["scope_aggregates"][0]["last"]["id"], 300)
        self.assertNotIn("scopes", result)
        self.assertLess(len(json.dumps(result)), 6000)
        doc["traceEvents"][-1]["args"]["state"] = 1
        with self.assertRaisesRegex(ValueError, "required phase missing or unsuccessful"):
            self.run_capture(doc)

    def test_diagnostic_aggregates_retain_every_service_state_and_late_witness(self):
        outcomes = [("fs.host.read_all", 1, 1500, 1499000),
                    ("resource.texture", 1, 2000, 1999000),
                    ("fs.host.read_all", 2, 2500, 2499000),
                    ("resource.texture", 2, 750, 749000),
                    ("resource.optional", 1, 1250, 1249000)]
        outcomes += [("fs.host.read_all", 1, 1000, 999000)] * 400
        outcomes += [("fs.host.read_all", 1, 3500, 3499000)]
        doc = self.diagnostic_capture(outcomes)
        # Export order, not cross-thread timestamp sorting, owns first/last.
        for event, timestamp in zip(doc["traceEvents"][-3:], (100, 200, 3700)):
            event["ts"] = timestamp
            event["tid"] = 99
        result = self.run_capture(doc, chunk_bytes=7)
        self.assert_diagnostic_counts(result, {("fs.host.read_all", 1): 402,
            ("fs.host.read_all", 2): 1, ("resource.texture", 1): 1,
            ("resource.texture", 2): 1, ("resource.optional", 1): 1})
        rows = result["nonrequired_task_outcomes"]["aggregates"]
        row = next(row for row in rows if (row["name"], row["state"]) == ("fs.host.read_all", 1))
        self.assertEqual(row["outcome"], "failed")
        self.assertEqual(row["elapsed_ms_sum"], 405)
        self.assertEqual((row["elapsed_ms_min"], row["elapsed_ms_max"]), (1, 3.5))
        self.assertEqual(row["measured_ns_sum"], 404598000)
        self.assertEqual((row["measured_ns_min"], row["measured_ns_max"]), (999000, 3499000))
        self.assertEqual((row["first"]["id"], row["last"]["id"]), (2, 407))
        self.assertEqual((row["first"]["end_raw_index"], row["last"]["end_raw_index"]), (5, 1220))
        self.assertEqual((row["first"]["end_timestamp_us"], row["last"]["end_timestamp_us"]), (7600, 3700))
        self.assertEqual(row["last"]["measured_ns"], 3499000)
        self.assertEqual(result["nonrequired_task_outcomes"]["failed"], 404)
        self.assertEqual(result["nonrequired_task_outcomes"]["cancelled"], 2)
        self.assertEqual(sum(row["observed_count"] for row in rows), 406)
        self.assertEqual(len(result["diagnostic_samples"]), 2)
        self.assertEqual(result["events_validated"], 1221)
        self.assertEqual(result["historical_task_count"], 407)
        self.assertTrue(result["validation_complete"])
        self.assertTrue(result["json_utf8_eof_validated"])
        self.assertLess(len(json.dumps(result, allow_nan=False)), 15000)
        legacy = analyze_trace(doc, ["app.init"])
        self.assertEqual(len(legacy["scopes"]), 407)
        self.assertEqual(len(legacy["nonrequired_task_outcomes"]["scopes"]), 406)
        self.assertNotIn("aggregates", legacy["nonrequired_task_outcomes"])

    def test_diagnostic_states_share_existing_name_and_history_admission(self):
        doc = self.diagnostic_capture([("optional.a", 1, 1000, 999000),
            ("optional.a", 2, 1000, 999000), ("optional.b", 1, 1000, 999000)])
        result = self.run_capture(doc, max_names=3, max_history=4)
        self.assert_diagnostic_counts(result, {("optional.a", 1): 1,
            ("optional.a", 2): 1, ("optional.b", 1): 1})
        for limits, message in (({"max_names": 2}, "retained name limit"),
                                ({"max_history": 3}, "historical task identity limit")):
            with self.subTest(limits=limits), self.assertRaisesRegex(TraceFileError, message) as failed:
                self.run_capture(doc, **limits)
            raw = self.path.read_bytes()
            self.assertEqual(failed.exception.evidence["sha256"], hashlib.sha256(raw).hexdigest())
            self.assertEqual(failed.exception.evidence["input_bytes"], len(raw))

    def test_diagnostic_elapsed_overflow_refuses_instead_of_emitting_infinity(self):
        doc = self.diagnostic_capture([("optional.large", 1, 1.7e308, 0)] * 1100)
        # These deliberately enormous overlapping intervals belong to distinct
        # threads; each individual producer clock still moves forwards.
        for index, event in enumerate(doc["traceEvents"][3:]):
            event["tid"] = 10 + index // 3
        with self.assertRaisesRegex(TraceFileError, "diagnostic elapsed aggregate is not finite"):
            self.run_capture(doc)

    def test_diagnostic_aggregation_does_not_relax_required_pair_or_eof_validation(self):
        doc = self.diagnostic_capture([("optional.a", 1, 1000, 999000)])
        for state in (1, 2):
            bad = copy.deepcopy(doc)
            bad["traceEvents"][2]["args"]["state"] = state
            with self.subTest(state=state), self.assertRaisesRegex(TraceFileError, "required phase missing or unsuccessful"):
                self.run_capture(bad)
        bad = copy.deepcopy(doc)
        bad["traceEvents"][-1]["args"]["id"] = 99
        with self.assertRaisesRegex(TraceFileError, "unpaired or reversed scope"):
            self.run_capture(bad)
        raw = json.dumps(doc).encode("utf-8") + b"INVALID_TAIL"
        with self.assertRaises(TraceFileError) as failed:
            self.run_capture(raw=raw)
        self.assertFalse(failed.exception.evidence["json_utf8_eof_validated"])

    def test_actual_cli_publishes_service_outcomes_without_changing_status(self):
        doc = self.diagnostic_capture([("optional.a", 1, 1000, 999000),
            ("optional.b", 2, 2000, 1999000), ("optional.a", 1, 3000, 2999000)])
        self.path.write_text(json.dumps(doc), encoding="utf-8")
        out = self.path.with_name("diagnostics.json")
        arguments = ["lifecycle_trace_report.py", str(self.path), "--require", "app.init", "--out", str(out)]
        with mock.patch.object(sys, "argv", arguments), redirect_stdout(io.StringIO()):
            self.assertEqual(reporter.main(), 0)
        result = json.loads(out.read_text(encoding="utf-8"))
        self.assert_diagnostic_counts(result, {("optional.a", 1): 2, ("optional.b", 2): 1})
        self.assertEqual(result["status"], "REQUIRED_CPU_LIFECYCLE_SCOPES_OBSERVED")
        self.assertEqual(result["sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())

    def test_omitted_diagnostic_wire_fails_same_behavioral_counts_after_execution(self):
        source = Path(reporter.__file__).read_text(encoding="utf-8")
        wire = "                self._record_diagnostic(scope, index, timestamp, duration_ns)\n"
        self.assertEqual(source.count(wire), 1)
        namespace = {"__name__": "jce_lifecycle_old_diagnostic_wire", "__file__": reporter.__file__}
        exec(compile(source.replace(wire, "                # Old owner omitted per-service diagnostic aggregation.\n", 1),
                     reporter.__file__, "exec"), namespace)
        doc = self.diagnostic_capture([("optional.a", 1, 1000, 999000),
            ("optional.b", 2, 1000, 999000), ("optional.a", 1, 1000, 999000)])
        self.path.write_text(json.dumps(doc), encoding="utf-8")
        old = namespace["analyze_trace_file"](self.path, ["app.init"], chunk_bytes=7)
        self.assertTrue(old["validation_complete"])
        self.assertEqual(old["events_validated"], 12)
        self.assertEqual(old["nonrequired_task_outcomes"]["failed"], 2)
        self.assertEqual(old["nonrequired_task_outcomes"]["cancelled"], 1)
        with self.assertRaises(AssertionError):
            self.assert_diagnostic_counts(old, {("optional.a", 1): 2, ("optional.b", 2): 1})

    def test_declared_history_active_name_and_value_limits_fail_closed(self):
        valid = self.repeated(2)
        self.assertEqual(self.run_capture(valid, max_history=2)["historical_task_count"], 2)
        with self.assertRaisesRegex(ValueError, "historical task identity limit"):
            self.run_capture(valid, max_history=1)
        nested = capture()
        wait = copy.deepcopy(nested["traceEvents"][1])
        wait.update(cat="wait", name="worker.join")
        wait["args"]["id"] = 9
        ending = copy.deepcopy(wait)
        ending.update(ph="E", ts=1100)
        ending["args"]["ns"] = 100000
        nested["traceEvents"][2:2] = [wait, ending]
        nested["jceTrace"].update(recorded=5, captured=5)
        self.run_capture(nested, max_active=2, max_names=2)
        for arguments, message in (({"max_active": 1}, "active scope limit"), ({"max_names": 1}, "retained name limit")):
            with self.subTest(arguments=arguments), self.assertRaisesRegex(ValueError, message):
                self.run_capture(nested, **arguments)
        bounded = capture()
        bounded["extra"] = "x" * 254  # quotes included: exactly 256 characters
        self.run_capture(bounded, max_value_chars=256, chunk_bytes=1)
        bounded["extra"] += "x"
        with self.assertRaisesRegex(ValueError, "JSON value size limit"):
            self.run_capture(bounded, max_value_chars=256, chunk_bytes=1)
        for arguments in ({"max_history": 0}, {"max_active": True}, {"chunk_bytes": 1048577}, {"max_value_chars": 1048577}):
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                self.run_capture(**arguments)

    def test_tail_utf8_syntax_duplicate_and_nonfinite_reject_with_raw_identity(self):
        raw = json.dumps(capture(), allow_nan=False).encode("utf-8")
        variants = {
            "truncated": raw[:-2], "trailing-junk": raw + b"false", "utf8": raw + b"\xff",
            "trailing-array-comma": raw.replace(b'"state": 0}}]', b'"state": 0}},]'),
            "duplicate-root": b'{"jceTrace":{},' + raw[1:],
            "escaped-duplicate": raw.replace(b'"pid": 1', b'"pid": 1,"p\\u0069d":1', 1),
            "nonfinite": raw.replace(b'"ts": 900', b'"ts": NaN', 1),
            "exponent-prefix": b'{"extra":1e+,' + raw[1:],
        }
        for name, damaged in variants.items():
            with self.subTest(name=name), self.assertRaises(TraceFileError) as failed:
                self.run_capture(raw=damaged, chunk_bytes=7)
            self.assertEqual(failed.exception.evidence["sha256"], hashlib.sha256(damaged).hexdigest())
            self.assertTrue(failed.exception.evidence["source_fingerprint_stable"])
            self.assertFalse(failed.exception.evidence["json_utf8_eof_validated"])

    def test_complete_syntax_does_not_substitute_for_footer_and_payload_count(self):
        for mutation in ("count", "lost", "generation", "footer-only", "write"):
            doc = capture()
            if mutation == "count": doc["jceTrace"].update(recorded=999, captured=999)
            elif mutation == "lost": doc["jceTrace"].update(recorded=4, overwritten_before_capture=1, lost=1)
            elif mutation == "generation": doc["jceTrace"]["capture_generation_stable"] = False
            elif mutation == "write": doc["jceTrace"]["write_ok"] = False
            else: doc["traceEvents"] = []
            with self.subTest(mutation=mutation), self.assertRaises(TraceFileError) as failed:
                self.run_capture(doc)
            self.assertTrue(failed.exception.evidence["json_utf8_eof_validated"])
            self.assertTrue(failed.exception.evidence["source_fingerprint_stable"])

    def test_mutation_between_full_reads_cannot_pass_same_size_file(self):
        original = reporter._JsonStream.finish_hash
        def change_after_first_read(reader):
            sha = original(reader)
            old = self.path.read_bytes()
            changed = old.replace(b'"ts": 900', b'"ts": 901', 1)
            self.assertNotEqual(old, changed)
            self.assertEqual(len(old), len(changed))
            self.path.write_bytes(changed)
            return sha
        with mock.patch.object(reporter._JsonStream, "finish_hash", change_after_first_read):
            with self.assertRaisesRegex(TraceFileError, "source fingerprint changed") as failed:
                self.run_capture()
        self.assertFalse(failed.exception.evidence["source_fingerprint_stable"])
        self.assertNotEqual(failed.exception.evidence["sha256"], failed.exception.evidence["source_sha256_after"])

    def test_actual_cli_success_and_invalid_tail_keep_distinct_status_and_hash(self):
        self.path.write_text(json.dumps(capture()), encoding="utf-8")
        out = self.path.with_name("result.json")
        arguments = ["lifecycle_trace_report.py", str(self.path), "--require", "app.init", "--out", str(out)]
        with mock.patch.object(sys, "argv", arguments), redirect_stdout(io.StringIO()):
            self.assertEqual(reporter.main(), 0)
        observed = json.loads(out.read_text(encoding="utf-8"))
        self.assertTrue(observed["validation_complete"])
        self.path.write_bytes(self.path.read_bytes() + b"INVALID_TAIL")
        with mock.patch.object(sys, "argv", arguments), redirect_stdout(io.StringIO()):
            self.assertEqual(reporter.main(), 1)
        failed = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(failed["status"], "INVALID_OR_INCOMPLETE_CAPTURE")
        self.assertFalse(failed["validation_complete"])
        self.assertFalse(failed["json_utf8_eof_validated"])
        self.assertEqual(failed["sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())


class ThreadClockTests(unittest.TestCase):
    def both(self, doc, **kwargs):
        memory = analyze_trace(doc, ["app.init"])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "thread-clock.json"
            path.write_text(json.dumps(doc), encoding="utf-8")
            stream = analyze_trace_file(path, ["app.init"], chunk_bytes=7, **kwargs)
        self.assertEqual(memory["status"], stream["status"])
        return stream

    def reject(self, doc, **kwargs):
        with self.assertRaisesRegex(ValueError, "timestamp moved backwards within thread"):
            analyze_trace(doc, ["app.init"])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "rollback.json"
            path.write_text(json.dumps(doc), encoding="utf-8")
            with self.assertRaisesRegex(TraceFileError, "timestamp moved backwards within thread") as failed:
                analyze_trace_file(path, ["app.init"], chunk_bytes=7, **kwargs)
            self.assertEqual(failed.exception.evidence["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())

    def test_same_thread_restart_or_late_counter_cannot_hide_rollback(self):
        doc = capture()
        restarted = copy.deepcopy(doc["traceEvents"])
        for event in restarted:
            event["ts"] -= 800
            for name in ("task", "id", "parent"):
                if event["args"].get(name): event["args"][name] = 2
        doc["traceEvents"].extend(restarted)
        doc["jceTrace"].update(recorded=6, captured=6)
        self.reject(doc)
        doc = counter_capture([0])
        doc["traceEvents"][-1]["ts"] = 100
        self.reject(doc)

    def test_equal_ticks_and_cross_thread_export_disorder_remain_valid(self):
        doc = capture()
        for event in doc["traceEvents"]: event["ts"] = 1000
        self.both(doc)
        doc = counter_capture([0])
        doc["traceEvents"][-1].update(tid=2, ts=100)
        self.both(doc)

    def test_thread_metadata_does_not_reset_producer_clock(self):
        doc = counter_capture([0])
        doc["traceEvents"][-1]["ts"] = 100
        doc["traceEvents"].insert(3, dict(ph="M", pid=1, tid=1, name="thread_name", args=dict(name="restarted-main")))
        doc["jceTrace"].update(recorded=5, captured=5)
        self.reject(doc)

    def test_thread_clock_history_is_bounded_even_without_task_submissions(self):
        doc = counter_capture([0])
        doc["traceEvents"][-1]["tid"] = 2
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "thread-limit.json"
            path.write_text(json.dumps(doc), encoding="utf-8")
            with self.assertRaisesRegex(TraceFileError, "thread clock identity limit"):
                analyze_trace_file(path, ["app.init"], max_history=1)
        self.both(doc, max_history=2)

    def test_ordinary_omitted_clock_gate_fails_same_rollback_assertion(self):
        source = Path(reporter.__file__).read_text(encoding="utf-8")
        wire = ('            if timestamp < self.thread_timestamps[tid]:\n'
                '                raise ValueError("trace timestamp moved backwards within thread: " + str(tid))\n')
        self.assertEqual(source.count(wire), 1)
        namespace = {"__name__": "tools.lifecycle_old_clock_gate", "__package__": "tools", "__file__": reporter.__file__}
        exec(compile(source.replace(wire, "            pass  # Old consumer validated pairs only.\n", 1), reporter.__file__, "exec"), namespace)
        doc = counter_capture([0])
        doc["traceEvents"][-1]["ts"] = 100
        self.reject(doc)
        with self.assertRaises(AssertionError):
            with self.assertRaises(ValueError):
                namespace["analyze_trace"](doc, ["app.init"])


class CleanupOwnershipTests(unittest.TestCase):
    @staticmethod
    def attempt(identity, reason=0, *, parent=0, tid=1, base=3000):
        common = dict(pid=1, tid=tid)
        return [
            dict(common, ph="i", cat="task", name="engine.cleanup_attempt", ts=base,
                 args=dict(task=identity, parent=parent, kind=2)),
            dict(common, ph="B", cat="task", name="engine.cleanup_attempt", ts=base + 1,
                 args=dict(id=identity, parent=identity)),
            dict(common, ph="C", cat="core", name="cleanup.reason", ts=base + 2,
                 args=dict(id=identity, parent=identity, value=reason)),
            dict(common, ph="C", cat="core", name="cleanup.completed", ts=base + 3,
                 args=dict(id=identity, parent=identity, value=int(reason == 0))),
            dict(common, ph="E", cat="task", name="engine.cleanup_attempt", ts=base + 4,
                 args=dict(id=identity, parent=identity, ns=3000, state=0 if reason == 0 else 1)),
        ]

    @classmethod
    def document(cls, reasons=(0,)):
        doc = capture()
        for i, reason in enumerate(reasons):
            doc["traceEvents"].extend(cls.attempt(i + 10, reason, base=3000 + 100 * i))
        doc["jceTrace"].update(mode="continuous", consumer_mode="threaded", worker_available=True,
            write_ok=True, capture_complete=True, unexported=0, active_threads=0,
            active_work_items=0, active_waits=0)
        return cls.recount(doc)

    @staticmethod
    def recount(doc):
        doc["jceTrace"].update(recorded=len(doc["traceEvents"]), captured=len(doc["traceEvents"]))
        return doc

    def both(self, doc, required=("app.init",), **kwargs):
        memory = analyze_trace(doc, required, cleanup_ownership=True, **kwargs)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "cleanup.json"
            path.write_text(json.dumps(doc), encoding="utf-8")
            stream = analyze_trace_file(path, required, cleanup_ownership=True, chunk_bytes=7, **kwargs)
        self.assertEqual(memory["cleanup_ownership"], stream["cleanup_ownership"])
        self.assertTrue(stream["validation_complete"])
        return stream

    def reject_both(self, doc, **kwargs):
        with self.assertRaises(ValueError):
            analyze_trace(doc, ["app.init"], cleanup_ownership=True, **kwargs)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "cleanup.json"
            path.write_text(json.dumps(doc), encoding="utf-8")
            with self.assertRaises(TraceFileError) as rejected:
                analyze_trace_file(path, ["app.init"], cleanup_ownership=True, chunk_bytes=7, **kwargs)
            self.assertTrue(rejected.exception.evidence["source_fingerprint_stable"])

    def test_success_all_refusal_reasons_recovery_and_default_contract(self):
        doc = self.document((1, 2, 3, 4, 5, 6, 0))
        result = self.both(doc)
        gate = result["cleanup_ownership"]
        self.assertEqual(gate["status"], "FINAL_CAPTURED_CLEANUP_SUCCESS_OBSERVED")
        self.assertEqual((gate["attempts_count"], gate["deferred_count"], gate["successful_count"]), (7, 6, 1))
        self.assertEqual(list(gate["reason_counts"].values()), [1] * 7)
        self.assertEqual((gate["first"]["scope_id"], gate["last"]["scope_id"]), (10, 16))
        self.assertEqual(result["nonrequired_task_outcomes"]["failed"], 6)
        self.assertNotIn("cleanup_ownership", analyze_trace(doc, ["app.init"]))
        with self.assertRaisesRegex(ValueError, "required phase missing or unsuccessful"):
            self.both(doc, required=("app.init", "engine.cleanup_attempt"))
        self.both(self.document(), required=("app.init", "engine.cleanup_attempt"))

    @classmethod
    def audio_document(cls, states=((1, 0, 1),), *, typed=True):
        doc = cls.document(())
        for i, (had, after, retired) in enumerate(states):
            base, owner, identity = 3000 + 100 * i, 10 + i, 100 + i
            outer = cls.attempt(owner, 0 if retired else 6, base=base)
            common = dict(pid=1, tid=1)
            events = [
                dict(common, ph="i", cat="task", name="engine.audio_master_retire", ts=base + 2,
                     args=dict(task=identity, parent=owner, kind=2)),
                dict(common, ph="B", cat="task", name="engine.audio_master_retire", ts=base + 3,
                     args=dict(id=identity, parent=identity))]
            if typed:
                for offset, (name, value) in enumerate(zip(
                        ("audio_master.had_device", "audio_master.device_live_after", "audio_master.retired"),
                        (had, after, retired))):
                    events.append(dict(common, ph="C", cat="core", name=name, ts=base + 4 + offset,
                                       args=dict(id=identity, parent=identity, value=value)))
            events.append(dict(common, ph="E", cat="task", name="engine.audio_master_retire", ts=base + 7,
                               args=dict(id=identity, parent=identity, ns=4000, state=0 if retired else 1)))
            for offset, event in enumerate(outer[2:]):
                event["ts"] = base + 10 + offset
            outer[-1]["args"]["ns"] = 11000
            doc["traceEvents"].extend(outer[:2] + events + outer[2:])
        return cls.recount(doc)

    def test_audio_reason6_deferral_then_retry_and_unknown7_rejected(self):
        gate = self.both(self.document((6, 0)))["cleanup_ownership"]
        self.assertEqual(gate["reason_counts"]["audio_master"], 1)
        self.assertEqual(gate["first"]["reason_name"], "audio_master")
        self.assertEqual((gate["deferred_count"], gate["successful_count"]), (1, 1))
        self.reject_both(self.document((7, 0)))

    def test_audio_typed_live_retention_then_native_return_is_distinct_from_no_device(self):
        doc = self.audio_document(((1, 1, 0), (1, 0, 1)))
        gate = self.both(doc)["cleanup_ownership"]
        audio = gate["audio_master"]
        self.assertEqual(audio["status"], "AVAILABLE")
        self.assertEqual((audio["attempts_count"], audio["typed_attempts_count"]), (2, 2))
        self.assertEqual((audio["device_uninit_return_count"], audio["retained_live_refusal_count"]), (1, 1))
        self.assertEqual((audio["no_device_success_count"], audio["failed_attempts_count"]), (0, 1))
        self.assertEqual(audio["first"]["had_device"], 1)
        self.assertEqual(audio["first"]["device_live_after"], 1)
        self.assertFalse(audio["first"]["device_uninit_return_observed"])
        self.assertTrue(audio["last"]["device_uninit_return_observed"])
        self.assertEqual(gate["reason_counts"]["audio_master"], 1)
        audio = self.both(self.audio_document(((0, 0, 1),)))["cleanup_ownership"]["audio_master"]
        self.assertEqual(audio["status"], "AVAILABLE")
        self.assertEqual((audio["device_uninit_return_count"], audio["no_device_success_count"]), (0, 1))
        self.assertFalse(audio["last"]["device_uninit_return_observed"])

    def test_audio_old_absent_fields_stay_unavailable_without_changing_cpu_admission(self):
        for doc, count in ((self.document(), 0), (self.audio_document(typed=False), 1)):
            with self.subTest(attempts=count):
                audio = self.both(doc)["cleanup_ownership"]["audio_master"]
                self.assertEqual(audio["status"], "UNAVAILABLE")
                self.assertEqual((audio["attempts_count"], audio["unavailable_attempts_count"]), (count, count))
                self.assertIsNone(audio["device_uninit_return_count"])
                if count:
                    for field in ("had_device", "device_live_after", "retired", "device_uninit_return_observed"):
                        self.assertIsNone(audio["last"][field])

    def test_audio_partial_duplicate_reordered_and_wrong_counter_shape_reject(self):
        fields = ("audio_master.had_device", "audio_master.device_live_after", "audio_master.retired")
        for name in fields:
            doc = self.audio_document()
            doc["traceEvents"] = [event for event in doc["traceEvents"] if event.get("name") != name]
            with self.subTest(missing=name): self.reject_both(self.recount(doc))
        for mutation in ("duplicate", "reordered", "unknown", "phase", "category", "owner", "thread"):
            doc = self.audio_document()
            field = next(e for e in doc["traceEvents"] if e["name"] == fields[0])
            index = doc["traceEvents"].index(field)
            if mutation == "duplicate": doc["traceEvents"].insert(index + 1, copy.deepcopy(field))
            elif mutation == "reordered":
                # Keep timestamps ordered while violating the producer field order.
                field["name"], doc["traceEvents"][index + 1]["name"] = fields[1], fields[0]
            elif mutation == "unknown": field["name"] = "audio_master.other"
            elif mutation == "phase": field["ph"] = "i"
            elif mutation == "category": field["cat"] = "render"
            elif mutation == "owner": field["args"]["parent"] = 10
            else: field["tid"] = 2
            with self.subTest(mutation=mutation): self.reject_both(self.recount(doc))

    def test_audio_fields_require_exact_01_integers_and_consistent_terminal_state(self):
        for name in ("audio_master.had_device", "audio_master.device_live_after", "audio_master.retired"):
            for value in (True, False, -1, 2, 0.0, "1", None):
                doc = self.audio_document()
                next(e for e in doc["traceEvents"] if e["name"] == name)["args"]["value"] = value
                with self.subTest(name=name, value=value): self.reject_both(doc)
        for states in (((1, 1, 1),), ((0, 1, 1),), ((0, 1, 0), (1, 0, 1))):
            with self.subTest(states=states): self.reject_both(self.audio_document(states))
        for state in (1, 2):
            doc = self.audio_document()
            next(e for e in doc["traceEvents"] if e["name"] == "engine.audio_master_retire" and e["ph"] == "E")["args"]["state"] = state
            with self.subTest(success_state=state): self.reject_both(doc)
        doc = self.audio_document(((1, 1, 0), (1, 0, 1)))
        next(e for e in doc["traceEvents"] if e["name"] == "engine.audio_master_retire" and e["ph"] == "E")["args"]["state"] = 0
        self.reject_both(doc)

    def test_nested_reentrant_attempts_close_before_outer_success(self):
        doc = self.document(())
        outer = self.attempt(10)
        inner = self.attempt(11, 2, parent=10, base=3010)
        for i, event in enumerate(outer[2:]):
            event["ts"] = 3030 + i
        doc["traceEvents"].extend(outer[:2] + inner + outer[2:])
        gate = self.both(self.recount(doc))["cleanup_ownership"]
        self.assertEqual((gate["first"]["scope_id"], gate["last"]["scope_id"]), (11, 10))
        self.assertEqual(gate["first"]["submitted_parent"], 10)
        self.assertEqual(gate["attempts_count"], 2)

    def test_interleaved_threads_keep_each_packet_and_exported_close_order(self):
        doc = self.document(())
        first, second = self.attempt(10, 3), self.attempt(11, tid=2, base=4000)
        doc["traceEvents"].extend(first[:3] + second[:3] + first[3:] + second[3:])
        gate = self.both(self.recount(doc))["cleanup_ownership"]
        self.assertEqual((gate["first"]["thread"], gate["last"]["thread"]), (1, 2))

    def test_zero_tid_requires_existing_named_cooperative_admission(self):
        doc = self.document()
        doc["jceTrace"].update(consumer_mode="cooperative", worker_available=False)
        for event in doc["traceEvents"]:
            event["tid"] = 0
        doc["traceEvents"].insert(0, dict(ph="M", pid=1, tid=0, name="thread_name", args=dict(name="main")))
        self.assertEqual(self.both(self.recount(doc))["cleanup_ownership"]["last"]["thread"], 0)
        del doc["traceEvents"][0]
        self.reject_both(self.recount(doc))

    def test_missing_attempt_final_refusal_and_unclosed_submission_reject(self):
        for reasons in ((), (1,), (0, 5)):
            with self.subTest(reasons=reasons):
                self.reject_both(self.document(reasons))
        doc = self.document()
        doc["traceEvents"].append(self.attempt(99)[0])
        self.reject_both(self.recount(doc))

    def test_missing_duplicate_submission_foreign_kind_and_service_category_reject(self):
        for mutation in ("missing", "duplicate", "kind0", "kind1", "wrongname", "category", "counter-service"):
            doc = self.document()
            if mutation == "missing": del doc["traceEvents"][3]
            elif mutation == "duplicate": doc["traceEvents"].insert(4, copy.deepcopy(doc["traceEvents"][3]))
            elif mutation.startswith("kind"): doc["traceEvents"][3]["args"]["kind"] = int(mutation[-1])
            elif mutation == "wrongname": doc["traceEvents"][3]["name"] = "foreign.service"
            elif mutation == "category": doc["traceEvents"][4]["cat"] = "wait"
            else: doc["traceEvents"][4]["ph"] = "C"
            with self.subTest(mutation=mutation): self.reject_both(self.recount(doc))

    def test_missing_duplicate_reordered_or_unknown_fields_reject(self):
        for mutation in ("reason", "completed", "duplicate", "reordered", "unknown", "phase", "category"):
            doc = self.document()
            if mutation == "reason": del doc["traceEvents"][5]
            elif mutation == "completed": del doc["traceEvents"][6]
            elif mutation == "duplicate": doc["traceEvents"].insert(6, copy.deepcopy(doc["traceEvents"][5]))
            elif mutation == "reordered": doc["traceEvents"][5:7] = reversed(doc["traceEvents"][5:7])
            elif mutation == "unknown": doc["traceEvents"][5]["name"] = "cleanup.other"
            elif mutation == "phase": doc["traceEvents"][5]["ph"] = "i"
            else: doc["traceEvents"][5]["cat"] = "render"
            with self.subTest(mutation=mutation): self.reject_both(self.recount(doc))

    def test_counters_before_begin_after_end_and_outside_timestamp_interval_reject(self):
        for mutation in ("before", "after", "early-ts", "late-ts", "backwards-ts"):
            doc = self.document()
            if mutation == "before": doc["traceEvents"].insert(4, doc["traceEvents"].pop(5))
            elif mutation == "after": doc["traceEvents"].append(doc["traceEvents"].pop(6))
            elif mutation == "early-ts": doc["traceEvents"][5]["ts"] = 1
            elif mutation == "late-ts": doc["traceEvents"][6]["ts"] = 9999
            else: doc["traceEvents"][6]["ts"] = doc["traceEvents"][5]["ts"] - 1
            with self.subTest(mutation=mutation): self.reject_both(doc)

    def test_counter_identity_thread_and_cross_attempt_fields_cannot_fake_pair(self):
        for field, value in (("id", 11), ("parent", 11), ("id", 0), ("id", 1 << 64), ("parent", True)):
            doc = self.document()
            doc["traceEvents"][5]["args"][field] = value
            with self.subTest(field=field, value=value): self.reject_both(doc)
        doc = self.document()
        doc["traceEvents"][5]["tid"] = 2
        self.reject_both(doc)
        doc = self.document((3, 0))
        # Move the prior attempt's completed counter into the final attempt,
        # retagging it cannot repair the first packet or replace its own field.
        doc["traceEvents"][6]["args"].update(id=11, parent=11)
        self.reject_both(doc)

    def test_reason_completed_types_bounds_and_terminal_state_agree(self):
        for field, values in ((5, (-1, 7, 0.0, True, "0", None)), (6, (-1, 2, 1.0, True, "1", None))):
            for value in values:
                doc = self.document()
                doc["traceEvents"][field]["args"]["value"] = value
                with self.subTest(field=field, value=value): self.reject_both(doc)
        for reason, completed, state in ((0, 0, 0), (1, 1, 1), (0, 1, 1), (0, 1, 2), (1, 0, 0), (1, 0, 2)):
            doc = self.document()
            doc["traceEvents"][5]["args"]["value"] = reason
            doc["traceEvents"][6]["args"]["value"] = completed
            doc["traceEvents"][7]["args"]["state"] = state
            with self.subTest(reason=reason, completed=completed, state=state): self.reject_both(doc)

    def test_complete_continuous_footer_and_loss_gates_remain_mandatory(self):
        mutations = (dict(mode="resident"), dict(capture_complete=False), dict(write_ok=False),
            dict(active_threads=1), dict(active_work_items=1), dict(active_waits=1), dict(unexported=1),
            dict(capture_generation_stable=False), dict(cursor_lost=1, lost=1))
        for change in mutations:
            doc = self.document()
            doc["jceTrace"].update(change)
            with self.subTest(change=change): self.reject_both(doc)
        doc = self.document()
        del doc["jceTrace"]["capture_complete"]
        self.reject_both(doc)

    def test_exact_wide_id_and_explicit_opt_in_bool(self):
        doc = self.document(())
        doc["traceEvents"].extend(self.attempt((1 << 64) - 1))
        gate = self.both(self.recount(doc))["cleanup_ownership"]
        self.assertEqual(gate["last"]["scope_id"], str((1 << 64) - 1))
        for value in (1, 0, "true", None):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "explicit bool"):
                analyze_trace(doc, ["app.init"], cleanup_ownership=value)

    def test_cli_full_stream_tail_failure_and_bounded_history(self):
        with tempfile.TemporaryDirectory() as directory:
            path, out = Path(directory) / "cleanup.json", Path(directory) / "report.json"
            path.write_text(json.dumps(self.document((3, 3, 0))), encoding="utf-8")
            argv = ["lifecycle_trace_report.py", str(path), "--require", "app.init", "--cleanup-ownership", "--out", str(out)]
            with mock.patch.object(sys, "argv", argv), redirect_stdout(io.StringIO()):
                self.assertEqual(reporter.main(), 0)
            self.assertEqual(json.loads(out.read_text())["cleanup_ownership"]["attempts_count"], 3)
            with self.assertRaisesRegex(TraceFileError, "historical task identity limit"):
                analyze_trace_file(path, ["app.init"], cleanup_ownership=True, max_history=3)
            path.write_bytes(path.read_bytes() + b"INVALID_TAIL")
            with mock.patch.object(sys, "argv", argv), redirect_stdout(io.StringIO()):
                self.assertEqual(reporter.main(), 1)
            failed = json.loads(out.read_text())
            self.assertFalse(failed["validation_complete"])
            self.assertEqual(failed["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())

    def test_ordinary_omitted_final_gate_cannot_pass_same_retirement_assertion(self):
        source = Path(reporter.__file__).read_text(encoding="utf-8")
        wire = '            result["cleanup_ownership"] = self.cleanup.finish(metadata, self.tasks, self.completed_services)\n'
        self.assertEqual(source.count(wire), 1)
        namespace = {"__name__": "tools.lifecycle_old_cleanup_gate", "__package__": "tools", "__file__": reporter.__file__}
        exec(compile(source.replace(wire, "            pass  # Old consumer omitted the ownership retirement gate.\n", 1),
                     reporter.__file__, "exec"), namespace)
        doc = self.document((0, 5))
        self.reject_both(doc)
        with self.assertRaises(AssertionError):
            with self.assertRaises(ValueError):
                namespace["analyze_trace"](doc, ["app.init"], cleanup_ownership=True)


class RequiredScopeWitnessTests(unittest.TestCase):
    def call(self, function, *args, **kwargs):
        # SAME call arguments on OLD RED and GREEN. A missing future API is
        # an explicit feature-unavailable assertion, never a retry or shim.
        kwargs["scope_witnesses"] = True
        try:
            return function(*args, **kwargs)
        except TypeError as error:
            if "scope_witnesses" in str(error):
                self.fail("required scope witness API unavailable: %s" % error)
            raise

    def both(self, document, required=("app.init",)):
        memory = self.call(reporter.analyze_trace, document, list(required))
        with tempfile.TemporaryDirectory(prefix="jce-scope-witness-") as folder:
            path = Path(folder) / "trace.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            stream = self.call(reporter.analyze_trace_file, path, list(required), chunk_bytes=7)
            self.assertEqual(stream["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertTrue(stream["source_fingerprint_stable"])
        self.assertIn("required_scope_witnesses", memory)
        self.assertIn("required_scope_witnesses", stream)
        self.assertEqual(memory["required_scope_witnesses"], stream["required_scope_witnesses"])
        return stream["required_scope_witnesses"]

    def test_single_interval_uses_actual_matched_timestamps_and_indices(self):
        report = self.both(capture())
        self.assertIn("not a full timeline", report["retention"])
        self.assertIn("no OS/SDL/GPU", report["scope"])
        row, = report["required"]
        self.assertEqual((row["name"], row["observed_count"], row["samples_complete"]), ("app.init", 1, True))
        self.assertEqual(row["first"], dict(scope_id=1, thread=1, owner=1, submitted_parent=0, state=0,
            begin_timestamp_us=1000, end_timestamp_us=2000, begin_raw_index=1, end_raw_index=2))
        self.assertEqual(row["first"], row["last"])
        # The authored measured ns is deliberately NOT the ts delta. Never
        # synthesize a begin timestamp by subtracting the measured duration.
        self.assertNotEqual(row["first"]["begin_timestamp_us"], 2000 - 999000 / 1000)

    def test_nested_owner_and_end_order_do_not_cross_pair(self):
        doc = capture()
        doc["traceEvents"][2:2] = [
            dict(ph="i", cat="task", pid=1, tid=1, ts=1100, name="child", args=dict(task=9, parent=1, kind=2)),
            dict(ph="B", cat="task", pid=1, tid=1, ts=1200, name="child", args=dict(id=9, parent=9)),
            dict(ph="E", cat="task", pid=1, tid=1, ts=1500, name="child", args=dict(id=9, parent=9, ns=300000, state=0))]
        doc["jceTrace"].update(recorded=6, captured=6)
        rows = {r["name"]: r for r in self.both(doc, ("app.init", "child"))["required"]}
        self.assertEqual((rows["app.init"]["first"]["begin_raw_index"], rows["app.init"]["last"]["end_raw_index"]), (1, 5))
        self.assertEqual(rows["child"]["first"], dict(scope_id=9, thread=1, owner=9, submitted_parent=1, state=0,
            begin_timestamp_us=1200, end_timestamp_us=1500, begin_raw_index=3, end_raw_index=4))

    def test_same_name_nested_intervals_follow_completion_order_not_envelope(self):
        doc = capture()
        doc["traceEvents"][2:2] = [
            dict(ph="i", cat="task", pid=1, tid=1, ts=1100, name="app.init", args=dict(task=9, parent=1, kind=2)),
            dict(ph="B", cat="task", pid=1, tid=1, ts=1200, name="app.init", args=dict(id=9, parent=9)),
            dict(ph="E", cat="task", pid=1, tid=1, ts=1500, name="app.init", args=dict(id=9, parent=9, ns=300000, state=0))]
        doc["jceTrace"].update(recorded=6, captured=6)
        row, = self.both(doc)["required"]
        self.assertEqual(row["observed_count"], 2)
        self.assertTrue(row["samples_complete"])
        self.assertEqual((row["first"]["begin_timestamp_us"], row["first"]["end_timestamp_us"]), (1200, 1500))
        self.assertEqual((row["last"]["begin_timestamp_us"], row["last"]["end_timestamp_us"]), (1000, 2000))
        self.assertEqual((row["first"]["begin_raw_index"], row["last"]["end_raw_index"]), (3, 5))

    def test_repeated_services_keep_only_first_last_and_count(self):
        doc = capture(); doc["traceEvents"] = []
        for index in range(513):
            block = capture()["traceEvents"]
            for event in block:
                event["ts"] += index * 3000
                for key in ("task", "id", "parent"):
                    if key in event["args"] and event["args"][key]: event["args"][key] = index + 1
            doc["traceEvents"].extend(block)
        doc["jceTrace"].update(recorded=1539, captured=1539)
        row, = self.both(doc)["required"]
        self.assertEqual(row["observed_count"], 513)
        self.assertFalse(row["samples_complete"])
        self.assertEqual((row["first"]["scope_id"], row["last"]["scope_id"]), (1, 513))
        self.assertEqual(set(row), {"name", "observed_count", "samples_complete", "first", "last"})
        self.assertEqual(row["last"]["end_raw_index"], 1538)

    def test_invalid_order_pair_state_and_loss_cannot_publish_witnesses(self):
        for kind in ("backwards", "unpaired", "failed", "cancelled", "loss"):
            bad = capture()
            if kind == "backwards": bad["traceEvents"][-1]["ts"] = 500
            if kind == "unpaired": bad["traceEvents"][-1]["args"]["id"] = 2
            if kind == "failed": bad["traceEvents"][-1]["args"]["state"] = 1
            if kind == "cancelled": bad["traceEvents"][-1]["args"]["state"] = 2
            if kind == "loss": bad["jceTrace"].update(lost=1, cursor_lost=1)
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                self.call(reporter.analyze_trace, bad, ["app.init"])

    def test_default_false_output_is_identical_and_absent(self):
        self.assertIn("scope_witnesses", inspect.signature(reporter.analyze_trace).parameters)
        expected = reporter.analyze_trace(capture(), ["app.init"])
        actual = reporter.analyze_trace(capture(), ["app.init"], scope_witnesses=False)
        self.assertEqual(actual, expected)
        self.assertNotIn("required_scope_witnesses", actual)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "trace.json"; path.write_text(json.dumps(capture()), encoding="utf-8")
            expected = reporter.analyze_trace_file(path, ["app.init"])
            actual = reporter.analyze_trace_file(path, ["app.init"], scope_witnesses=False)
            self.assertEqual(actual, expected)
            self.assertNotIn("required_scope_witnesses", actual)

    def test_strict_boolean_refuses_before_stream_io(self):
        self.assertIn("scope_witnesses", inspect.signature(reporter.analyze_trace_file).parameters)
        for value in (1, 0, None, "true", [], {}):
            with self.subTest(value=value), mock.patch.object(Path, "open") as opened:
                with self.assertRaisesRegex(ValueError, "scope_witnesses must be an explicit bool"):
                    reporter.analyze_trace_file(Path("absent.json"), ["app.init"], scope_witnesses=value)
                opened.assert_not_called()
            with self.assertRaisesRegex(ValueError, "explicit bool"):
                reporter.analyze_trace(capture(), ["app.init"], scope_witnesses=value)

    def test_opt_in_required_name_budget_and_no_false_prefix_pass(self):
        self.assertIn("scope_witnesses", inspect.signature(reporter.analyze_trace).parameters)
        with self.assertRaisesRegex(reporter.TraceLimitError, "scope witness limit"):
            reporter.analyze_trace(capture(), ["app.init"] + ["phase.%d" % i for i in range(128)], scope_witnesses=True)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "trace.json"; path.write_bytes(json.dumps(capture()).encode() + b"BAD_TAIL")
            with self.assertRaises(reporter.TraceFileError) as caught:
                self.call(reporter.analyze_trace_file, path, ["app.init"])
            self.assertEqual(caught.exception.evidence["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())

    def test_shader_filesystem_and_scope_witnesses_share_memory_and_stream_owner(self):
        from tools.tests.test_shader_provenance_observation import success as shader_success
        from tools.tests.test_filesystem_read_observation import sample, success as read_success
        from tools import shader_provenance_observation as shaders
        document = shader_success()
        document["traceEvents"] += sample(200, timestamp=500, state=0, values=read_success())
        document["traceEvents"].sort(key=lambda event: event["ts"])
        document["jceTrace"].update(recorded=len(document["traceEvents"]), captured=len(document["traceEvents"]))
        required = ["app.init", shaders.CREATE, "fs.host.read_all"]
        options = dict(shader_provenance=True, filesystem_read_details=True)
        legacy = reporter.analyze_trace(document, required, **options)
        memory = self.call(reporter.analyze_trace, document, required, **options)
        with tempfile.TemporaryDirectory(prefix="jce-scope-composition-") as folder:
            path = Path(folder) / "trace.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            stream = self.call(reporter.analyze_trace_file, path, required, chunk_bytes=7, **options)
            self.assertEqual(stream["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertTrue(stream["source_fingerprint_stable"])
        for report in (memory, stream):
            self.assertIn("required_scope_witnesses", report)
            self.assertEqual(report["shader_provenance"], legacy["shader_provenance"])
            self.assertEqual(report["filesystem_read_details"], legacy["filesystem_read_details"])
            self.assertEqual(report["shader_provenance"]["consumed_create_receipt_count"], 2)
            self.assertEqual(report["filesystem_read_details"]["complete_packets_count"], 1)
            rows = {row["name"]: row for row in report["required_scope_witnesses"]["required"]}
            self.assertEqual(rows[shaders.CREATE]["observed_count"], 2)
            self.assertEqual(rows["fs.host.read_all"]["first"]["begin_timestamp_us"], 500)
            self.assertEqual(rows["fs.host.read_all"]["last"]["end_timestamp_us"], 530)
        self.assertEqual(memory["required_scope_witnesses"], stream["required_scope_witnesses"])
        self.assertNotIn("required_scope_witnesses", legacy)

    def test_cli_opt_in_uses_same_canonical_reader(self):
        with tempfile.TemporaryDirectory() as folder:
            path, out = Path(folder) / "trace.json", Path(folder) / "report.json"
            path.write_text(json.dumps(capture()), encoding="utf-8")
            argv = ["lifecycle_trace_report.py", str(path), "--require", "app.init", "--scope-witnesses", "--out", str(out)]
            with mock.patch.object(sys, "argv", argv), redirect_stdout(io.StringIO()):
                try: code = reporter.main()
                except SystemExit as error: self.fail("scope witness CLI rejected opt-in: %s" % error.code)
            self.assertEqual(code, 0)
            self.assertIn("required_scope_witnesses", json.loads(out.read_text()))


class ScopePrefixObservationTests(unittest.TestCase):
    @staticmethod
    def document():
        doc = capture()
        for identity, thread, name, begin, end, measured in (
                (9, 2, "bgfx.cpu.submit", 3000, 4000, 900000),
                (10, 2, "bgfx.cpu.submit", 5000, 8000, 2900000),
                (11, 3, "bgfx.cpu.submit", 100, 600, 400000),
                (12, 2, "bgfx.cpu.wait", 9000, 9500, 300000)):
            doc["traceEvents"].extend([
                dict(ph="B", cat="render", pid=1, tid=thread, ts=begin, name=name,
                     args=dict(id=identity, parent=0)),
                dict(ph="E", cat="render", pid=1, tid=thread, ts=end, name=name,
                     args=dict(id=identity, parent=0, ns=measured))])
        doc["jceTrace"].update(recorded=len(doc["traceEvents"]), captured=len(doc["traceEvents"]))
        return doc

    def both(self, doc=None, prefixes=("bgfx.cpu.",), **limits):
        doc = self.document() if doc is None else doc
        memory = reporter.analyze_trace(doc, ["app.init"], scope_prefixes=prefixes)
        with tempfile.TemporaryDirectory(prefix="jce-prefix-observation-") as folder:
            path = Path(folder) / "trace.json"
            path.write_text(json.dumps(doc), encoding="utf-8")
            stream = reporter.analyze_trace_file(path, ["app.init"], scope_prefixes=prefixes,
                                                chunk_bytes=7, **limits)
            self.assertEqual(stream["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertTrue(stream["source_fingerprint_stable"])
            self.assertTrue(stream["validation_complete"])
        self.assertEqual(memory["scope_prefix_observations"], stream["scope_prefix_observations"])
        return stream["scope_prefix_observations"]

    def assert_aggregates(self, report):
        self.assertEqual(report["status"], "CPU_SCOPE_ACTIVITY_OBSERVED")
        rows = {(row["name"], row["thread"]): row for row in report["aggregates"]}
        self.assertEqual(set(rows), {("bgfx.cpu.submit", 2), ("bgfx.cpu.submit", 3), ("bgfx.cpu.wait", 2)})
        row = rows[("bgfx.cpu.submit", 2)]
        self.assertEqual((row["observed_count"], row["elapsed_ms_sum"], row["elapsed_ms_min"], row["elapsed_ms_max"]),
                         (2, 4.0, 1.0, 3.0))
        self.assertEqual((row["measured_ns_sum"], row["measured_ns_min"], row["measured_ns_max"]),
                         (3800000, 900000, 2900000))
        self.assertEqual((row["first"]["begin_timestamp_us"], row["first"]["end_timestamp_us"],
                          row["first"]["begin_raw_index"], row["first"]["end_raw_index"]), (3000, 4000, 3, 4))
        self.assertEqual((row["last"]["begin_timestamp_us"], row["last"]["end_timestamp_us"],
                          row["last"]["begin_raw_index"], row["last"]["end_raw_index"]), (5000, 8000, 5, 6))
        self.assertEqual(row["first"]["measured_ns"], 900000)
        self.assertEqual(rows[("bgfx.cpu.submit", 3)]["elapsed_ms_sum"], 0.5)
        self.assertIn("not OS CPU execution", report["scope"])
        self.assertIn("not GPU", report["scope"])
        self.assertIn("not wall time", report["scope"])

    def test_memory_stream_and_overlapping_prefixes_keep_exact_thread_intervals(self):
        report = self.both(prefixes=("bgfx.cpu.", "bgfx.cpu.submit", "absent."))
        self.assert_aggregates(report)
        self.assertEqual(report["prefixes"], [
            dict(prefix="bgfx.cpu.", observed_count=4, status="CPU_SCOPE_ACTIVITY_OBSERVED"),
            dict(prefix="bgfx.cpu.submit", observed_count=3, status="CPU_SCOPE_ACTIVITY_OBSERVED"),
            dict(prefix="absent.", observed_count=0, status="NO_ACTIVITY")])

    def test_default_has_no_observer_and_empty_matches_are_no_activity(self):
        expected = reporter.analyze_trace(capture(), ["app.init"])
        self.assertEqual(expected, reporter.analyze_trace(capture(), ["app.init"], scope_prefixes=()))
        self.assertNotIn("scope_prefix_observations", expected)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "trace.json"
            path.write_text(json.dumps(capture()), encoding="utf-8")
            expected = reporter.analyze_trace_file(path, ["app.init"])
            self.assertEqual(expected, reporter.analyze_trace_file(path, ["app.init"], scope_prefixes=[]))
            self.assertNotIn("scope_prefix_observations", expected)
        empty = self.both(capture())
        self.assertEqual((empty["status"], empty["aggregates"]), ("NO_ACTIVITY", []))
        self.assertEqual(empty["prefixes"][0]["status"], "NO_ACTIVITY")

    def test_prefix_types_duplicates_lengths_and_count_refuse_before_input_or_output(self):
        invalid = (None, True, "bgfx.cpu.", [""], [" "], [" bgfx"], ["bgfx\n"], [1],
                   ["x" * 129], ["x", "x"], ["prefix.%d" % index for index in range(129)])
        for prefixes in invalid:
            with self.subTest(prefixes=prefixes), mock.patch.object(Path, "open") as opened:
                with self.assertRaisesRegex(ValueError, "scope prefix"):
                    reporter.analyze_trace_file("absent.json", ["app.init"], scope_prefixes=prefixes)
                opened.assert_not_called()
            with self.assertRaisesRegex(ValueError, "scope prefix"):
                reporter.analyze_trace(capture(), ["app.init"], scope_prefixes=prefixes)
        with mock.patch.object(sys, "argv", ["trace", "absent.json", "--require", "app.init",
                                            "--scope-prefix", "", "--out", "absent/report.json"]), \
                mock.patch.object(Path, "open") as opened, mock.patch.object(Path, "mkdir") as mkdir, \
                mock.patch.object(Path, "write_text") as written, redirect_stdout(io.StringIO()), \
                redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as failed:
                reporter.main()
            self.assertEqual(failed.exception.code, 2)
            opened.assert_not_called(); mkdir.assert_not_called(); written.assert_not_called()

    def test_existing_row_name_and_active_budgets_reject_instead_of_truncating(self):
        doc = self.document()
        doc["traceEvents"].extend([
            dict(ph="B", cat="render", pid=1, tid=2, ts=10000, name="bgfx.cpu.extra", args=dict(id=13, parent=0)),
            dict(ph="E", cat="render", pid=1, tid=2, ts=10100, name="bgfx.cpu.extra", args=dict(id=13, parent=0, ns=90000))])
        doc["jceTrace"].update(recorded=13, captured=13)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "trace.json"
            path.write_text(json.dumps(doc), encoding="utf-8")
            for limits, message in ((dict(max_history=3), "scope prefix aggregate limit"),
                                    (dict(max_names=2), "retained name limit")):
                with self.subTest(limits=limits), self.assertRaisesRegex(TraceFileError, message) as failed:
                    reporter.analyze_trace_file(path, ["app.init"], scope_prefixes=["bgfx.cpu."], **limits)
                self.assertTrue(failed.exception.evidence["source_fingerprint_stable"])
            reporter.analyze_trace_file(path, ["app.init"], scope_prefixes=["bgfx.cpu."], max_history=4, max_names=4)
            doc["traceEvents"][4:4] = [
                dict(ph="B", cat="render", pid=1, tid=2, ts=3200, name="bgfx.cpu.nested", args=dict(id=99, parent=0)),
                dict(ph="E", cat="render", pid=1, tid=2, ts=3500, name="bgfx.cpu.nested", args=dict(id=99, parent=0, ns=200000))]
            doc["jceTrace"].update(recorded=15, captured=15)
            path.write_text(json.dumps(doc), encoding="utf-8")
            with self.assertRaisesRegex(TraceFileError, "active scope limit"):
                reporter.analyze_trace_file(path, ["app.init"], scope_prefixes=["bgfx.cpu."], max_active=1)

    def test_nested_same_name_services_keep_completion_order_outcomes_and_shared_witnesses(self):
        doc = capture()
        doc["traceEvents"][2:2] = [
            dict(ph="i", cat="task", pid=1, tid=1, ts=1100, name="app.init", args=dict(task=9, parent=1, kind=2)),
            dict(ph="B", cat="task", pid=1, tid=1, ts=1200, name="app.init", args=dict(id=9, parent=9)),
            dict(ph="E", cat="task", pid=1, tid=1, ts=1500, name="app.init", args=dict(id=9, parent=9, ns=200000, state=0))]
        doc["jceTrace"].update(recorded=6, captured=6)
        row, = self.both(doc, prefixes=["app."])["aggregates"]
        self.assertEqual((row["observed_count"], row["elapsed_ms_sum"], row["measured_ns_sum"]), (2, 1.3, 1199000))
        self.assertEqual((row["first"]["id"], row["last"]["id"]), (9, 1))
        self.assertEqual((row["first"]["begin_raw_index"], row["last"]["end_raw_index"]), (3, 5))
        result = reporter.analyze_trace(doc, ["app.init"], scope_prefixes=["app."], scope_witnesses=True)
        witness, = result["required_scope_witnesses"]["required"]
        self.assertEqual(witness["first"]["begin_raw_index"], row["first"]["begin_raw_index"])
        doc["traceEvents"][2:5] = [dict(event, name="optional.failed") for event in doc["traceEvents"][2:5]]
        doc["traceEvents"][4]["args"]["state"] = 1
        report = self.both(doc, prefixes=["optional."])
        row, = report["aggregates"]
        self.assertEqual((row["failed"], row["cancelled"], row["first"]["state"]), (1, 0, 1))
        self.assertEqual(report["status"], "CPU_SCOPE_ACTIVITY_OBSERVED")

    def test_full_validator_rejects_nonselected_tail_loss_pair_and_required_failure(self):
        for mutation in ("unpaired", "backwards", "loss", "required", "tail"):
            doc = self.document()
            if mutation == "unpaired": doc["traceEvents"][-1]["args"]["id"] = 99
            if mutation == "backwards": doc["traceEvents"][-1]["ts"] = 50
            if mutation == "loss": doc["jceTrace"].update(lost=1, cursor_lost=1)
            if mutation == "required": doc["traceEvents"][2]["args"]["state"] = 1
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "trace.json"
                path.write_bytes(json.dumps(doc).encode() + (b"BAD_TAIL" if mutation == "tail" else b""))
                with self.subTest(mutation=mutation), self.assertRaises(TraceFileError) as failed:
                    reporter.analyze_trace_file(path, ["app.init"], scope_prefixes=["absent."])
                self.assertEqual(failed.exception.evidence["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())

    def test_actual_cli_repeatable_wiring_and_no_activity_keep_cpu_lifecycle_status(self):
        with tempfile.TemporaryDirectory() as folder:
            path, out = Path(folder) / "trace.json", Path(folder) / "report.json"
            path.write_text(json.dumps(self.document()), encoding="utf-8")
            argv = ["trace", str(path), "--require", "app.init", "--scope-prefix", "bgfx.cpu.",
                    "--scope-prefix", "absent.", "--out", str(out)]
            with mock.patch.object(sys, "argv", argv), redirect_stdout(io.StringIO()):
                self.assertEqual(reporter.main(), 0)
            result = json.loads(out.read_text())
            self.assert_aggregates(result["scope_prefix_observations"])
            self.assertEqual(result["status"], "REQUIRED_CPU_LIFECYCLE_SCOPES_OBSERVED")
            self.assertEqual(result["scope_prefix_observations"]["prefixes"][-1]["status"], "NO_ACTIVITY")

    def test_disconnected_and_wrong_sum_controls_fail_the_same_behavior_assertion(self):
        source = Path(reporter.__file__).read_text(encoding="utf-8")
        mutations = (
            ("            self._record_scope_prefix(scope, start, index, timestamp, duration_ns)\n", "            pass\n"),
            ('        _accumulate_scope_timing(row, scope["elapsed_ms"], duration_ns, "scope prefix")\n',
             '        _accumulate_scope_timing(row, scope["elapsed_ms"] * 2, duration_ns, "scope prefix")\n'))
        for wire, replacement in mutations:
            self.assertEqual(source.count(wire), 1)
            namespace = dict(__name__="tools.lifecycle_mutated_prefix", __package__="tools", __file__=reporter.__file__)
            exec(compile(source.replace(wire, replacement, 1), reporter.__file__, "exec"), namespace)
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "trace.json"
                path.write_text(json.dumps(self.document()), encoding="utf-8")
                report = namespace["analyze_trace_file"](path, ["app.init"], scope_prefixes=["bgfx.cpu."])
                self.assertTrue(report["validation_complete"])
                self.assertEqual(report["events_validated"], 11)
                with self.assertRaises(AssertionError):
                    self.assert_aggregates(report["scope_prefix_observations"])


if __name__ == "__main__": unittest.main()
