"""Screenshot ledger controls through the canonical memory/stream/CLI reader.

The Native-file mode reads a real Core ring/capture-writer protocol fixture.
Actual PNG callback outcomes have a separate Native renderer fixture; this
protocol/file control does not claim an end-to-end screenshot or GPU result.
"""
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
from tools import screenshot_request_observation as screenshot

SERVICE = "lifecycle.screenshot_protocol"
UINT64_MAX = (1 << 64) - 1


def counter(name, identity, value, *, tid=2, timestamp=100):
    return dict(ph="C", cat="render", pid=1, tid=tid, ts=timestamp, name=name,
                args=dict(id=identity, parent=0, value=value))


def protocol_events(identities=range(1, 7)):
    identities = tuple(identities)
    admits = [counter(screenshot.ADMIT, identity, index // 3)
              for index, identity in enumerate(identities)]
    terminals = [counter(screenshot.TERMINAL, identities[index], index % 3, tid=3)
                 for index in (1, 0, 3, 2, 5, 4)]
    return admits + terminals


def capture(events=()):
    lifecycle = [
        dict(ph="i", cat="task", pid=1, tid=1, ts=10, name=SERVICE,
             args=dict(task=1, parent=0, kind=2)),
        dict(ph="B", cat="task", pid=1, tid=1, ts=11, name=SERVICE,
             args=dict(id=1, parent=1)),
        dict(ph="E", cat="task", pid=1, tid=1, ts=12, name=SERVICE,
             args=dict(id=1, parent=1, state=0, ns=1000))]
    document = dict(traceEvents=lifecycle + list(events), jceTrace=dict(
        mode="continuous", consumer_mode="threaded", worker_available=True,
        write_ok=True, capture_complete=True, unexported=0, active_threads=0,
        active_work_items=0, active_waits=0, capture_generation_stable=True,
        sequence_exhausted=False, overwritten_before_capture=0, cursor_lost=0, lost=0))
    return recount(document)


def recount(document):
    document["jceTrace"].update(recorded=len(document["traceEvents"]),
                                captured=len(document["traceEvents"]))
    return document


def ledger_indices(document, name):
    return [index for index, event in enumerate(document["traceEvents"])
            if event.get("name") == name]


def ledger_mutants(document):
    """Writer-derived deliberate mutations; metadata counts are recounted so
    a missing ledger edge is not hidden behind the unrelated count gate."""
    variants = []
    for kind, name in (("missing-admit", screenshot.ADMIT),
                       ("missing-terminal", screenshot.TERMINAL)):
        changed = copy.deepcopy(document)
        del changed["traceEvents"][ledger_indices(changed, name)[0]]
        variants.append((kind, recount(changed)))
    for kind, name in (("duplicate-admit", screenshot.ADMIT),
                       ("duplicate-terminal", screenshot.TERMINAL)):
        changed = copy.deepcopy(document)
        index = ledger_indices(changed, name)[0]
        changed["traceEvents"].insert(index + 1, copy.deepcopy(changed["traceEvents"][index]))
        variants.append((kind, recount(changed)))
    for name, field, value in (
            (screenshot.ADMIT, "value", 2), (screenshot.TERMINAL, "value", 3),
            (screenshot.TERMINAL, "id", 1), (screenshot.ADMIT, "parent", 1),
            (screenshot.ADMIT, "id", 1 << 64), (screenshot.ADMIT, "id", True)):
        changed = copy.deepcopy(document)
        changed["traceEvents"][ledger_indices(changed, name)[0]]["args"][field] = value
        variants.append((name + "-" + field + "-" + str(value), changed))
    for field, value in (("cat", "core"), ("ph", "i"),
                         ("name", screenshot.PREFIX + "unknown")):
        changed = copy.deepcopy(document)
        changed["traceEvents"][ledger_indices(changed, screenshot.ADMIT)[0]][field] = value
        variants.append((field, changed))
    changed = copy.deepcopy(document)
    terminal_index = ledger_indices(changed, screenshot.TERMINAL)[0]
    terminal = changed["traceEvents"].pop(terminal_index)
    # A unique thread avoids an unrelated per-thread clock rollback rejecting
    # this mutation before the ledger's terminal-before-admit gate.
    terminal["tid"] = 99
    changed["traceEvents"].insert(ledger_indices(changed, screenshot.ADMIT)[0], terminal)
    variants.append(("terminal-before-admit", changed))
    return variants


class ScreenshotRequestTests(unittest.TestCase):
    def setUp(self):
        output = Path(__file__).resolve().parents[2] / "build/tests/screenshot-request-observation"
        output.mkdir(parents=True, exist_ok=True)
        self.folder = tempfile.TemporaryDirectory(prefix="cpu-", dir=output)
        self.addCleanup(self.folder.cleanup)
        self.path = Path(self.folder.name) / "trace.json"

    def file(self, document, **kwargs):
        self.path.write_text(json.dumps(document, allow_nan=False), encoding="utf-8")
        return reporter.analyze_trace_file(self.path, [SERVICE], screenshot_requests=True,
                                           chunk_bytes=7, **kwargs)

    def both(self, document):
        memory = reporter.analyze_trace(document, [SERVICE], screenshot_requests=True)
        streamed = self.file(document)
        self.assertEqual(memory["screenshot_requests"], streamed["screenshot_requests"])
        self.assertTrue(streamed["validation_complete"])
        self.assertEqual(streamed["sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())
        return streamed["screenshot_requests"]

    def reject(self, document):
        with self.assertRaises(ValueError):
            reporter.analyze_trace(document, [SERVICE], screenshot_requests=True)
        with self.assertRaises(reporter.TraceFileError) as failed:
            self.file(document)
        self.assertEqual(failed.exception.evidence["sha256"],
                         hashlib.sha256(self.path.read_bytes()).hexdigest())

    def test_modes_outcomes_and_interleaved_terminal_order(self):
        data = self.both(capture(protocol_events()))
        self.assertEqual(data["status"], "CPU_SCREENSHOT_REQUEST_TERMINALS_OBSERVED")
        self.assertTrue(data["activity_observed"])
        self.assertEqual((data["admitted_count"], data["terminal_count"]), (6, 6))
        self.assertEqual(data["mode_counts"], dict(backbuffer=3, readback=3))
        self.assertEqual(data["outcome_counts"], dict(succeeded=2, failed=2, cancelled=2))
        self.assertEqual(data["peak_unclosed_admissions"], 6)
        self.assertEqual((data["first"]["identity"], data["last"]["identity"]), (2, 5))
        self.assertEqual((data["first"]["admit_thread"], data["first"]["terminal_thread"]), (2, 3))
        self.assertEqual((data["first"]["admit_raw_index"], data["first"]["terminal_raw_index"]), (4, 9))
        self.assertEqual(data["cpu_pixels_count"], 0)
        self.assertIsNone(data["first_cpu_pixels"])
        self.assertIsNone(data["last_cpu_pixels"])
        self.assertNotIn("cpu_pixels", data["first"])
        self.assertIn("no GPU completion", data["scope"])

    def test_cpu_pixels_witnesses_match_backend_requests(self):
        first, second = UINT64_MAX - 2, UINT64_MAX - 1
        events = [counter(screenshot.ADMIT, first, 0),
                  counter(screenshot.ADMIT, second, 0),
                  counter(screenshot.ADMIT, UINT64_MAX, 1),
                  counter(screenshot.CPU_PIXELS, first, 0, tid=4, timestamp=150),
                  counter(screenshot.CPU_PIXELS, second, 65535, tid=4, timestamp=151),
                  counter(screenshot.TERMINAL, second, 1, tid=3, timestamp=200),
                  counter(screenshot.TERMINAL, first, 0, tid=3, timestamp=201),
                  counter(screenshot.TERMINAL, UINT64_MAX, 2, tid=3, timestamp=202)]
        data = self.both(capture(events))
        self.assertEqual(data["cpu_pixels_count"], 2)
        self.assertEqual(data["first_cpu_pixels"], dict(identity=str(first), raw_index=6,
            timestamp_us=150, thread=4, framebuffer_index=0))
        self.assertEqual(data["last_cpu_pixels"], dict(identity=str(second), raw_index=7,
            timestamp_us=151, thread=4, framebuffer_index=65535))
        self.assertEqual(data["first"]["cpu_pixels"], data["last_cpu_pixels"])
        self.assertNotIn("cpu_pixels", data["last"])

    def test_cpu_pixels_reject_invalid_request_binding_and_order(self):
        admit = counter(screenshot.ADMIT, 1, 0, timestamp=110)
        marker = counter(screenshot.CPU_PIXELS, 1, 7, tid=4, timestamp=120)
        terminal = counter(screenshot.TERMINAL, 1, 0, tid=3, timestamp=130)
        variants = {
            "orphan": [admit, counter(screenshot.CPU_PIXELS, 2, 7), terminal],
            "after-close": [admit, terminal, marker],
            "before-admit": [marker, admit, terminal],
            "duplicate": [admit, marker, marker, terminal],
            "readback": [counter(screenshot.ADMIT, 1, 1), marker, terminal],
            "reversed-admit-time": [admit, dict(marker, ts=109), terminal],
            "reversed-terminal-time": [admit, marker, dict(terminal, ts=119)],
            "missing-terminal": [admit, marker],
        }
        for name, events in variants.items():
            with self.subTest(case=name):
                self.reject(capture(events))
        for field, value in (("value", 65536), ("value", -1), ("value", True),
                             ("parent", 1), ("id", 0)):
            bad_marker = dict(marker, args=dict(marker["args"], **{field: value}))
            with self.subTest(field=field, value=value):
                self.reject(capture([admit, bad_marker, terminal]))

    def test_no_requests_is_explicit_no_activity(self):
        data = self.both(capture())
        self.assertEqual(data["status"], "NO_ACTIVITY")
        self.assertFalse(data["activity_observed"])
        self.assertEqual((data["admitted_count"], data["terminal_count"]), (0, 0))
        self.assertIsNone(data["first"])
        self.assertIsNone(data["last"])
        self.assertIn("NO_ACTIVITY is not screenshot acceptance", data["scope"])

    def test_default_does_not_consume_and_option_requires_bool(self):
        document = capture([counter(screenshot.ADMIT, 1, 2)])
        self.assertNotIn("screenshot_requests", reporter.analyze_trace(document, [SERVICE]))
        self.path.write_text(json.dumps(document), encoding="utf-8")
        self.assertNotIn("screenshot_requests", reporter.analyze_trace_file(self.path, [SERVICE]))
        for value in (1, 0, "true", None):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "explicit bool"):
                reporter.analyze_trace(capture(), [SERVICE], screenshot_requests=value)

    def test_missing_unique_terminal_has_no_valid_prefix(self):
        document = capture([counter(screenshot.ADMIT, 1, 0)])
        self.reject(document)

    def test_missing_duplicate_orphan_and_malformed_writer_derived_edges_reject(self):
        document = capture(protocol_events(((1 << 53) + 7, (1 << 53) + 9,
                                            (1 << 63) + 1, (1 << 63) + 5,
                                            UINT64_MAX - 1, UINT64_MAX)))
        for name, changed in ledger_mutants(document):
            with self.subTest(mutation=name):
                self.reject(changed)

    def test_closed_identity_replay_and_decreasing_admissions_reject_without_history(self):
        for events in (
                [counter(screenshot.ADMIT, 3, 0), counter(screenshot.TERMINAL, 3, 0),
                 counter(screenshot.ADMIT, 3, 0), counter(screenshot.TERMINAL, 3, 0)],
                [counter(screenshot.ADMIT, 3, 0), counter(screenshot.ADMIT, 2, 1),
                 counter(screenshot.TERMINAL, 2, 1), counter(screenshot.TERMINAL, 3, 0)],
                [counter(screenshot.ADMIT, 3, 0), counter(screenshot.TERMINAL, 3, 0),
                 counter(screenshot.TERMINAL, 3, 0)]):
            self.reject(capture(events))

    def test_exact_uint64_and_no_integer_coercions(self):
        for field in ("id", "parent", "value"):
            for value in (True, False, 0.0, "0", None, -1, 1 << 64):
                events = [counter(screenshot.ADMIT, 1, 0), counter(screenshot.TERMINAL, 1, 0)]
                events[0]["args"][field] = value
                with self.subTest(field=field, value=value):
                    self.reject(capture(events))
        events = [counter(screenshot.ADMIT, 0, 0), counter(screenshot.TERMINAL, 0, 0)]
        self.reject(capture(events))
        identities = ((1 << 53) + 3, (1 << 53) + 5, (1 << 63) + 2,
                      (1 << 63) + 9, UINT64_MAX - 1, UINT64_MAX)
        data = self.both(capture(protocol_events(identities)))
        self.assertEqual(data["admission_serial_high_water"], str(UINT64_MAX))
        self.assertEqual(data["first"]["identity"], str(identities[1]))
        self.assertEqual(data["last"]["identity"], str(identities[4]))

    def test_per_identity_timestamp_order_not_global_cross_thread_sort(self):
        events = [counter(screenshot.ADMIT, 1, 0, tid=2, timestamp=100),
                  counter(screenshot.ADMIT, 2, 1, tid=2, timestamp=200),
                  counter(screenshot.TERMINAL, 1, 0, tid=3, timestamp=150),
                  counter(screenshot.TERMINAL, 2, 2, tid=3, timestamp=250)]
        self.assertEqual(self.both(capture(events))["terminal_count"], 2)
        events[2]["ts"] = 99
        self.reject(capture(events))

    def test_bounded_active_map_and_many_closed_requests_do_not_keep_history(self):
        with self.assertRaisesRegex(reporter.TraceFileError, "active limit"):
            self.file(capture(protocol_events()), max_active=1)
        events = []
        for identity in range(1, 501):
            events.extend((counter(screenshot.ADMIT, identity * 2, identity % 2),
                           counter(screenshot.TERMINAL, identity * 2, identity % 3)))
        data = self.file(capture(events), max_active=1, max_history=4)["screenshot_requests"]
        self.assertEqual((data["terminal_count"], data["peak_unclosed_admissions"]), (500, 1))
        for value in (0, -1, True, 1.0):
            with self.subTest(limit=value), self.assertRaises(ValueError):
                screenshot.ScreenshotRequestObservation(value)

    def test_complete_continuous_capture_and_existing_integrity_gates_are_required(self):
        for change in (dict(mode="resident"), dict(capture_complete=False), dict(write_ok=False),
                       dict(active_threads=1), dict(active_work_items=1), dict(active_waits=1),
                       dict(unexported=1), dict(capture_generation_stable=False),
                       dict(cursor_lost=1, lost=1), dict(sequence_exhausted=True)):
            document = capture(protocol_events())
            document["jceTrace"].update(change)
            with self.subTest(change=change):
                self.reject(document)
        document = capture(protocol_events())
        del document["jceTrace"]["capture_complete"]
        self.reject(document)

    def test_stream_chunks_cli_and_full_tail_invalidity(self):
        events = protocol_events()
        events.insert(6, counter(screenshot.CPU_PIXELS, 2, 65535, tid=4))
        document = capture(events)
        expected = self.file(document)["screenshot_requests"]
        for chunk in (1, 13, 1024):
            actual = reporter.analyze_trace_file(self.path, [SERVICE],
                screenshot_requests=True, chunk_bytes=chunk)["screenshot_requests"]
            self.assertEqual(actual, expected)
        output = Path(self.folder.name) / "summary.json"
        argv = ["lifecycle_trace_report", str(self.path), "--require", SERVICE,
                "--screenshot-requests", "--out", str(output)]
        with mock.patch.object(sys, "argv", argv), redirect_stdout(io.StringIO()):
            self.assertEqual(reporter.main(), 0)
        self.assertEqual(json.loads(output.read_text())["screenshot_requests"], expected)
        self.path.write_bytes(self.path.read_bytes() + b"INVALID_TAIL")
        with mock.patch.object(sys, "argv", argv), redirect_stdout(io.StringIO()):
            self.assertEqual(reporter.main(), 1)
        failed = json.loads(output.read_text())
        self.assertEqual(failed["status"], "INVALID_OR_INCOMPLETE_CAPTURE")
        self.assertFalse(failed["validation_complete"])
        self.assertEqual(failed["sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())

    def test_ordinary_old_unwired_owner_fails_unchanged_assertion(self):
        result, raw = old_unwired_control()
        self.assertEqual((result.testsRun, len(result.failures), len(result.errors)), (1, 1, 0))
        self.assertIn("AssertionError: ValueError not raised", raw)


def old_unwired_control(stream=None):
    source = Path(reporter.__file__).read_text(encoding="utf-8")
    needle = '            self.screenshots = ScreenshotRequestObservation(limits["active"] if limits else 8192)'
    if source.count(needle) != 1:
        raise AssertionError("unique canonical screenshot admission wire required")
    owner = types.ModuleType("tools.lifecycle_old_unwired_screenshot")
    owner.__package__ = "tools"
    exec(compile(source.replace(needle, "            self.screenshots = None", 1),
                 reporter.__file__, "exec"), owner.__dict__)
    output = stream or io.StringIO()
    unchanged = ScreenshotRequestTests("test_missing_unique_terminal_has_no_valid_prefix")
    with mock.patch.object(reporter, "_TraceAnalysis", owner._TraceAnalysis):
        result = unittest.TextTestRunner(stream=output, verbosity=2).run(unittest.TestSuite([unchanged]))
    return result, output.getvalue() if hasattr(output, "getvalue") else ""


def native_trace_control(path, stream):
    """Explicit same-test entry for the small preserved Native writer file.
    No application, compiler, second JSON reader or capture runner is started."""
    class NativeWriterTests(ScreenshotRequestTests):
        def test_real_core_writer_file_is_consumed_by_the_same_observer(self):
            report = reporter.analyze_trace_file(path, [SERVICE], screenshot_requests=True)
            data = report["screenshot_requests"]
            self.assertTrue(report["validation_complete"])
            self.assertEqual(data["status"], "CPU_SCREENSHOT_REQUEST_TERMINALS_OBSERVED")
            self.assertTrue(data["activity_observed"])
            self.assertEqual((data["admitted_count"], data["terminal_count"]), (6, 6))
            self.assertEqual(data["mode_counts"], dict(backbuffer=3, readback=3))
            self.assertEqual(data["outcome_counts"], dict(succeeded=2, failed=2, cancelled=2))
            self.assertEqual(data["admission_serial_high_water"], str(UINT64_MAX))
            self.assertEqual(data["peak_unclosed_admissions"], 6)
            document = json.loads(Path(path).read_text(encoding="utf-8"))
            self.assertEqual(data, reporter.analyze_trace(document, [SERVICE],
                                                          screenshot_requests=True)["screenshot_requests"])
            self.assertNotEqual(data["first"]["admit_thread"], data["first"]["terminal_thread"])

        def test_mutated_real_writer_file_edges_are_rejected(self):
            document = json.loads(Path(path).read_text(encoding="utf-8"))
            for name, changed in ledger_mutants(document):
                with self.subTest(mutation=name):
                    self.reject(changed)

    suite = unittest.TestSuite(NativeWriterTests(name) for name in (
        "test_real_core_writer_file_is_consumed_by_the_same_observer",
        "test_mutated_real_writer_file_edges_are_rejected"))
    return unittest.TextTestRunner(stream=stream, verbosity=2).run(suite)


if __name__ == "__main__":
    if sys.argv[1:] == ["--ordinary-old-unwired"]:
        result, _ = old_unwired_control(sys.stdout)
        raise SystemExit(0 if result.wasSuccessful() else 1)
    if len(sys.argv) == 3 and sys.argv[1] == "--native-trace":
        result = native_trace_control(Path(sys.argv[2]), sys.stdout)
        raise SystemExit(0 if result.wasSuccessful() else 1)
    unittest.main()
