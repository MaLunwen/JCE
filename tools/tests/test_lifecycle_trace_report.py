"""Incomplete, failed and malformed traces must not pass lifecycle acceptance."""
import copy
import unittest
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lifecycle_trace_report import analyze_trace


def capture():
    return {"jceTrace": {"recorded": 3, "captured": 3, "overwritten_before_capture": 0,
        "cursor_lost": 0, "lost": 0, "capture_generation_stable": True},
        "traceEvents": [
            {"ph": "i", "cat": "task", "pid": 1, "tid": 1, "ts": 900, "name": "app.init", "args": {"task": 1, "parent": 0, "kind": 2}},
            {"ph": "B", "cat": "task", "pid": 1, "tid": 1, "ts": 1000, "name": "app.init", "args": {"id": 1, "parent": 1}},
            {"ph": "E", "cat": "task", "pid": 1, "tid": 1, "ts": 2000, "name": "app.init", "args": {"id": 1, "parent": 1, "ns": 999000, "state": 0}}]}


class LifecycleTraceTests(unittest.TestCase):
    def test_completed_scope_has_measured_clock_and_explicit_limit(self):
        result = analyze_trace(capture(), ["app.init"])
        self.assertEqual(result["scopes"][0]["measured_ms"], .999)
        self.assertEqual(result["scopes"][0]["elapsed_ms"], 1)
        self.assertIn("not GPU", result["scope"])

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


if __name__ == "__main__": unittest.main()
