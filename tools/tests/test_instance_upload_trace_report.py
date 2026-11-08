#!/usr/bin/env python3
"""Synthetic positive and negative controls for the CPU instance-owner ledger."""
from __future__ import annotations
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
import uuid
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from instance_upload_trace_report import FIELDS, analyze_instance_upload_trace


class Trace:
    def __init__(self):
        self.events, self.next_id = [], 1

    def emit(self, name, phase, args, tid=7, category="task"):
        self.events.append(dict(name=name, ph=phase, pid=1, tid=tid,
                                cat=category, ts=1000 + len(self.events), args=args))

    def begin(self, name, parent=0, tid=7):
        scope = dict(id=self.next_id, name="renderer.instance." + name, tid=tid)
        self.next_id += 1
        self.emit(scope["name"], "i", dict(task=scope["id"], parent=parent, kind=2), tid)
        self.emit(scope["name"], "B", dict(id=scope["id"], parent=scope["id"], ns=0), tid)
        return scope

    def end(self, scope, state=0):
        self.emit(scope["name"], "E", dict(id=scope["id"], parent=scope["id"], ns=1, state=state), scope["tid"])

    def counter(self, scope, name, value, serial=None):
        self.emit("instance." + name, "C", dict(value=value, id=scope["id"] if serial is None else serial,
                  parent=scope["id"]), scope["tid"], "render")

    def identity(self, scope, packet):
        for field in FIELDS: self.counter(scope, field, packet[field], packet["serial"])

    def snapshot(self, scope, serial, held=0, backend=0, leases=0, peak=1, backend_peak=1):
        values = dict(traced_backend_held_bytes=held, backend_held_lease_count=backend,
                      staging_lease_count=leases, staging_lease_peak=peak,
                      backend_held_lease_peak=backend_peak, staging_slot_capacity=256,
                      staging_instrumented_scope=1)
        for name in ("staging_unavailable_total", "staging_state_oom_total", "staging_atomic_oom_total",
                     "staging_mutex_oom_total", "staging_pool_full_total", "staging_provider_gap_total"):
            values[name] = 0
        for name, value in values.items(): self.counter(scope, name, value, serial)

    def origin(self, scope, serial, origin):
        for name, value in dict(origin_span=origin, origin_capture_generation=10,
                               current_capture_generation=10, origin_same_capture=1,
                               staging_origin_capture_gap=0).items():
            self.counter(scope, name, value, serial)

    def resource(self, name, packet, parent=0, reason=0, state=0):
        scope = self.begin(name, parent)
        self.identity(scope, packet)
        self.counter(scope, "resource_capacity_bytes", packet["capacity"] * packet["stride"], packet["serial"])
        self.counter(scope, "resource_reason", reason, packet["serial"])
        self.end(scope, state)

    def reserve(self, packet, create=True):
        scope = self.begin("reserve")
        if create and packet["route_dynamic"]: self.resource("candidate_create", packet, scope["id"])
        # Same numeric scope ID and serial is legal. Generic counters may duplicate identity names.
        self.counter(scope, "requested", packet["requested"])
        self.counter(scope, "epoch_serial", packet["epoch_serial"])
        self.identity(scope, packet)
        self.counter(scope, "route_dynamic", packet["route_dynamic"])
        self.end(scope)

    def stage(self, kind, packet, origin, tid=7):
        scope = self.begin("staging_" + kind, origin, tid)
        self.origin(scope, packet["serial"], origin)
        self.identity(scope, dict(packet, owner_observed=0, is_main=int(tid == 7)))
        self.counter(scope, "staging_" + kind + "_bytes", packet["count"] * packet["stride"], packet["serial"])
        if kind == "transfer": self.counter(scope, "staging_submit_bytes", packet["actual"] * packet["stride"], packet["serial"])
        else: self.counter(scope, "staging_was_transferred", int(kind == "release"), packet["serial"])
        self.snapshot(scope, packet["serial"], packet["count"] * packet["stride"] if kind == "transfer" else 0,
                      int(kind == "transfer"), int(kind == "transfer"), backend_peak=int(kind != "cancel"))
        self.end(scope, 2 if kind == "cancel" else 0)

    def commit(self, packet, release=True, dynamic=True):
        scope = self.begin("upload")
        if dynamic:
            self.stage("transfer", packet, scope["id"])
            if release: self.stage("release", packet, scope["id"])
        self.identity(scope, packet)
        self.end(scope)
        return scope["id"]

    def bind(self, packet, actual=None, encoder=123):
        scope = self.begin("bind")
        self.identity(scope, dict(packet, actual=packet["actual"] if actual is None else actual, encoder_opaque=encoder))
        self.end(scope)

    def summary(self, packet, origin=0, full=True, tid=7, name="staging_summary", held=0, backend_peak=1):
        scope = self.begin(name, origin, tid)
        zero = {field: 0 for field in FIELDS}
        zero.update(token_generation=packet["token_generation"], owner_generation=packet["token_generation"],
                    handle=65535, is_main=int(tid == 7))
        if full: self.origin(scope, 0, origin)
        self.identity(scope, zero)
        if full: self.snapshot(scope, 0, held, int(held > 0), int(held > 0), backend_peak=backend_peak)
        else:
            for name in ("staging_unavailable_total", "staging_state_oom_total", "staging_atomic_oom_total",
                         "staging_mutex_oom_total", "staging_pool_full_total", "staging_provider_gap_total"):
                self.counter(scope, name, 0, 0)
            self.counter(scope, "staging_instrumented_scope", 1, 0)
        self.end(scope)

    def destroy(self, packet, origin=0, full=True, summary=True, resource=True, backend_peak=1):
        scope = self.begin("destroy")
        if resource: self.resource("handle_destroy", packet, scope["id"], 3)
        self.counter(scope, "generation", packet["token_generation"])
        self.counter(scope, "logical_owned_bytes", 0)
        if summary: self.summary(packet, origin, full, backend_peak=backend_peak)
        else: self.summary(packet, origin, True, name="staging_detach", held=packet["count"] * packet["stride"])
        self.end(scope)

    def document(self):
        return dict(traceEvents=self.events, jceTrace=dict(mode="continuous", consumer_mode="threaded",
                    worker_available=True, recorded=len(self.events), captured=len(self.events),
                    overwritten_before_capture=0, cursor_lost=0, lost=0, unexported=0,
                    capture_complete=True, write_ok=True, capture_generation_stable=True,
                    active_threads=0, active_work_items=0, active_waits=0, gpu_completion_observed=False))


