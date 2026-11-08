"""Readback packet controls through the actual canonical memory/stream/CLI owner."""
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
from tools import readback_operation_observation as readback

CONTROL = "lifecycle.readback_control"


def packet(scope, stage, operation=1, timestamp=None, **changes):
    timestamp = timestamp if timestamp is not None else scope * 100 + 100
    values = dict(operation_lo=operation & readback.UINT32_MAX, operation_hi=operation >> 32,
        capture_linked=1, context_generation=7, ticket=8, stage=stage, ready_frame=10,
        observed_frame=10, observed_frame_known=1, readback_ready_observed=int(stage in (2, 4)),
        destination_released=int(stage in (4, 5)), backend_stopped=int(stage == 5), destination_bytes=4096)
    values.update(changes)
    base = dict(pid=1, tid=7)
    events = [dict(base, ph="i", cat="task", ts=timestamp-1, name=readback.SERVICE,
        args=dict(task=scope, parent=0, kind=2)),
        dict(base, ph="B", cat="task", ts=timestamp, name=readback.SERVICE,
             args=dict(id=scope, parent=scope))]
    for field in readback.FIELDS:
        events.append(dict(base, ph="C", cat="render", ts=timestamp+len(events),
            name=readback.PREFIX+field, args=dict(id=scope, parent=scope, value=values[field])))
    events.append(dict(base, ph="E", cat="task", ts=timestamp+len(events)+1, name=readback.SERVICE,
                       args=dict(id=scope, parent=scope, state=0, ns=1000)))
    return events


def capture(*packets):
    base = dict(pid=1, tid=1)
    events = [dict(base, ph="i", cat="task", ts=1, name=CONTROL, args=dict(task=1000000, parent=0, kind=2)),
        dict(base, ph="B", cat="task", ts=2, name=CONTROL, args=dict(id=1000000, parent=1000000)),
        dict(base, ph="E", cat="task", ts=3, name=CONTROL, args=dict(id=1000000, parent=1000000, state=0, ns=1000))]
    events += [event for group in packets for event in group]
    return dict(jceTrace=dict(recorded=len(events), captured=len(events), overwritten_before_capture=0,
        cursor_lost=0, lost=0, capture_generation_stable=True), traceEvents=events)


