"""Synthetic typed shader receipts through the canonical memory/file/CLI owner."""
import builtins
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools import lifecycle_trace_report as reporter
from tools import shader_provenance_observation as shaders


def packet(service, identity, timestamp, values=None, *, parent=77, thread=7, state=0):
    base = dict(pid=1, tid=thread)
    events = [dict(base, ph="i", cat="task", ts=timestamp, name=service,
        args=dict(task=identity, parent=parent, kind=2)),
        dict(base, ph="B", cat="task", ts=timestamp + 1, name=service,
        args=dict(id=identity, parent=identity))]
    events.extend(dict(base, ph="C", cat="render", ts=timestamp + 2 + n, name=field,
        args=dict(id=identity, parent=parent, value=value)) for n, (field, value) in enumerate((values or {}).items()))
    events.append(dict(base, ph="E", cat="task", ts=timestamp + 30, name=service,
        args=dict(id=identity, parent=identity, ns=29000, state=state)))
    return events


def create(identity=10, handle=5, timestamp=100, *, parent=77, thread=7, capture=3, context=9, payload=True, changes=None):
    digest = hashlib.sha256(b"a known shader container\x00").digest()
    values = dict(capture=capture, context_known=int(bool(context)), context_hi=context >> 32,
        context_lo=context & shaders.UINT32_MAX, bytes=25 if payload else 0, payload_known=int(payload))
    values.update({"sha256." + str(n): int.from_bytes(digest[4*n:4*n+4], "big") if payload else 0 for n in range(8)})
    values.update(handle=handle, cpu_returned=1)
    values.update(changes or {})
    return packet(shaders.CREATE, identity, timestamp,
        {"shader.create." + key: value for key, value in values.items()}, parent=parent, thread=thread,
        state=int(handle == shaders.INVALID_HANDLE))


def release(identity=40, create_id=10, handle=5, timestamp=300, *, reason=0, capture=3, parent=77, thread=7):
    values = dict(capture=capture, create_hi=create_id >> 32, create_lo=create_id & shaders.UINT32_MAX,
        reason=reason, handle=handle, cpu_returned=1)
    return packet(shaders.RELEASE, identity, timestamp,
        {"shader.release." + key: value for key, value in values.items()}, parent=parent, thread=thread)


def link(identity=30, vs_id=10, fs_id=11, timestamp=300, *, vs=5, fs=6, program=8,
         vs_reason=0, fs_reason=0, capture=3, parent=77, thread=7):
    success = program != shaders.INVALID_HANDLE
    state = int(not success)
    values = dict(capture=capture, vs_create_hi=vs_id >> 32, vs_create_lo=vs_id & shaders.UINT32_MAX,
        fs_create_hi=fs_id >> 32, fs_create_lo=fs_id & shaders.UINT32_MAX,
        vs_reason=vs_reason, fs_reason=fs_reason, cpu_returned=1)
    outer = packet(shaders.LOAD_LINK, identity, timestamp,
        {"shader.owner." + key: value for key, value in values.items()}, parent=parent, thread=thread, state=state)
    stages = vs != shaders.INVALID_HANDLE and fs != shaders.INVALID_HANDLE
    fields = dict(vs=vs, fs=fs, program=program,
        creator_releases=int(vs != shaders.INVALID_HANDLE) + int(fs != shaders.INVALID_HANDLE),
        success=int(success), rejection=0 if success else 2 if stages else 1)
    child = packet(shaders.LINK, identity + 1, timestamp + 3,
        {"shader.link." + key: value for key, value in fields.items()}, parent=identity, thread=thread, state=state)
    for event in outer[2:]:
        event["ts"] += 40
    return outer[:2] + child + outer[2:]


def capture(*groups, thread=7):
    events = packet("app.init", 1, 1, parent=0, thread=thread)
    events += [event for group in groups for event in group]
    return dict(jceTrace=dict(recorded=len(events), captured=len(events), lost=0,
        overwritten_before_capture=0, cursor_lost=0, capture_generation_stable=True), traceEvents=events)


def success(**options):
    return capture(create(), create(11, 6, 200), link(**options))