def packet(**changes):
    values = {field: 0 for field in FIELDS}
    values.update(owner_observed=1, owner_generation=11, owner_epoch=100, owner_epoch_serial=1,
                  token_generation=11, serial=1, allocation_serial=1, epoch=100, epoch_serial=1,
                  handle=0, requested=4, count=4, capacity=4, stride=64, route_dynamic=1, is_main=1)
    values.update(changes)
    return values


def fixture(dynamic=True, unbound=False, late=False):
    trace = Trace()
    root = packet() if dynamic else packet(route_dynamic=0, handle=9, transient_num=4, transient_size=256, transient_stride=64)
    trace.reserve(root)
    accepted = dict(root, actual=2)
    origin = trace.commit(accepted, not late, dynamic)
    if not unbound: trace.bind(accepted, 1)
    trace.destroy(accepted, origin, dynamic, not late, dynamic)
    if late:
        trace.stage("release", accepted, origin, 19)
        trace.summary(accepted, origin, True, 19)
    return trace.document()


def remove_operation(doc, name):
    ids = {e["args"]["id"] for e in doc["traceEvents"] if e["ph"] == "B" and e["name"] == "renderer.instance." + name}
    doc["traceEvents"][:] = [e for e in doc["traceEvents"] if not ((e["ph"] == "i" and e["args"].get("task") in ids)
        or (e["ph"] in ("B", "E", "C") and e["args"].get("parent") in ids))]