class ReadbackOperationTests(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[2]/"build/tests/readback-operation-observation"
        root.mkdir(parents=True, exist_ok=True)
        folder = tempfile.TemporaryDirectory(prefix="cpu-", dir=root)
        self.addCleanup(folder.cleanup)
        self.path = Path(folder.name)/"trace.json"

    def file(self, document, **kwargs):
        self.path.write_text(json.dumps(document, allow_nan=False), encoding="utf-8")
        return reporter.analyze_trace_file(self.path, [CONTROL], readback_operations=True, chunk_bytes=7, **kwargs)

    def memory(self, *packets):
        return reporter.analyze_trace(capture(*packets), [CONTROL], readback_operations=True)["readback_operations"]

    def test_memory_stream_actual_thirteen_fields_and_wide_identity_agree(self):
        identity = (1 << 63)+9
        document = capture(packet(identity, 1, identity, timestamp=100, destination_bytes=(1 << 53)+9),
            packet(2, 2, identity, timestamp=200, destination_bytes=(1 << 53)+9),
            packet(3, 4, identity, timestamp=300, destination_bytes=(1 << 53)+9))
        actual = self.file(document)
        data = actual["readback_operations"]
        self.assertEqual(data, reporter.analyze_trace(document, [CONTROL], readback_operations=True)["readback_operations"])
        self.assertEqual(data["packets_count"], 3)
        self.assertEqual(data["first"]["operation_id"], str(identity))
        self.assertEqual(data["first"]["scope_id"], str(identity))
        self.assertEqual(data["first"]["values"]["destination_bytes"], str((1 << 53)+9))
        self.assertEqual(list(data["first"]["field_raw_indices"]), list(readback.FIELDS))
        self.assertEqual(data["ordinary_destination_releases_count"], 1)
        self.assertEqual(data["outstanding_operations_count"], 0)
        self.assertIn("not global leaks", data["scope"])
        self.assertIn("GPU completion", data["scope"])
        self.assertTrue(actual["validation_complete"])
        self.assertEqual(actual["sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())

    def test_wrap_zero_max_and_half_range_readiness_only_at_actual_ready_stage(self):
        for promise, observed in ((0, 0), (readback.UINT32_MAX, readback.UINT32_MAX),
                                   (readback.UINT32_MAX, 0), (0, (1 << 31)-1)):
            with self.subTest(promise=promise, observed=observed):
                data = self.memory(packet(1, 1, ready_frame=promise, observed_frame=0),
                    packet(2, 2, ready_frame=promise, observed_frame=observed))
                self.assertEqual(data["linked_ready_operations_observed_count"], 1)
        for observed in ((1 << 31), readback.UINT32_MAX):
            with self.assertRaises(ValueError): self.memory(packet(1, 1, ready_frame=0), packet(2, 2, ready_frame=0, observed_frame=observed))
        data = self.memory(packet(1, 1, ready_frame=0), packet(2, 3, ready_frame=0, observed_frame=1 << 31))
        self.assertFalse(data["outstanding_operations"][0]["ready_observed"])
        self.assertTrue(data["outstanding_operations"][0]["cancel_intent_observed"])

    def test_backend_stop_can_release_unready_and_never_invents_ready(self):
        data = self.memory(packet(1, 1), packet(2, 3), packet(3, 5, observed_frame_known=0))
        self.assertEqual(data["backend_stopped_unready_releases_count"], 1)
        self.assertEqual(data["linked_ready_operations_observed_count"], 0)
        self.assertEqual(data["outstanding_operations_count"], 0)
        data = self.memory(packet(1, 1), packet(2, 2), packet(3, 5, readback_ready_observed=1))
        self.assertEqual(data["backend_stopped_unready_releases_count"], 0)
        for stage in (4, 5):
            with self.assertRaises(ValueError): self.memory(packet(1, 1), packet(2, stage, readback_ready_observed=1))
        with self.assertRaises(ValueError): self.memory(packet(1, 1), packet(2, 2), packet(3, 5))
        with self.assertRaises(ValueError): self.memory(packet(1, 1), packet(2, 2), packet(3, 2))

    def test_unlinked_capture_and_unknown_context_do_not_fabricate_submission(self):
        data = self.memory(packet(1, 2, 0, capture_linked=0, context_generation=-1),
                           packet(2, 5, 0, capture_linked=0, context_generation=-1))
        self.assertEqual(data["unlinked_packets_count"], 2)
        self.assertEqual(data["submitted_operations_count"], 0)
        self.assertEqual(data["linked_ready_operations_observed_count"], 0)
        self.assertEqual(data["first"]["join_status"], "UNAVAILABLE_UNLINKED_CAPTURE")
        data = self.memory(packet(1, 1, context_generation=-1), packet(2, 5, context_generation=-1))
        self.assertEqual(data["unknown_context_packets_count"], 2)
        self.assertEqual(data["submitted_operations_count"], 1)
        for changes in (dict(context_generation=0), dict(capture_linked=0), dict(operation_lo=0), dict(operation_lo=2)):
            with self.assertRaises(ValueError): self.memory(packet(1, 1, **changes))

    def test_context_ticket_and_extent_are_per_operation_and_release_is_once(self):
        data = self.memory(packet(1, 1), packet(2, 1, 2), packet(3, 2), packet(4, 5, 2))
        self.assertEqual(data["outstanding_operations_count"], 1)
        self.assertEqual(data["outstanding_operations"][0]["operation_id"], 1)
        self.assertTrue(data["outstanding_operations"][0]["ready_observed"])
        for changes in (dict(ticket=9), dict(context_generation=8), dict(ready_frame=11), dict(destination_bytes=8192)):
            with self.assertRaises(ValueError): self.memory(packet(1, 1), packet(2, 2, **changes))
        with self.assertRaises(ValueError): self.memory(packet(1, 2))
        with self.assertRaises(ValueError): self.memory(packet(1, 1), packet(2, 5), packet(3, 5))

    def test_missing_packet_field_is_refused(self):
        events = packet(1, 1); events.pop(5)
        with self.assertRaisesRegex(ValueError, "readback"):
            self.file(capture(events))

    def test_field_order_unknowns_types_ranges_owner_and_cpu_outcome_refused(self):
        for mode in ("duplicate", "reorder", "unknown", "owner", "thread", "category", "outcome"):
            events = packet(1, 1)
            if mode == "duplicate": events.insert(3, copy.deepcopy(events[2]))
            elif mode == "reorder": events[2], events[3] = events[3], events[2]
            elif mode == "unknown": events[2]["name"] = readback.PREFIX+"unknown"
            elif mode == "owner": events[2]["args"]["parent"] = 2
            elif mode == "thread": events[2]["tid"] = 9
            elif mode == "category": events[2]["cat"] = "core"
            else: events[-1]["args"]["state"] = 1
            with self.subTest(mode=mode), self.assertRaises(ValueError): self.file(capture(events))
        for field in readback.FIELDS:
            for bad in (True, 1.0, "1", None, -(1 << 63), 1 << 63):
                events = packet(1, 1); events[2+readback.FIELDS.index(field)]["args"]["value"] = bad
                with self.subTest(field=field, bad=bad), self.assertRaises(ValueError): self.file(capture(events))
        for stage in (0, 6):
            with self.assertRaises(ValueError): self.memory(packet(1, stage))
        for changes in (dict(destination_released=1), dict(backend_stopped=1)):
            with self.assertRaises(ValueError): self.memory(packet(1, 1, **changes))
        with self.assertRaises(ValueError): self.memory(packet(1, 1), packet(2, 2, observed_frame_known=0))

    def test_no_activity_defaults_bool_and_combined_pending_budget(self):
        document = capture()
        self.assertEqual(self.file(document)["readback_operations"]["status"], "NO_ACTIVITY")
        self.assertNotIn("readback_operations", reporter.analyze_trace(document, [CONTROL]))
        for bad in (1, 0, None, "true"):
            with self.assertRaises(ValueError): reporter.analyze_trace(document, [CONTROL], readback_operations=bad)
        self.assertEqual(self.file(capture(packet(1, 1)), max_active=1)["readback_operations"]["outstanding_operations_count"], 1)
        with self.assertRaisesRegex(ValueError, "combined"):
            self.file(capture(packet(1, 1), packet(2, 5)), max_active=1)
        self.assertEqual(self.file(capture(packet(1, 1), packet(2, 5)), max_active=2)["readback_operations"]["outstanding_operations_count"], 0)

    def test_canonical_eof_loss_generation_hash_and_real_cli_wires(self):
        document = capture(packet(1, 1), packet(2, 5))
        expected = self.file(document)["readback_operations"]
        for chunk in (1, 19, 4096):
            self.assertEqual(reporter.analyze_trace_file(self.path, [CONTROL], readback_operations=True, chunk_bytes=chunk)["readback_operations"], expected)
        for mode in ("loss", "generation", "count", "unclosed"):
            bad = copy.deepcopy(document)
            if mode == "loss": bad["jceTrace"].update(lost=1, cursor_lost=1)
            elif mode == "generation": bad["jceTrace"]["capture_generation_stable"] = False
            elif mode == "count": bad["jceTrace"]["recorded"] += 1
            else: bad["traceEvents"].pop(); bad["jceTrace"].update(recorded=len(bad["traceEvents"]), captured=len(bad["traceEvents"]))
            with self.assertRaises(ValueError): self.file(bad)
        self.file(document)
        out = self.path.parent/"out.json"
        argv = ["lifecycle_trace_report", str(self.path), "--require", CONTROL, "--readback-operations", "--out", str(out)]
        with mock.patch.object(sys, "argv", argv), redirect_stdout(io.StringIO()): self.assertEqual(reporter.main(), 0)
        self.assertEqual(json.loads(out.read_text())["readback_operations"], expected)
        self.path.write_bytes(json.dumps(document).encode()+b" trailing")
        with self.assertRaises(ValueError): reporter.analyze_trace_file(self.path, [CONTROL], readback_operations=True)

    def test_missing_wire_is_an_ordinary_unchanged_assertion_failure(self):
        result, raw = old_unwired_control()
        self.assertEqual((result.testsRun, len(result.failures), len(result.errors)), (1, 1, 0))
        self.assertIn("ValueError not raised", raw)


def old_unwired_control(stream=None):
    source = Path(reporter.__file__).read_text(encoding="utf-8")
    needle = '            self.readbacks = ReadbackOperationObservation(limits["active"] if limits else 8192)'
    if source.count(needle) != 1:
        raise AssertionError("unique canonical readback observer admission wire required")
    owner = types.ModuleType("tools.lifecycle_old_unwired_readback")
    owner.__package__ = "tools"
    exec(compile(source.replace(needle, "            self.readbacks = None", 1), reporter.__file__, "exec"), owner.__dict__)
    output = stream or io.StringIO()
    with mock.patch.object(reporter, "_TraceAnalysis", owner._TraceAnalysis):
        result = unittest.TextTestRunner(stream=output, verbosity=2).run(unittest.TestSuite([
            ReadbackOperationTests("test_missing_packet_field_is_refused")]))
    return result, output.getvalue() if hasattr(output, "getvalue") else ""


if __name__ == "__main__":
    if sys.argv[1:] == ["--ordinary-old-unwired"]:
        result, _ = old_unwired_control(sys.stdout)
        raise SystemExit(0 if result.wasSuccessful() else 1)
    unittest.main()