class ShaderProvenanceTests(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[2] / "build/tests/shader-provenance-observation"
        root.mkdir(parents=True, exist_ok=True)
        self.folder = tempfile.TemporaryDirectory(prefix="cpu-", dir=root)
        self.addCleanup(self.folder.cleanup)
        self.path = Path(self.folder.name) / "trace.json"

    def memory(self, document, **options):
        return reporter.analyze_trace(document, ["app.init"], shader_provenance=True, **options)["shader_provenance"]

    def file(self, document, **options):
        self.path.write_text(json.dumps(document, allow_nan=False), encoding="utf-8")
        return reporter.analyze_trace_file(self.path, ["app.init"], shader_provenance=True,
            chunk_bytes=options.pop("chunk_bytes", 7), **options)["shader_provenance"]

    def test_complete_buffer_create_link_creator_release_preserves_exact_digest(self):
        data = self.file(success())
        self.assertEqual(data, self.memory(success()))
        self.assertEqual(data["creator_receipt_coverage"], "COMPLETE_CAPTURED_CPU_RECEIPTS")
        self.assertEqual((data["consumed_create_receipt_count"], data["complete_create_link_creator_release_count"]), (2, 1))
        row = data["services"][shaders.LOAD_LINK]["first_complete"]
        self.assertEqual(row["link"]["creator_releases"], 2)
        self.assertEqual((row["link"]["vs"], row["link"]["fs"], row["link"]["program"]), (5, 6, 8))
        self.assertEqual(row["stages"]["vs"]["create"]["sha256"], hashlib.sha256(b"a known shader container\x00").hexdigest())
        self.assertEqual(row["stages"]["vs"]["create"]["buffer_bytes"], 25)

    def test_wide_ids_context_and_same_handle_acquisitions_remain_distinct(self):
        wide, context = (1 << 60) + 7, (1 << 63) + 13
        doc = capture(create(wide, 5, context=context, parent=wide + 10),
            create(wide + 1, 5, 200, context=context, parent=wide + 10),
            link(wide + 2, wide, wide + 1, vs=5, fs=5, parent=wide + 10))
        data = self.file(doc)
        row = data["services"][shaders.LOAD_LINK]["last_complete"]
        self.assertEqual(row["scope_id"], str(wide + 2))
        self.assertEqual(row["submitted_parent"], str(wide + 10))
        self.assertEqual(row["stages"]["vs"]["create_id"], str(wide))
        self.assertEqual(row["stages"]["fs"]["create_id"], str(wide + 1))
        self.assertEqual(row["stages"]["vs"]["create"]["context_generation"], str(context))
        self.assertEqual(data["consumed_create_receipt_count"], 2)
        doc = capture(create(), release(), create(11, 5, 400), release(41, 11, 5, 500))
        self.assertEqual(self.file(doc)["complete_create_creator_release_count"], 2)

    def test_failed_create_and_vs_cleanup_after_fs_failure_are_legal_cpu_facts(self):
        data = self.file(capture(create(), create(11, shaders.INVALID_HANDLE, 200), release()))
        self.assertEqual((data["failed_create_count"], data["complete_create_creator_release_count"]), (1, 1))
        self.assertEqual(data["unresolved_successful_create_count"], 0)
        data = self.file(success(program=shaders.INVALID_HANDLE))
        self.assertEqual(data["complete_create_link_creator_release_count"], 1)
        self.assertEqual(data["services"][shaders.LOAD_LINK]["outcomes"]["failed"], 1)

    def test_unavailable_gaps_do_not_consume_or_certify_successful_receipts(self):
        for reason in range(1, 5):
            data = self.file(capture(create(), release(create_id=0, reason=reason)))
            self.assertEqual((data["provenance_gap_count"], data["unresolved_successful_create_count"]), (1, 1))
            self.assertEqual(data["creator_receipt_coverage"], "UNRESOLVED_OR_UNAVAILABLE")
            data = self.file(capture(link(vs_id=0, fs_id=0, vs_reason=reason, fs_reason=reason)))
            self.assertEqual(data["complete_create_link_creator_release_count"], 0)
            self.assertEqual(data["provenance_gap_count"], 2)
        for reason, identity in ((0, 0), (1, 10), (2, 10), (3, 10), (4, 10)):
            with self.subTest(reason=reason), self.assertRaises(ValueError):
                self.file(capture(create(), release(create_id=identity, reason=reason)))
        data = self.file(capture(create(context=0), release(create_id=0, reason=4)))
        self.assertIsNone(data["services"][shaders.CREATE]["first_complete"]["context_generation"])

    def test_absent_packets_and_standalone_shared_link_stay_unavailable(self):
        data = self.file(capture())
        self.assertEqual(data["status"], "UNAVAILABLE")
        data = self.file(capture(packet(shaders.CREATE, 10, 100)))
        self.assertEqual(data["status"], "UNAVAILABLE")
        self.assertEqual(data["services"][shaders.CREATE]["unavailable"], 1)
        shared = link()[2:11]
        for event in shared:
            if event["ph"] == "i": event["args"]["parent"] = 77
            elif event["ph"] == "C": event["args"]["parent"] = 77
        data = self.file(capture(shared))
        self.assertEqual(data["status"], "UNAVAILABLE")
        self.assertEqual(data["unbound_shared_link_count"], 1)
        data = self.file(capture(create()))
        self.assertEqual(data["unresolved_successful_create_count"], 1)
        self.assertNotEqual(data["creator_receipt_coverage"], "COMPLETE_CAPTURED_CPU_RECEIPTS")

    def test_missing_duplicate_out_of_order_unknown_and_mistyped_fields_reject(self):
        for service in shaders.SCHEMAS:
            original = success()["traceEvents"]
            offsets = [n for n, event in enumerate(original) if event["ph"] == "C" and
                event["name"] in shaders.SCHEMAS[service]]
            if service == shaders.RELEASE:
                original = capture(create(), release())["traceEvents"]
                offsets = [n for n, event in enumerate(original) if event["ph"] == "C" and event["name"].startswith("shader.release.")]
            for offset in offsets:
                for mode in ("missing", "duplicate", "order"):
                    events = copy.deepcopy(original)
                    if mode == "missing": events.pop(offset)
                    elif mode == "duplicate": events.insert(offset, copy.deepcopy(events[offset]))
                    elif offset != offsets[-1]: events[offset], events[offset + 1] = events[offset + 1], events[offset]
                    else: continue
                    with self.subTest(service=service, offset=offset, mode=mode), self.assertRaises(ValueError):
                        self.file(capture(events[3:]))
        for name, value in (("shader.create.sha256.0", -1), ("shader.create.sha256.7", 1 << 32),
                ("shader.create.handle", 1 << 16), ("shader.create.context_known", 2),
                ("shader.create.payload_known", True), ("shader.create.bytes", 1.0),
                ("shader.owner.vs_reason", 5), ("shader.owner.cpu_returned", 0),
                ("shader.link.creator_releases", 3), ("shader.link.success", "1"),
                ("shader.release.reason", 5)):
            doc = capture(create(), release()) if name.startswith("shader.release.") else success()
            next(event for event in doc["traceEvents"] if event["name"] == name)["args"]["value"] = value
            with self.subTest(name=name, value=value), self.assertRaises(ValueError): self.file(doc)
        doc = success(); doc["traceEvents"][5]["name"] = "shader.create.unpublished"
        with self.assertRaises(ValueError): self.file(doc)

    def test_wrong_scope_parent_thread_phase_category_and_unclosed_receipts_reject(self):
        for where, name, value in (("args", "parent", 10), ("args", "id", 999),
                ("args", "parent", 1.0), ("event", "tid", 8), ("event", "cat", "core"), ("event", "ph", "i")):
            doc = success(); event = doc["traceEvents"][5]
            (event["args"] if where == "args" else event)[name] = value
            with self.subTest(where=where, name=name), self.assertRaises(ValueError): self.file(doc)
        for options in (dict(parent=78), dict(thread=8), dict(capture=4), dict(handle=6)):
            with self.subTest(options=options), self.assertRaises(ValueError):
                self.file(capture(create(), release(**options)))
        for changes in (dict(context_known=0), dict(bytes=0), dict(payload_known=0), dict(cpu_returned=0)):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.file(capture(create(changes=changes)))
        for mode in ("unclosed", "unsubmitted", "wrong-kind", "outcome"):
            doc = success()
            if mode == "unclosed": doc["traceEvents"].pop(21)
            elif mode == "unsubmitted": doc["traceEvents"].pop(3)
            elif mode == "wrong-kind": doc["traceEvents"][3]["args"]["kind"] = 0
            else: doc["traceEvents"][21]["args"]["state"] = 1
            doc["jceTrace"].update(recorded=len(doc["traceEvents"]), captured=len(doc["traceEvents"]))
            with self.subTest(mode=mode), self.assertRaises(ValueError): self.file(doc)

    def test_link_child_mismatch_and_double_consumption_reject(self):
        for field, bad in (("vs", 7), ("fs", 7), ("program", shaders.INVALID_HANDLE),
                           ("creator_releases", 1), ("success", 0), ("rejection", 2)):
            doc = success()
            next(event for event in doc["traceEvents"] if event["name"] == "shader.link." + field)["args"]["value"] = bad
            with self.subTest(field=field), self.assertRaises(ValueError): self.file(doc)
        for mode in ("missing", "thread", "parent", "outcome", "duplicate", "after-owner-fields"):
            doc = success(); events = doc["traceEvents"]
            child = [event for event in events if event["name"] == shaders.LINK or event["name"].startswith("shader.link.")]
            if mode == "missing": events[:] = [event for event in events if event not in child]
            elif mode == "thread":
                for event in child: event["tid"] = 8
            elif mode == "parent":
                for event in child:
                    if event["ph"] in ("i", "C"): event["args"]["parent"] = 77
            elif mode == "outcome": child[-1]["args"]["state"] = 1
            elif mode == "duplicate":
                clone = copy.deepcopy(child)
                for event in clone:
                    event["ts"] += 31
                    event["args"]["task" if event["ph"] == "i" else "id"] = 32
                    if event["ph"] in ("B", "E"): event["args"]["parent"] = 32
                position = events.index(child[-1]) + 1
                events[position:position] = clone
            else:
                owner_field = next(event for event in events if event["name"] == "shader.owner.capture")
                events.remove(owner_field); owner_field["ts"] = 302
                events.insert(events.index(child[0]), owner_field)
            doc["jceTrace"].update(recorded=len(events), captured=len(events))
            with self.subTest(mode=mode), self.assertRaises(ValueError): self.file(doc)
        for groups in ((create(), release(), release(41, timestamp=400)),
                       (create(), create(11, 6, 200), link(fs_id=10, fs=5)),
                       (create(), create(11, 6, 200, context=10), link())):
            with self.assertRaises(ValueError): self.file(capture(*groups))

    def test_full_footer_eof_hash_limits_and_retention_are_canonical(self):
        for changes in (dict(lost=1, cursor_lost=1), dict(capture_generation_stable=False),
                        dict(recorded=999), dict(capture_complete=False)):
            doc = success(); doc["jceTrace"].update(changes)
            with self.subTest(changes=changes), self.assertRaises(reporter.TraceFileError) as failed:
                self.file(doc)
            self.assertEqual(failed.exception.evidence["sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())
        for suffix in (b" trailing", b"\xff"):
            self.path.write_bytes(json.dumps(success()).encode() + suffix)
            with self.assertRaises(reporter.TraceFileError):
                reporter.analyze_trace_file(self.path, ["app.init"], shader_provenance=True, chunk_bytes=1)
        for options in (dict(max_active=1), dict(max_history=3)):
            with self.subTest(options=options), self.assertRaises(ValueError): self.file(success(), **options)
        observer = shaders.ShaderProvenanceObservation(2, 1)
        original = shaders.ShaderProvenanceObservation
        with mock.patch.object(shaders, "ShaderProvenanceObservation", return_value=observer):
            with self.assertRaisesRegex(ValueError, "receipt history limit"):
                self.memory(capture(create(), create(11, 6, 200)))
        self.assertIs(shaders.ShaderProvenanceObservation, original)
        groups = []
        for n in range(100):
            groups.extend((create(10 + 2*n, 5, 100 + n*100), release(11 + 2*n, 10 + 2*n, 5, 140 + n*100)))
        data = self.file(capture(*groups), max_active=1)
        self.assertEqual(data["peak_pending_create_receipts"], 1)
        self.assertEqual(data["services"][shaders.CREATE]["first_complete"]["scope_id"], 10)
        self.assertEqual(data["services"][shaders.CREATE]["last_complete"]["scope_id"], 208)
        self.assertLess(len(json.dumps(data)), 20000)
        doc = capture(*groups)
        doc["traceEvents"][-2]["args"]["value"] = 0
        with self.assertRaisesRegex(ValueError, "CPU call return"):
            self.file(doc, max_active=1)

    def test_named_cooperative_zero_thread_uses_canonical_metadata_admission(self):
        doc = capture(create(thread=0), create(11, 6, 200, thread=0), link(thread=0), thread=0)
        doc["traceEvents"].insert(0, dict(ph="M", pid=1, tid=0, name="thread_name", args=dict(name="MAIN")))
        count = len(doc["traceEvents"])
        doc["jceTrace"].update(recorded=count, captured=count, mode="continuous", consumer_mode="cooperative",
            worker_available=False, write_ok=True, capture_complete=True, unexported=0,
            active_threads=0, active_work_items=0, active_waits=0)
        data = self.file(doc)
        self.assertEqual(data["services"][shaders.CREATE]["first_complete"]["thread"], 0)
        self.assertEqual(data["consumed_create_receipt_count"], 2)
        doc["jceTrace"].update(consumer_mode="threaded", worker_available=True)
        with self.assertRaises(ValueError):
            self.file(doc)

    def test_default_never_imports_observer_and_explicit_flag_is_strict(self):
        original = builtins.__import__
        def guard(name, *args, **kwargs):
            if "shader_provenance_observation" in name:
                raise AssertionError("default must not import shader observer")
            return original(name, *args, **kwargs)
        with mock.patch.object(builtins, "__import__", side_effect=guard):
            self.assertNotIn("shader_provenance", reporter.analyze_trace(success(), ["app.init"]))
        for value in (1, "yes", None):
            with self.subTest(value=value), self.assertRaises(ValueError):
                reporter.analyze_trace(capture(), ["app.init"], shader_provenance=value)

    def test_missing_consumer_wire_fails_an_ordinary_admission_assertion(self):
        malformed = success()
        malformed["traceEvents"][5]["args"]["value"] = -1
        def admission_assertion():
            try:
                reporter.analyze_trace(malformed, ["app.init"], shader_provenance=True)
            except ValueError:
                return
            self.fail("disconnected shader observer admitted a malformed packet")
        admission_assertion()
        original = reporter._TraceAnalysis.__init__
        def disconnected(analysis, *args, **kwargs):
            original(analysis, *args, **kwargs)
            analysis.shaders = None
        with mock.patch.object(reporter._TraceAnalysis, "__init__", disconnected):
            with self.assertRaisesRegex(AssertionError, "disconnected shader observer admitted"):
                admission_assertion()

    def test_tiny_chunks_memory_and_cli_share_the_same_owner(self):
        expected = self.file(success())
        for chunk in (1, 13, 1024):
            self.assertEqual(self.file(success(), chunk_bytes=chunk), expected)
        output = Path(self.folder.name) / "summary.json"
        args = ["lifecycle_trace_report", str(self.path), "--require", "app.init", "--shader-provenance", "--out", str(output)]
        with mock.patch.object(sys, "argv", args), redirect_stdout(io.StringIO()): self.assertEqual(reporter.main(), 0)
        self.assertEqual(json.loads(output.read_text())["shader_provenance"], expected)
        args.remove("--shader-provenance")
        with mock.patch.object(sys, "argv", args), redirect_stdout(io.StringIO()): self.assertEqual(reporter.main(), 0)
        self.assertNotIn("shader_provenance", json.loads(output.read_text()))


if __name__ == "__main__":
    unittest.main()