def failed_update_fixture():
    t = Trace(); p = packet(); t.reserve(p); a = dict(p, actual=2); upload = t.begin("upload")
    t.stage("cancel", a, upload["id"])
    t.identity(upload, p); t.counter(upload, "attempted_actual", 2, p["serial"])
    discard = t.begin("discard", upload["id"]); t.identity(discard, p)
    free = t.begin("staging_cpu_free", discard["id"]); t.identity(free, p); t.counter(free, "staging_free_bytes", 256, 1); t.end(free, 2)
    t.resource("handle_destroy", p, discard["id"], 4); t.end(discard, 2); t.end(upload, 1)
    t.destroy(p, upload["id"], resource=False, backend_peak=0)
    return t.document()


def adjust_counts(doc):
    doc["jceTrace"].update(recorded=len(doc["traceEvents"]), captured=len(doc["traceEvents"]))


def counter(doc, operation, name, last=False):
    scopes = {e["args"]["id"] for e in doc["traceEvents"] if e["ph"] == "B" and e["name"] == "renderer.instance." + operation}
    matches = [e for e in doc["traceEvents"] if e["ph"] == "C" and e["name"] == "instance." + name and e["args"]["parent"] in scopes]
    return matches[-1 if last else 0]


class LedgerTests(unittest.TestCase):
    def reject(self, mutate, match=None, source=None):
        doc = copy.deepcopy(source or fixture()); mutate(doc); adjust_counts(doc)
        with self.assertRaises(ValueError) as caught: analyze_instance_upload_trace(doc)
        if match: self.assertIn(match, str(caught.exception))

    def test_closed_dynamic_and_unbound_are_distinct(self):
        report = analyze_instance_upload_trace(fixture(), True)
        self.assertEqual(report["dynamic"], "EXERCISED_CLOSED")
        self.assertEqual(report["tokens"], dict(reserved=1, committed=1, cancelled=0, binds=1, committed_unbound=0))
        self.assertEqual(report["resources"]["destroy_requested"], 1)
        self.assertEqual(report["staging"]["retained_transfer_bytes"], 256)
        self.assertEqual(analyze_instance_upload_trace(fixture(unbound=True))["tokens"]["committed_unbound"], 1)

    def test_transient_does_not_forge_dynamic_evidence(self):
        doc = fixture(False)
        self.assertEqual(analyze_instance_upload_trace(doc)["dynamic"], "NOT_EXERCISED")
        with self.assertRaisesRegex(ValueError, "NOT_EXERCISED"): analyze_instance_upload_trace(doc, True)

    def test_late_foreign_callback_after_owner_destroy(self):
        report = analyze_instance_upload_trace(fixture(late=True), True)
        self.assertEqual(report["staging"]["transferred"], 1)

    def test_global_timestamps_may_interleave(self):
        doc = fixture(late=True)
        for event in doc["traceEvents"]:
            if event["tid"] == 19: event["ts"] -= 100
        self.assertEqual(analyze_instance_upload_trace(doc, True)["dynamic"], "EXERCISED_CLOSED")

    def test_id_collision_and_generic_duplicates_preserve_identity(self):
        doc = fixture()
        e = counter(doc, "reserve", "route_dynamic", True)
        self.assertEqual(e["args"]["id"], 1)
        e["args"]["value"] = -1
        self.assertEqual(analyze_instance_upload_trace(doc)["tokens"]["reserved"], 1)

    def test_missing_each_identity_field(self):
        for name in FIELDS:
            with self.subTest(name=name):
                self.reject(lambda d: d["traceEvents"].remove(counter(d, "bind", name)))

    def test_bad_counter_value_types(self):
        for value in (True, "1", 1.0, float("nan"), float("inf"), -1):
            with self.subTest(value=value):
                self.reject(lambda d: counter(d, "bind", "count")["args"].update(value=value))

    def test_bad_counter_ownership(self):
        for change in (dict(parent=999999), dict(id=True)):
            with self.subTest(change=change):
                self.reject(lambda d: counter(d, "bind", "count")["args"].update(change))
        self.reject(lambda d: counter(d, "bind", "count").update(tid=999))

    def test_immutable_mismatch(self):
        for name in ("token_generation", "allocation_serial", "epoch_serial", "handle", "offset", "requested", "capacity", "stride", "fallback_reason"):
            with self.subTest(name=name):
                self.reject(lambda d: counter(d, "bind", name)["args"].update(value=counter(d, "bind", name)["args"]["value"] + 1))

    def test_bind_overcount_and_missing_commit(self):
        self.reject(lambda d: counter(d, "bind", "actual")["args"].update(value=3), "exceeds")
        self.reject(lambda d: d["traceEvents"].remove(counter(d, "upload", "owner_observed")))

    def test_missing_destroy_and_duplicate_destroy(self):
        def remove(d):
            scopes = {e["args"]["id"] for e in d["traceEvents"] if e["ph"] == "B" and e["name"].endswith(".handle_destroy")}
            d["traceEvents"][:] = [e for e in d["traceEvents"] if not ((e["ph"] == "i" and e["args"].get("task") in scopes) or (e["ph"] != "i" and e["args"].get("parent") in scopes))]
        self.reject(remove, "missing destroy")
        t = Trace(); p = packet(); t.reserve(p); a = dict(p, actual=2); origin = t.commit(a)
        t.resource("handle_destroy", a, reason=2); t.resource("handle_destroy", a, reason=2); t.destroy(a, origin, resource=False)
        with self.assertRaisesRegex(ValueError, "double"): analyze_instance_upload_trace(t.document())

    def test_double_release_and_reversed_release(self):
        t = Trace(); p = packet(); t.reserve(p); a = dict(p, actual=2); origin = t.commit(a)
        t.stage("release", a, origin); t.destroy(a, origin)
        with self.assertRaisesRegex(ValueError, "double"): analyze_instance_upload_trace(t.document())
        self.reject(lambda d: counter(d, "staging_release", "origin_span")["args"].update(value=1))

    def test_missing_staging_metrics(self):
        for op, name in (("staging_transfer", "staging_submit_bytes"), ("staging_release", "staging_release_bytes"),
                         ("staging_release", "staging_was_transferred"), ("staging_summary", "staging_lease_count"),
                         ("staging_transfer", "backend_held_lease_peak")):
            with self.subTest(op=op, name=name): self.reject(lambda d: d["traceEvents"].remove(counter(d, op, name)))

    def test_staging_bytes_identity_and_origin_guards(self):
        for op, name, value in (("staging_transfer", "staging_submit_bytes", 129), ("staging_release", "staging_release_bytes", 128),
                                ("staging_release", "actual", 1), ("staging_release", "origin_same_capture", 0),
                                ("staging_release", "current_capture_generation", 11), ("staging_release", "staging_origin_capture_gap", 1)):
            with self.subTest(op=op, name=name): self.reject(lambda d: counter(d, op, name)["args"].update(value=value))

    def test_unavailable_and_final_nonzero(self):
        for name in ("staging_unavailable_total", "staging_pool_full_total", "staging_atomic_oom_total", "staging_mutex_oom_total", "staging_provider_gap_total"):
            with self.subTest(name=name): self.reject(lambda d: counter(d, "staging_summary", name)["args"].update(value=1))
        for op, name in (("staging_summary", "traced_backend_held_bytes"), ("staging_summary", "backend_held_lease_count"),
                         ("staging_summary", "staging_lease_count"), ("destroy", "logical_owned_bytes")):
            with self.subTest(op=op, name=name): self.reject(lambda d: counter(d, op, name)["args"].update(value=1))

    def test_footer_loss_missing_and_types(self):
        for name, value in (("lost", 1), ("captured", True), ("capture_complete", False), ("active_threads", 1), ("capture_generation_stable", False)):
            with self.subTest(name=name):
                doc = fixture(); doc["jceTrace"][name] = value
                with self.assertRaises(ValueError): analyze_instance_upload_trace(doc)
        doc = fixture(); del doc["jceTrace"]["cursor_lost"]
        with self.assertRaises(ValueError): analyze_instance_upload_trace(doc)

    def test_same_handle_reuse_across_allocations_and_contexts(self):
        t = Trace(); p = packet(); t.reserve(p); a = dict(p, actual=2); origin = t.commit(a)
        t.resource("handle_destroy", a, reason=2)
        q = packet(serial=2, allocation_serial=2, epoch=101, owner_epoch=101, epoch_serial=2, owner_epoch_serial=2)
        t.reserve(q); b = dict(q, actual=2); t.commit(b); t.bind(b); t.destroy(b, origin)
        report = analyze_instance_upload_trace(t.document(), True)
        self.assertEqual(report["resources"]["created"], 2)
        u = packet(token_generation=12, owner_generation=12); t.reserve(u); c = dict(u, actual=2); second_origin = t.commit(c); t.destroy(c, second_origin)
        self.assertEqual(analyze_instance_upload_trace(t.document(), True)["resources"]["created"], 3)

    def test_retirement_and_original_allocation_identity(self):
        t = Trace(); p = packet(); t.reserve(p); a = dict(p, actual=2); origin = t.commit(a)
        q = packet(serial=2, allocation_serial=2, handle=1, requested=3, count=3, offset=2, capacity=8)
        t.reserve(q); b = dict(q, actual=3); t.commit(b)
        t.resource("retire_enqueue", a); t.resource("handle_destroy", a, reason=1); t.destroy(b, origin)
        report = analyze_instance_upload_trace(t.document(), True)
        self.assertEqual(report["resources"]["retired"], 1)

    def test_failed_update_separates_metadata_cancel_and_cpu_free(self):
        report = analyze_instance_upload_trace(failed_update_fixture())
        self.assertEqual(report["staging"]["metadata_cancelled"], 1)
        self.assertEqual(report["staging"]["cpu_freed"], 1)
        self.assertEqual(report["dynamic"], "NOT_EXERCISED")

    def test_multiple_tokens_one_allocation_and_many_encoders(self):
        t = Trace(); p = packet(); t.reserve(p); a = dict(p, actual=2); origin = t.commit(a)
        q = packet(serial=2, allocation_serial=1, count=2, requested=2, offset=2)
        t.reserve(q, create=False); b = dict(q, actual=1); t.commit(b)
        t.bind(a, 2, 18); t.bind(a, 1, 19); t.bind(b, 1, 18); t.destroy(a, origin)
        report = analyze_instance_upload_trace(t.document(), True)
        self.assertEqual(report["resources"]["created"], 1)
        self.assertEqual(report["tokens"]["committed"], 2)
        self.assertEqual(report["tokens"]["binds"], 3)

    def test_failed_creation_then_transient_recovery(self):
        t = Trace(); reserve = t.begin("reserve"); failed = packet(handle=65535, count=8, requested=8, capacity=8)
        t.resource("candidate_create", failed, reserve["id"], 6, 1)
        free = t.begin("staging_cpu_free", reserve["id"]); t.identity(free, failed)
        t.counter(free, "staging_free_bytes", 512, 1); t.end(free, 2)
        recovered = packet(route_dynamic=0, handle=9, count=2, requested=8, capacity=2,
                           transient_num=2, transient_size=512, transient_stride=64, fallback_reason=7)
        t.identity(reserve, recovered); t.counter(reserve, "route_dynamic", 0); t.end(reserve)
        accepted = dict(recovered, actual=2); t.commit(accepted, dynamic=False); t.bind(accepted)
        t.destroy(accepted, full=False, resource=False)
        doc = t.document(); report = analyze_instance_upload_trace(doc)
        self.assertEqual(report["dynamic"], "NOT_EXERCISED")
        self.assertEqual(report["staging"]["cpu_freed"], 1)
        self.reject(lambda d: remove_operation(d, "staging_cpu_free"), "disposal", doc)

    def test_orphan_allocation_and_removed_creation(self):
        t = Trace(); p = packet(); t.resource("candidate_create", p); t.destroy(p, full=False)
        with self.assertRaisesRegex(ValueError, "no successful reserve"): analyze_instance_upload_trace(t.document())
        self.reject(lambda d: remove_operation(d, "candidate_create"), "absent")

    def test_reverse_and_missing_backend_release(self):
        t = Trace(); p = packet(); t.reserve(p); upload = t.begin("upload"); accepted = dict(p, actual=2)
        t.stage("release", accepted, upload["id"]); t.stage("transfer", accepted, upload["id"])
        t.identity(upload, accepted); t.end(upload); t.destroy(accepted, upload["id"])
        with self.assertRaisesRegex(ValueError, "reversed"): analyze_instance_upload_trace(t.document())
        self.reject(lambda d: remove_operation(d, "staging_release"), "closed backend")

    def test_failed_update_requires_metadata_cancel_and_heap_disposal(self):
        doc = failed_update_fixture()
        self.reject(lambda d: remove_operation(d, "staging_cancel"), "metadata cancellation", doc)
        self.reject(lambda d: remove_operation(d, "staging_cpu_free"), "CPU free", doc)
        self.reject(lambda d: d["traceEvents"].remove(counter(d, "upload", "attempted_actual")), "attempted_actual", doc)

    def test_unknown_operation_huge_identity_and_summary_origin_mismatch(self):
        self.reject(lambda d: counter(d, "bind", "count")["args"].update(value=1 << 40))
        self.reject(lambda d: counter(d, "staging_summary", "current_capture_generation")["args"].update(value=11))
        def change(d):
            for e in d["traceEvents"]:
                if e["name"] == "renderer.instance.bind": e["name"] = "renderer.instance.unknown"
        self.reject(change, "unknown instance operation")

    def test_zero_token_and_allocation_serials_are_never_admitted(self):
        def clear(d):
            for e in d["traceEvents"]:
                if e["ph"] != "C": continue
                if e["args"]["id"] == 1: e["args"]["id"] = 0
                if e["name"] in ("instance.serial", "instance.allocation_serial"): e["args"]["value"] = 0
        self.reject(clear)

    def test_actual_committed_intervals_reject_consistent_overlap_metadata(self):
        t = Trace(); p = packet(); t.reserve(p); a = dict(p, actual=2); origin = t.commit(a)
        q = packet(serial=2, allocation_serial=1, count=2, requested=2, offset=2)
        t.reserve(q, create=False); b = dict(q, actual=1); t.commit(b); t.bind(a); t.bind(b); t.destroy(a, origin)
        doc = t.document()
        self.assertEqual(analyze_instance_upload_trace(doc, True)["tokens"]["committed"], 2)
        def overlap(d):
            for e in d["traceEvents"]:
                if e["ph"] == "C" and e["name"] == "instance.offset" and e["args"]["id"] == 2: e["args"]["value"] = 0
        self.reject(overlap, "overlapping committed", doc)

    def test_new_epoch_reuses_same_allocation_interval(self):
        t = Trace(); p = packet(); t.reserve(p); a = dict(p, actual=2); origin = t.commit(a)
        q = packet(serial=2, allocation_serial=1, epoch=101, owner_epoch=101, epoch_serial=2, owner_epoch_serial=2)
        t.reserve(q, create=False); b = dict(q, actual=2); t.commit(b); t.bind(b); t.destroy(a, origin)
        self.assertEqual(analyze_instance_upload_trace(t.document(), True)["tokens"]["committed"], 2)

    def test_advance_requires_retirement_or_exact_pending_cancel_parent(self):
        self.reject(lambda d: counter(d, "handle_destroy", "resource_reason")["args"].update(value=1), "unretired")
        t = Trace(); p = packet(); t.reserve(p); retire = t.begin("retire"); cancel = t.begin("implicit_cancel", retire["id"])
        t.identity(cancel, p); t.counter(cancel, "resource_reason", 1, p["serial"])
        free = t.begin("staging_cpu_free", cancel["id"]); t.identity(free, p); t.counter(free, "staging_free_bytes", 256, 1); t.end(free, 2)
        t.resource("handle_destroy", p, cancel["id"], 1); t.end(cancel, 2); t.end(retire)
        t.destroy(p, full=False, resource=False)
        doc = t.document(); report = analyze_instance_upload_trace(doc)
        self.assertEqual(report["tokens"]["cancelled"], 1)
        self.assertEqual(report["resources"]["destroy_requested"], 1)
        def bad_parent(d):
            for e in d["traceEvents"]:
                if e["ph"] == "i" and e["name"] == "renderer.instance.handle_destroy": e["args"]["parent"] = 0
        self.reject(bad_parent, "pending advance cancellation", doc)

    def test_unused_owner_no_dynamic_state_stays_unexercised(self):
        t = Trace(); t.destroy(packet(), full=False, resource=False)
        report = analyze_instance_upload_trace(t.document())
        self.assertEqual(report["dynamic"], "NOT_EXERCISED")
        self.assertEqual(report["tokens"]["reserved"], 0)
        self.assertEqual(report["status"], "INSTANCE_OWNER_UPLOADS_NOT_EXERCISED")

    def test_cli_status_exact_hash_and_negative_exit(self):
        destination = os.environ.get("JCE_TRACE_REPORT_TEST_DIR")
        if not destination: self.skipTest("explicit evidence output directory required for CLI artifacts")
        folder = Path(destination) / ("cli-" + uuid.uuid4().hex); folder.mkdir(parents=True)
        path, out = folder / "trace.json", folder / "report.json"
        raw = (json.dumps(fixture()) + "\n").encode(); path.write_bytes(raw)
        command = [sys.executable, "-B", str(Path(__file__).resolve().parents[1] / "instance_upload_trace_report.py"), str(path), "--out", str(out), "--require-dynamic"]
        good = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(good.returncode, 0, good.stderr)
        self.assertEqual(json.loads(out.read_text())["sha256"], hashlib.sha256(raw).hexdigest())
        path.write_text("{broken", encoding="utf-8"); bad = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(bad.returncode, 1)
        self.assertEqual(json.loads(out.read_text())["status"], "INVALID_OR_INCOMPLETE_INSTANCE_CAPTURE")
        empty = Trace(); empty.destroy(packet(), full=False, resource=False)
        path.write_text(json.dumps(empty.document()), encoding="utf-8")
        unexercised = subprocess.run(command[:-1], capture_output=True, text=True, check=False)
        self.assertEqual(unexercised.returncode, 2, unexercised.stderr)
        self.assertEqual(json.loads(out.read_text())["status"], "INSTANCE_OWNER_UPLOADS_NOT_EXERCISED")
        self.assertNotEqual(unexercised.returncode, good.returncode)
        (folder / "command-results.json").write_text(json.dumps(dict(command=command, positive_exit=good.returncode, negative_exit=bad.returncode,
                                                                   not_exercised_exit=unexercised.returncode)) + "\n")


if __name__ == "__main__":
    unittest.main(verbosity=2)
