"""Terminal protocol controls use authored packets and mocked process launch.

The real bounded trace reader validates every event. No native host or GPU is
launched; production-host emission remains a separate Native witness.
"""
import copy
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from tools import perf_bench
from tools import terminal_ack_observation as terminal

TOKEN = "0123456789abcdef0123456789abcdef"
PERF_LOG = ("jce_renderer: renderer: Direct3D 11 (graphics tier stable)\n"
            "engine: perf: 1.5 ms avg (667 FPS) | cpu 1.0 / gpu 0.5 ms | 10 draws | "
            "gpu-mem 12 MB (tex 10 + rt 2) | rss 20 MB | commit 30\n")


def packet(**updates):
    value = dict(schema=1, token=TOKEN, host="sdl", session="1", cleanup_scope="0",
        app_result=1, engine_tail_complete=True, trace_status="unavailable",
        trace_published=False, trace_write_ok=False, logger_shutdown_returned=True,
        log_file_close="unobserved")
    value.update(updates)
    return value


def encoded(value):
    return terminal.PREFIX + json.dumps(value, separators=(",", ":")).encode("utf-8") + b"\n"


def trace_document(session=1, scope=10):
    def event(phase, name, timestamp, args, category="task"):
        return dict(ph=phase, cat=category, pid=1, tid=77, ts=timestamp, name=name, args=args)
    events = [
        event("i", "engine.log_quiesce", 1, dict(task=1, parent=0, kind=2)),
        event("B", "engine.log_quiesce", 2, dict(id=1, parent=1)),
        event("E", "engine.log_quiesce", 3, dict(id=1, parent=1, ns=1000, state=0)),
        event("i", "engine.cleanup_attempt", 100, dict(task=scope, parent=0, kind=2)),
        event("B", "engine.cleanup_attempt", 101, dict(id=scope, parent=scope)),
        event("C", "engine.terminal_session", 102, dict(id=scope, parent=scope, value=session), "core"),
        event("C", "cleanup.reason", 103, dict(id=scope, parent=scope, value=0), "core"),
        event("C", "cleanup.completed", 104, dict(id=scope, parent=scope, value=1), "core"),
        event("E", "engine.cleanup_attempt", 105, dict(id=scope, parent=scope, ns=4000, state=0)),
    ]
    return dict(traceEvents=events, jceTrace=dict(mode="continuous", consumer_mode="cooperative",
        worker_available=False, write_ok=True, capture_complete=True, recorded=len(events),
        captured=len(events), overwritten_before_capture=0, cursor_lost=0, lost=0,
        unexported=0, active_threads=0, active_work_items=0, active_waits=0,
        capture_generation_stable=True))


class TerminalAckControls(unittest.TestCase):
    def setUp(self):
        holder = tempfile.TemporaryDirectory(prefix="jce-terminal-ack-")
        self.addCleanup(holder.cleanup)
        self.root = Path(holder.name)
        self.stderr = self.root / "stderr.log"
        self.trace = self.root / "trace.json"
        self.process = dict(timed_out=False, returncode=0, stderr_path=str(self.stderr),
            stderr="ignored merged text", process_identity=dict(pid=123,
                creation_time_unix_s=1000.5, creation_time_source="psutil.create_time"))

    def observe(self, value=None, *, raw=None, trace=None, process=None, history_budget=None,
                analysis_options=None, return_trace_report=False):
        self.stderr.write_bytes(encoded(packet() if value is None else value) if raw is None else raw)
        if trace is not None:
            trace["jceTrace"].update(recorded=len(trace["traceEvents"]), captured=len(trace["traceEvents"]))
            self.trace.write_text(json.dumps(trace), encoding="utf-8")
        return terminal.observe_terminal_ack(process or self.process, TOKEN,
            expected_stderr_path=self.stderr, trace_path=self.trace if trace is not None else None,
            terminal_trace_max_history=history_budget, trace_analysis_options=analysis_options,
            return_trace_report=return_trace_report)

    def test_scope_witness_option_uses_existing_required_transport_once(self):
        document = trace_document()
        phases = []
        for index, name in enumerate(("editor.play.begin", "editor.play.stop")):
            phases.append(name)
            task, base = 20 + index, 10 + index * 20
            document["traceEvents"][3:3] = [
                dict(ph="i", cat="task", pid=1, tid=77, ts=base, name=name, args=dict(task=task, parent=0, kind=2)),
                dict(ph="B", cat="task", pid=1, tid=77, ts=base+1, name=name, args=dict(id=task, parent=task)),
                dict(ph="E", cat="task", pid=1, tid=77, ts=base+2, name=name, args=dict(id=task, parent=task, ns=1000, state=0))]
        document["traceEvents"].sort(key=lambda event: event["ts"])
        options = dict(scope_witnesses=True, required=phases)
        before = copy.deepcopy(options)
        try:
            with self.counted_real_trace_reader() as reads:
                result = self.observe(packet(cleanup_scope="10", trace_status="complete", trace_published=True, trace_write_ok=True),
                    trace=document, analysis_options=options, return_trace_report=True)
        except ValueError as error:
            self.fail("existing ACK transport rejected scope witness option: %s" % error)
        self.assertEqual(reads, dict(opens=1, bytes=2*self.trace.stat().st_size, seeks=[(0,)], decodes=1))
        self.assertEqual(options, before)
        report = result["trace_report"]
        self.assertIn("required_scope_witnesses", report)
        rows = {row["name"]: row for row in report["required_scope_witnesses"]["required"]}
        self.assertEqual(set(rows), {"editor.play.begin", "editor.play.stop", "engine.log_quiesce"})
        self.assertEqual((rows["editor.play.begin"]["first"]["begin_timestamp_us"], rows["editor.play.stop"]["last"]["end_timestamp_us"]), (11, 32))
        self.assertEqual(result["trace"]["sha256"], report["sha256"])

    def test_scope_witness_strict_bool_is_checked_before_owned_stderr(self):
        for value in (1, None, "true"):
            with self.subTest(value=value), mock.patch.object(terminal, "_stderr_packet") as read:
                with self.assertRaises(ValueError):
                    terminal.observe_terminal_ack(self.process, TOKEN, expected_stderr_path=self.stderr,
                        trace_path=self.trace, trace_analysis_options=dict(scope_witnesses=value))
                read.assert_not_called()

    @staticmethod
    def prefix_trace():
        document = trace_document()
        for identity, begin, end, measured in ((20, 10, 20, 9000), (21, 40, 70, 29000)):
            document["traceEvents"][3:3] = [
                dict(ph="B", cat="render", pid=1, tid=77, ts=begin, name="bgfx.cpu.submit", args=dict(id=identity, parent=0)),
                dict(ph="E", cat="render", pid=1, tid=77, ts=end, name="bgfx.cpu.submit", args=dict(id=identity, parent=0, ns=measured))]
        document["traceEvents"].sort(key=lambda event: event["ts"])
        return document

    def assert_prefix_activity(self, report):
        self.assertIn("scope_prefix_observations", report)
        observation = report["scope_prefix_observations"]
        self.assertEqual(observation["status"], "CPU_SCOPE_ACTIVITY_OBSERVED")
        row, = observation["aggregates"]
        self.assertEqual((row["name"], row["thread"], row["observed_count"], row["measured_ns_sum"]),
                         ("bgfx.cpu.submit", 77, 2, 38000))
        self.assertEqual((row["first"]["begin_timestamp_us"], row["last"]["end_timestamp_us"]), (10, 70))

    def test_scope_prefixes_use_one_real_ack_decode_and_retain_same_report(self):
        options = dict(scope_prefixes=("bgfx.cpu.", "absent."))
        unchanged = copy.deepcopy(options)
        actual = terminal.lifecycle.analyze_trace_file
        with self.counted_real_trace_reader() as reads, \
                mock.patch.object(terminal.lifecycle, "analyze_trace_file", wraps=actual) as reader:
            result = self.observe(self.complete_packet(), trace=self.prefix_trace(),
                analysis_options=options, return_trace_report=True)
        self.assertEqual(reader.call_count, 1)
        self.assertEqual(reader.call_args.kwargs["scope_prefixes"], options["scope_prefixes"])
        self.assertEqual(reads, dict(opens=1, bytes=2*self.trace.stat().st_size, seeks=[(0,)], decodes=1))
        self.assertEqual(options, unchanged)
        self.assert_prefix_activity(result["trace_report"])
        self.assertEqual(result["trace"]["sha256"], result["trace_report"]["sha256"])
        self.assertEqual(result["trace_report"]["required"], ["engine.log_quiesce"])
        self.assertEqual(result["trace_report"]["scope_prefix_observations"]["prefixes"][-1]["status"], "NO_ACTIVITY")

    def test_scope_prefix_defaults_empty_and_unmatched_keep_existing_ack_admission(self):
        for options in (None, dict(scope_prefixes=[])):
            result = self.observe(self.complete_packet(), trace=trace_document(),
                analysis_options=options, return_trace_report=True)
            self.assertNotIn("scope_prefix_observations", result["trace_report"])
            self.assertEqual(result["status"], "HOST_TERMINAL_ACK_OBSERVED")
        result = self.observe(self.complete_packet(), trace=trace_document(),
            analysis_options=dict(scope_prefixes=["bgfx.cpu."]), return_trace_report=True)
        self.assertEqual(result["trace_report"]["scope_prefix_observations"]["status"], "NO_ACTIVITY")

    def test_invalid_prefix_options_refuse_before_stderr_or_trace_io(self):
        for prefixes in (None, True, "bgfx.cpu.", {"x"}, [""], [" x"], ["x\n"], [1],
                         ["x" * 129], ["x", "x"], ["p.%d" % i for i in range(129)]):
            with self.subTest(prefixes=prefixes), mock.patch.object(terminal, "_stderr_packet") as read, \
                    mock.patch.object(Path, "open") as opened:
                with self.assertRaisesRegex(ValueError, "scope prefix|scope_prefixes"):
                    terminal.observe_terminal_ack(self.process, TOKEN, expected_stderr_path=self.stderr,
                        trace_path=self.trace, trace_analysis_options=dict(scope_prefixes=prefixes))
                read.assert_not_called(); opened.assert_not_called()
        with mock.patch.object(terminal, "_stderr_packet") as read:
            with self.assertRaisesRegex(ValueError, "requested trace"):
                terminal.observe_terminal_ack(self.process, TOKEN, expected_stderr_path=self.stderr,
                    trace_analysis_options=dict(scope_prefixes=["bgfx.cpu."]))
            read.assert_not_called()

    def test_ordinary_missing_ack_prefix_forward_fails_the_same_activity_assertion(self):
        source = Path(terminal.__file__).read_text(encoding="utf-8")
        wire = '    required = options.pop("required")\n'
        self.assertEqual(source.count(wire), 1)
        namespace = dict(__name__="tools.terminal_missing_prefix", __package__="tools", __file__=terminal.__file__)
        exec(compile(source.replace(wire, wire + '    options.pop("scope_prefixes", None)\n', 1), terminal.__file__, "exec"), namespace)
        with mock.patch.object(terminal, "_trace_binding", namespace["_trace_binding"]):
            result = self.observe(self.complete_packet(), trace=self.prefix_trace(),
                analysis_options=dict(scope_prefixes=["bgfx.cpu."]), return_trace_report=True)
        self.assertEqual(result["trace_binding"], "validated")
        with self.assertRaises(AssertionError):
            self.assert_prefix_activity(result["trace_report"])

    def test_valid_packet_preserves_unobserved_close_and_identity(self):
        result = self.observe()
        self.assertEqual(result["packet"], packet())
        self.assertEqual(result["process_identity"], self.process["process_identity"])
        self.assertFalse(result["log_file_api_close_observed"])
        self.assertFalse(result["log_durability_observed"])
        self.assertEqual(result["stderr"]["sha256"], hashlib.sha256(self.stderr.read_bytes()).hexdigest())
        self.assertIsNone(result["trace"])
        self.assertEqual(result["trace_binding"], "not_requested")

    def test_no_file_api_ok_and_unavailable_birth_are_explicit(self):
        for close in ("no_file", "api_ok"):
            result = self.observe(packet(host="jni", log_file_close=close))
            self.assertEqual(result["log_file_api_close_observed"], close == "api_ok")
            self.assertFalse(result["log_durability_observed"])
        self.process["process_identity"].update(creation_time_unix_s=None, creation_time_source="UNAVAILABLE")
        self.assertIsNone(self.observe()["process_identity"]["creation_time_unix_s"])

    def test_missing_duplicate_wrong_token_and_truncated_lines_are_rejected(self):
        cases = (b"unrelated log\n", encoded(packet()) * 2,
                 encoded(packet(token="f" * 32)), encoded(packet()).rstrip(b"\n"),
                 terminal.PREFIX + b'{"schema":1}\n', b"JCE_TERMINAL_ACK\n",
                 terminal.PREFIX + b"x" * 5000 + b"\n", encoded(packet()) + terminal.PREFIX + b"{")
        for raw in cases:
            with self.subTest(raw=raw[:45]), self.assertRaises((ValueError, OSError)):
                self.observe(raw=raw)

    def test_raw_stderr_only_no_mirror_dedup_or_text_repair(self):
        self.process["stderr"] = encoded(packet()).decode("utf-8")
        with self.assertRaisesRegex(ValueError, "missing"):
            self.observe(raw=b"")
        self.observe(raw=b"x" * 9000 + b"\n" + encoded(packet()).replace(b"\n", b"\r\n"))
        with self.assertRaises(ValueError):
            self.observe(raw=terminal.PREFIX + b'{"schema":1,"token":"\xff"}\n')
        with self.assertRaisesRegex(ValueError, "missing"):
            self.observe(raw=b"prefix " + encoded(packet()))

    def test_strict_packet_types_ranges_and_terminal_outcomes(self):
        mutations = (("schema", True), ("schema", 2), ("host", "editor"),
            ("session", 1), ("session", "0"), ("session", "01"), ("session", str(1 << 63)),
            ("cleanup_scope", "-1"), ("cleanup_scope", str(1 << 64)),
            ("app_result", True), ("app_result", 0), ("app_result", 2),
            ("engine_tail_complete", 1), ("engine_tail_complete", False),
            ("logger_shutdown_returned", False), ("trace_published", 0),
            ("trace_published", True), ("trace_write_ok", True),
            ("trace_status", "incomplete"), ("trace_status", "other"),
            ("log_file_close", "api_failed"), ("log_file_close", "flushed"))
        for key, value in mutations:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                self.observe(packet(**{key: value}))
        for key in packet():
            value = packet(); del value[key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                self.observe(value)
        with self.assertRaises(ValueError):
            self.observe(packet(extra=True))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.observe(raw=encoded(packet()).replace(b'{"schema":1,', b'{"schema":1,"schema":1,'))

    def test_process_path_exit_and_birth_mismatches_refuse(self):
        for update in (dict(returncode=1), dict(returncode=True), dict(timed_out=True),
                       dict(stderr_path=str(self.root / "foreign.log")), dict(process_identity=None)):
            with self.subTest(update=update), self.assertRaises(ValueError):
                self.observe(process={**self.process, **update})
        for update in (dict(pid=True), dict(pid=0), dict(creation_time_unix_s=float("nan")),
                       dict(creation_time_unix_s=None), dict(creation_time_source="UNAVAILABLE")):
            bad = copy.deepcopy(self.process); bad["process_identity"].update(update)
            with self.subTest(update=update), self.assertRaises(ValueError):
                self.observe(process=bad)

    def test_valid_trace_binds_wide_session_and_final_cleanup(self):
        for session, scope in ((1, 10), ((1 << 63) - 1, (1 << 64) - 1)):
            value = packet(session=str(session), cleanup_scope=str(scope), trace_status="complete",
                           trace_published=True, trace_write_ok=True)
            result = self.observe(value, trace=trace_document(session, scope))
            self.assertEqual(result["trace"]["session"], str(session))
            self.assertEqual(result["trace"]["cleanup_scope"], str(scope))
            self.assertEqual(result["trace"]["sha256"], hashlib.sha256(self.trace.read_bytes()).hexdigest())
            self.assertEqual(result["trace_binding"], "validated")

    def test_history_budget_defaults_and_explicit_limits_use_real_complete_reader(self):
        default = self.observe(self.complete_packet(), trace=trace_document())
        self.assertEqual(default["trace"]["validation_limits"],
                         {"history": 200000, "active": 8192, "names": 4096})
        with self.assertRaisesRegex(ValueError, "historical task identity limit"):
            self.observe(self.complete_packet(), trace=trace_document(), history_budget=1)
        for budget in (2, 1000000):
            with self.subTest(budget=budget):
                observed = self.observe(self.complete_packet(), trace=trace_document(), history_budget=budget)
                self.assertEqual(observed["trace"]["validation_limits"],
                                 {"history": budget, "active": 8192, "names": 4096})
                self.assertEqual(observed["trace"]["cleanup_scope"], "10")
                self.assertEqual(observed["trace_binding"], "validated")

    def test_history_budget_rejects_nonintegers_ranges_and_absent_trace_before_io(self):
        for budget in (True, False, 0, -1, 1000001, 1.0, "2", [], {}):
            with self.subTest(budget=budget), mock.patch.object(terminal, "_stderr_packet") as read:
                with self.assertRaisesRegex(ValueError, "terminal_trace_max_history"):
                    terminal.observe_terminal_ack(self.process, TOKEN,
                        expected_stderr_path=self.stderr, trace_path=self.trace,
                        terminal_trace_max_history=budget)
                read.assert_not_called()
        with mock.patch.object(terminal, "_stderr_packet") as read:
            with self.assertRaisesRegex(ValueError, "requested.*trace"):
                terminal.observe_terminal_ack(self.process, TOKEN,
                    expected_stderr_path=self.stderr, terminal_trace_max_history=200000)
            read.assert_not_called()

    def test_explicit_history_budget_does_not_relax_loss_or_terminal_binding(self):
        for update in ({"lost": 1}, {"capture_complete": False}, {"write_ok": False}):
            document = trace_document(); document["jceTrace"].update(update)
            with self.subTest(update=update), self.assertRaises(ValueError):
                self.observe(self.complete_packet(), trace=document, history_budget=1000000)
        with self.assertRaisesRegex(ValueError, "uniquely belong"):
            self.observe(self.complete_packet(session="2"), trace=trace_document(), history_budget=1000000)

    def complete_packet(self, **updates):
        value = packet(cleanup_scope="10", trace_status="complete", trace_published=True, trace_write_ok=True)
        value.update(updates)
        return value

    def test_requested_trace_must_exist_be_complete_and_have_matching_final_scope(self):
        with self.assertRaisesRegex(ValueError, "requested trace"):
            self.observe(packet(), trace=trace_document())
        for change in (dict(cleanup_scope="9"), dict(session="2"), dict(cleanup_scope="0")):
            value = self.complete_packet(); value.update(change)
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.observe(value, trace=trace_document())
        for change in (dict(capture_complete=False), dict(lost=1), dict(write_ok=False)):
            doc = trace_document(); doc["jceTrace"].update(change)
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.observe(self.complete_packet(), trace=doc)

    def test_counter_binding_refuses_missing_duplicate_foreign_and_outside_scope(self):
        for field, value in (("id", 9), ("id", 1 << 64), ("id", True), ("parent", 9),
                             ("value", 2), ("value", 1.0), ("value", True), ("value", 0),
                             ("value", -1), ("value", 1 << 63)):
            doc = trace_document(); doc["traceEvents"][5]["args"][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                self.observe(self.complete_packet(), trace=doc)
        for field, value in (("tid", 78), ("tid", 1 << 64), ("cat", "render"), ("ph", "i")):
            doc = trace_document(); doc["traceEvents"][5][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                self.observe(self.complete_packet(), trace=doc)
        for kind in ("missing", "duplicate", "before", "after"):
            doc = trace_document(); sample = doc["traceEvents"].pop(5)
            if kind == "duplicate":
                doc["traceEvents"][5:5] = [sample, copy.deepcopy(sample)]
            elif kind == "before":
                sample["ts"] = 100; doc["traceEvents"].insert(4, sample)
            elif kind == "after":
                sample["ts"] = 106; doc["traceEvents"].append(sample)
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                self.observe(self.complete_packet(), trace=doc)

    def test_later_cleanup_prevents_binding_an_earlier_success(self):
        doc = trace_document()
        tail = trace_document(session=2, scope=11)["traceEvents"][3:]
        for item in tail: item["ts"] += 100
        doc["traceEvents"].extend(tail)
        with self.assertRaisesRegex(ValueError, "final trace retirement"):
            self.observe(self.complete_packet(), trace=doc)
        result = self.observe(packet(session="2", cleanup_scope="11", trace_status="complete",
            trace_published=True, trace_write_ok=True), trace=doc)
        self.assertEqual(result["trace"]["cleanup_attempts"], 2)

    def test_failed_cleanup_retry_does_not_invalidate_later_terminal_binding(self):
        doc = trace_document()
        failed = trace_document(scope=9)["traceEvents"][3:]
        failed.pop(2)  # A refused attempt has no successful terminal-session marker.
        for item in failed: item["ts"] -= 50
        failed[2]["args"]["value"] = 3
        failed[3]["args"]["value"] = 0
        failed[4]["args"]["state"] = 1
        doc["traceEvents"][3:3] = failed
        result = self.observe(self.complete_packet(), trace=doc)
        self.assertEqual(result["trace"]["cleanup_attempts"], 2)

    def test_trace_changed_after_canonical_validation_is_rejected(self):
        actual = terminal.lifecycle.analyze_trace_file
        def change(path, *args, **kwargs):
            result = actual(path, *args, **kwargs)
            with Path(path).open("ab") as stream: stream.write(b" ")
            return result
        with mock.patch.object(terminal.lifecycle, "analyze_trace_file", side_effect=change):
            with self.assertRaisesRegex(ValueError, "changed between"):
                self.observe(self.complete_packet(), trace=trace_document())

    def test_ordinary_omitted_token_gate_fails_same_foreign_token_assertion(self):
        source = Path(terminal.__file__).read_text(encoding="utf-8")
        wire = ('    if packet["token"] != token:\n'
                '        raise ValueError("terminal ack token does not belong to this run")\n')
        self.assertEqual(source.count(wire), 1)
        namespace = {"__file__": terminal.__file__, "__name__": "tools.terminal_ack_old_token"}
        exec(compile(source.replace(wire, "    # Omitted run-token gate.\n", 1),
                     terminal.__file__, "exec"), namespace)
        raw = json.dumps(packet(token="f" * 32)).encode()
        with self.assertRaises(ValueError): terminal._packet(raw, TOKEN)
        with self.assertRaises(AssertionError):
            with self.assertRaises(ValueError): namespace["_packet"](raw, TOKEN)

    def test_ordinary_omitted_trace_binding_fails_same_foreign_session_assertion(self):
        source = Path(terminal.__file__).read_text(encoding="utf-8")
        wire = ('    binding, trace_report = (_trace_binding(trace_path, packet, max_history=history_budget,\n'
                '        analysis_options=analysis_options) if trace_path is not None else (None, None))\n')
        self.assertEqual(source.count(wire), 1)
        namespace = {"__file__": terminal.__file__, "__name__": "tools.terminal_ack_old_binding"}
        exec(compile(source.replace(wire, "    binding, trace_report = None, None\n", 1), terminal.__file__, "exec"), namespace)
        value = self.complete_packet(session="2")
        with self.assertRaises(ValueError): self.observe(value, trace=trace_document())
        with self.assertRaises(AssertionError):
            with self.assertRaises(ValueError):
                namespace["observe_terminal_ack"](self.process, TOKEN,
                    expected_stderr_path=self.stderr, trace_path=self.trace)

    @contextmanager
    def counted_real_trace_reader(self):
        """Forward actual file IO and the actual canonical JSON decoder."""
        actual_open = Path.open
        actual_envelope = terminal.lifecycle._JsonStream.read_envelope
        counts = dict(opens=0, bytes=0, seeks=[], decodes=0)
        target = self.trace.resolve()
        class Stream:
            def __init__(inner, stream): inner.stream = stream
            def __enter__(inner): inner.stream.__enter__(); return inner
            def __exit__(inner, *args): return inner.stream.__exit__(*args)
            def __getattr__(inner, name): return getattr(inner.stream, name)
            def read(inner, *args):
                raw = inner.stream.read(*args); counts["bytes"] += len(raw); return raw
            def seek(inner, *args):
                counts["seeks"].append(args); return inner.stream.seek(*args)
        def opened(path, *args, **kwargs):
            stream = actual_open(path, *args, **kwargs)
            mode = args[0] if args else kwargs.get("mode", "r")
            if path.resolve() == target and mode == "rb":
                counts["opens"] += 1
                return Stream(stream)
            return stream
        def decoded(reader, accept_event):
            counts["decodes"] += 1
            return actual_envelope(reader, accept_event)
        with mock.patch.object(Path, "open", opened), mock.patch.object(terminal.lifecycle._JsonStream, "read_envelope", decoded):
            yield counts

    def integrated_trace(self):
        from tools.tests import test_shader_provenance_observation as shader_fixture
        document = trace_document()
        cleanup = document["traceEvents"][3:]
        for event in cleanup: event["ts"] += 1000
        document["traceEvents"][3:] = (shader_fixture.create(100, thread=77) +
            shader_fixture.create(101, 6, 200, thread=77) +
            shader_fixture.link(130, 100, 101, thread=77) +
            shader_fixture.create(102, shader_fixture.shaders.INVALID_HANDLE, 400, thread=77))
        phases = ["engine.renderer_create", "engine.app_init", "engine.app_exit", "engine.renderer_destroy"]
        for n, name in enumerate(phases):
            document["traceEvents"].extend(shader_fixture.packet(name, 200+n, 500+n*100, thread=77))
        document["traceEvents"].extend(cleanup)
        return document, phases

    def test_same_report_includes_required_cleanup_shader_and_one_real_decode_two_byte_reads(self):
        document, phases = self.integrated_trace()
        options = dict(required=phases, shader_provenance=True)
        unchanged = copy.deepcopy(options)
        with self.counted_real_trace_reader() as reads:
            result = self.observe(self.complete_packet(), trace=document,
                analysis_options=options, return_trace_report=True)
        report = result["trace_report"]
        self.assertEqual(options, unchanged)
        self.assertEqual(report["required"], phases + ["engine.log_quiesce"])
        self.assertEqual(reads, dict(opens=1, bytes=2*self.trace.stat().st_size, seeks=[(0,)], decodes=1))
        self.assertEqual(result["trace"]["sha256"], report["sha256"])
        self.assertEqual(report["sha256"], report["source_sha256_after"])
        self.assertTrue(report["json_utf8_eof_validated"] and report["source_fingerprint_stable"])
        self.assertEqual(report["shader_provenance"]["complete_create_link_creator_release_count"], 1)
        self.assertEqual(report["shader_provenance"]["failed_create_count"], 1)
        self.assertEqual(report["cleanup_ownership"]["terminal_sessions"]["final"]["session"], 1)
        self.assertEqual(set(result["trace"]), {"path", "sha256", "input_bytes", "cleanup_scope", "session", "counter", "cleanup_attempts", "validation_limits", "scope"})
        self.assertEqual(set(result["trace"]["counter"]), {"raw_index", "thread", "timestamp_us"})
        self.assertNotIn("trace_report", self.observe(self.complete_packet(), trace=trace_document()))

    def test_counted_reader_extra_decode_control_fails_ordinary_read_count_assertion(self):
        actual = terminal._trace_binding
        def extra_scan(path, *args, **kwargs):
            result = actual(path, *args, **kwargs)
            with Path(path).open("rb") as stream:
                reader = terminal.lifecycle._JsonStream(stream, 65536, 65536)
                reader.read_envelope(lambda _event: None)
                reader.finish_hash()
            return result
        with mock.patch.object(terminal, "_trace_binding", extra_scan), self.counted_real_trace_reader() as reads:
            self.observe(self.complete_packet(), trace=trace_document())
            with self.assertRaisesRegex(AssertionError, "one canonical decode"):
                self.assertEqual(reads["decodes"], 1, "one canonical decode required")
            self.assertEqual(reads["bytes"], 3*self.trace.stat().st_size)

    def test_trace_options_are_explicit_bounded_and_cannot_override_binding(self):
        bad_options = (True, [], "shader_provenance", {"unknown": True}, {"max_history": 1000000},
            {"cleanup_ownership": False}, {"terminal_session_binding": False}, {"path": "other"},
            {"trace_report": {}}, {"shader_provenance": 1}, {"resource_observations": "true"},
            {"required": "engine.log_quiesce"}, {"required": ["x", "x"]}, {"require_wait": [""]},
            {"require_counter_zero": [1]}, {"required": None})
        for options in bad_options:
            with self.subTest(options=options), mock.patch.object(terminal, "_stderr_packet") as read:
                with self.assertRaises((ValueError, TypeError)):
                    terminal.observe_terminal_ack(self.process, TOKEN, expected_stderr_path=self.stderr,
                        trace_path=self.trace, trace_analysis_options=options)
                read.assert_not_called()
        for options in ({}, {"shader_provenance": False}):
            with self.subTest(options=options), mock.patch.object(terminal, "_stderr_packet") as read:
                with self.assertRaisesRegex(ValueError, "requested trace"):
                    terminal.observe_terminal_ack(self.process, TOKEN, expected_stderr_path=self.stderr,
                        trace_analysis_options=options)
                read.assert_not_called()
        for value in (1, None, "true", True):
            with self.subTest(value=value), mock.patch.object(terminal, "_stderr_packet") as read:
                with self.assertRaises(ValueError):
                    terminal.observe_terminal_ack(self.process, TOKEN, expected_stderr_path=self.stderr,
                        return_trace_report=value)
                read.assert_not_called()
        with self.assertRaisesRegex(ValueError, "required phase missing"):
            self.observe(self.complete_packet(), trace=trace_document(), analysis_options={"required": ["absent.phase"]})

    def test_terminal_collection_default_off_and_strict_canonical_memory_file_options(self):
        document = trace_document()
        ordinary = terminal.lifecycle.analyze_trace(document, ["engine.log_quiesce"], cleanup_ownership=True)
        self.assertNotIn("terminal_sessions", ordinary["cleanup_ownership"])
        self.assertIsNone(terminal.lifecycle._TraceAnalysis(["engine.log_quiesce"], [], [], cleanup_ownership=True).cleanup.terminal)
        explicit = terminal.lifecycle.analyze_trace(document, ["engine.log_quiesce"], cleanup_ownership=True,
            terminal_session_binding=True)["cleanup_ownership"]
        returned = self.observe(self.complete_packet(), trace=document, return_trace_report=True)["trace_report"]["cleanup_ownership"]
        self.assertEqual(explicit, returned)
        for value in (1, None, "true"):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "explicit bool"):
                terminal.lifecycle.analyze_trace(document, ["engine.log_quiesce"], cleanup_ownership=True,
                    terminal_session_binding=value)
        with self.assertRaisesRegex(ValueError, "requires cleanup_ownership"):
            terminal.lifecycle.analyze_trace(document, ["engine.log_quiesce"], terminal_session_binding=True)

    def test_supported_wait_counter_options_and_terminal_cli_use_same_canonical_owner(self):
        import io
        import sys
        from contextlib import redirect_stdout
        document = trace_document()
        base = dict(cat="wait", pid=1, tid=77, name="cpu.wait")
        document["traceEvents"][2:2] = [dict(base, ph="B", ts=2.1, args=dict(id=900, parent=1)),
            dict(base, ph="E", ts=2.2, args=dict(id=900, parent=1, ns=100))]
        options = dict(required=("engine.log_quiesce",), require_wait=["cpu.wait"],
            require_counter_zero=["cleanup.reason"], require_counter_last_zero=["cleanup.reason"],
            shader_provenance=False)
        result = self.observe(self.complete_packet(), trace=document, analysis_options=options, return_trace_report=True)
        report = result["trace_report"]
        self.assertEqual(report["required_wait_aggregates"][0]["observed_count"], 1)
        self.assertEqual(report["counter_controls"]["all_zero"][0]["last_value"], 0)
        output = self.root / "report.json"
        args = ["lifecycle_trace_report", str(self.trace), "--require", "engine.log_quiesce", "--cleanup-ownership",
            "--terminal-session-binding", "--out", str(output)]
        with mock.patch.object(sys, "argv", args), redirect_stdout(io.StringIO()):
            self.assertEqual(terminal.lifecycle.main(), 0)
        self.assertEqual(json.loads(output.read_text())["cleanup_ownership"], report["cleanup_ownership"])

    def test_all_named_markers_validate_even_unrelated_or_orphan_and_session_reuse(self):
        for bad in (dict(id=999, parent=999, value=2), dict(id=999, parent=999, value=True),
                    dict(id=0, parent=0, value=2), dict(id=999, parent=998, value=2)):
            document = trace_document()
            marker = copy.deepcopy(document["traceEvents"][5]); marker.update(ts=50); marker["args"] = bad
            document["traceEvents"].insert(3, marker)
            with self.subTest(marker=bad), self.assertRaises(ValueError):
                self.observe(self.complete_packet(), trace=document)
        document = trace_document()
        tail = trace_document(session=1, scope=11)["traceEvents"][3:]
        for event in tail: event["ts"] += 100
        document["traceEvents"].extend(tail)
        with self.assertRaisesRegex(ValueError, "uniquely belong"):
            self.observe(self.complete_packet(cleanup_scope="11"), trace=document)
        document = trace_document()
        document["traceEvents"][6]["args"]["value"] = 3
        document["traceEvents"][7]["args"]["value"] = 0
        document["traceEvents"][8]["args"]["state"] = 1
        with self.assertRaisesRegex(ValueError, "successful closed cleanup"):
            self.observe(self.complete_packet(), trace=document)
        document = trace_document()
        base = dict(pid=1, tid=77, name="other.service", cat="task")
        foreign = [dict(base, ph="i", ts=40, args=dict(task=900, parent=0, kind=2)),
            dict(base, ph="B", ts=41, args=dict(id=900, parent=900)),
            dict(base, ph="E", ts=43, args=dict(id=900, parent=900, ns=2000, state=0))]
        marker = copy.deepcopy(document["traceEvents"][5]); marker["ts"] = 42
        marker["args"].update(id=900, parent=900, value=2)
        foreign.insert(2, marker); document["traceEvents"][3:3] = foreign
        with self.assertRaisesRegex(ValueError, "active cleanup owner"):
            self.observe(self.complete_packet(), trace=document)

    def test_wide_thread_ids_and_bounded_sessions_keep_exact_first_last_final(self):
        wide = (1 << 64) - 1
        document = trace_document(session=(1 << 63)-1, scope=wide-1)
        for event in document["traceEvents"]: event["tid"] = wide
        value = self.complete_packet(session=str((1 << 63)-1)); value["cleanup_scope"] = str(wide-1)
        result = self.observe(value, trace=document, return_trace_report=True)
        self.assertEqual(result["trace"]["counter"]["thread"], str(wide))
        summary = result["trace_report"]["cleanup_ownership"]["terminal_sessions"]
        self.assertEqual(summary["final"]["scope_id"], str(wide-1))
        self.assertEqual(summary["final"]["session"], str((1 << 63)-1))
        document = trace_document()
        for n in range(2, 101):
            tail = trace_document(session=n, scope=n+10)["traceEvents"][3:]
            for event in tail: event["ts"] += n*100
            document["traceEvents"].extend(tail)
        value = self.complete_packet(session="100"); value["cleanup_scope"] = "110"
        result = self.observe(value, trace=document, history_budget=150, return_trace_report=True)
        summary = result["trace_report"]["cleanup_ownership"]["terminal_sessions"]
        self.assertEqual(summary["observed_count"], 100)
        self.assertEqual((summary["first"]["session"], summary["last"]["session"], summary["final"]["session"]), (1,100,100))
        self.assertEqual(summary["validation_limits"], dict(historical_sessions=150, retained_witnesses=3))
        self.assertLess(len(json.dumps(summary)), 1500)

    def test_malformed_tail_retains_whole_file_fingerprint_without_extra_decode(self):
        self.stderr.write_bytes(encoded(self.complete_packet()))
        for suffix in (b" trailing", b"\xff", b"{\"orphan\":1}"):
            self.trace.write_bytes(json.dumps(trace_document()).encode() + suffix)
            with self.counted_real_trace_reader() as reads:
                with self.assertRaises(terminal.lifecycle.TraceFileError) as failed:
                    terminal.observe_terminal_ack(self.process, TOKEN, expected_stderr_path=self.stderr,
                        trace_path=self.trace, return_trace_report=True)
            self.assertEqual(reads, dict(opens=1, bytes=2*self.trace.stat().st_size, seeks=[(0,)], decodes=1))
            self.assertEqual(failed.exception.evidence["sha256"], hashlib.sha256(self.trace.read_bytes()).hexdigest())


class PerfTerminalAckControls(unittest.TestCase):
    def setUp(self):
        holder = tempfile.TemporaryDirectory(prefix="jce-perf-terminal-")
        self.addCleanup(holder.cleanup)
        self.root = Path(holder.name)
        self.log = self.root / "engine.log"
        self.observed = {}

    def invoke(self, *, owner=perf_bench, require=True, emit=True, environment=None, trace=False, history_budget=None):
        environment = dict(environment or {})
        def child(_command, *, env, output_dir, **kwargs):
            self.observed.update(env=env, options=kwargs)
            Path(env["JCE_LOG_FILE"]).write_text(PERF_LOG, encoding="utf-8")
            output_dir = Path(output_dir); output_dir.mkdir(parents=True, exist_ok=True)
            stderr = output_dir / "stderr.log"
            value = packet(token=env.get("JCE_TERMINAL_ACK_TOKEN", TOKEN))
            if trace:
                trace_path = Path(kwargs["cwd"]) / env["JCE_TRACE_STREAM"]
                trace_path.write_text(json.dumps(trace_document()), encoding="utf-8")
                value.update(trace_status="complete", trace_published=True, trace_write_ok=True, cleanup_scope="10")
            stderr.write_bytes(encoded(value) if emit else b"")
            return dict(timed_out=False, returncode=0, elapsed_s=1, samples=[], stdout="", stderr="",
                stderr_path=str(stderr), process_identity=dict(pid=456,
                    creation_time_unix_s=None, creation_time_source="UNAVAILABLE"))
        with mock.patch.object(owner, "run_process", side_effect=child):
            return owner.run(environment, self.log, frames=3, exe=self.root / "fixture.exe",
                             cwd=self.root, require_terminal_ack=require,
                             terminal_trace_max_history=history_budget)

    def test_default_neither_requests_nor_consumes_ack(self):
        with mock.patch.object(perf_bench.secrets, "token_hex", side_effect=AssertionError("must remain opt-in")):
            result = self.invoke(require=False, emit=False)
        self.assertNotIn("JCE_TERMINAL_ACK_TOKEN", self.observed["env"])
        self.assertNotIn("terminal_ack", result)

    def test_opt_in_injects_fresh_generated_token_and_ignores_caller_reuse(self):
        tokens = []
        for _ in range(2):
            result = self.invoke(environment={"JCE_TERMINAL_ACK_TOKEN": "caller stale token"})
            token = self.observed["env"]["JCE_TERMINAL_ACK_TOKEN"]
            self.assertRegex(token, r"\A[0-9a-f]{32}\Z")
            self.assertEqual(result["terminal_ack"]["packet"]["token"], token)
            tokens.append(token)
        self.assertNotEqual(tokens[0], tokens[1])

    def test_opt_in_missing_ack_is_an_ordinary_admission_failure(self):
        with self.assertRaisesRegex(ValueError, "missing terminal ack"):
            self.invoke(emit=False)

    def test_requested_fresh_relative_trace_is_bound(self):
        result = self.invoke(environment={"JCE_TRACE_STREAM": "trace.json"}, trace=True)
        self.assertEqual(result["terminal_ack"]["trace_binding"], "validated")

    def test_explicit_history_budget_is_forwarded_through_real_trace_binding(self):
        with self.assertRaisesRegex(ValueError, "historical task identity limit"):
            self.invoke(environment={"JCE_TRACE_STREAM": "small.json"}, trace=True, history_budget=1)
        result = self.invoke(environment={"JCE_TRACE_STREAM": "sufficient.json"}, trace=True, history_budget=2)
        self.assertEqual(result["terminal_ack"]["trace"]["validation_limits"],
                         {"history": 2, "active": 8192, "names": 4096})

    def test_invalid_history_budget_preserves_outputs_before_process_launch(self):
        samples = self.root / "frames.csv"
        self.log.write_bytes(b"prior log")
        samples.write_bytes(b"prior CSV")
        invalid = [(True, {"JCE_TRACE_STREAM": "fresh.json"}, budget)
                   for budget in (True, False, 0, -1, 1000001, 1.0, "2", [], {})]
        invalid.extend(((False, {}, 2), (False, {"JCE_TRACE_STREAM": "fresh.json"}, 2),
                        (True, {}, 2), (True, {}, 200000)))
        for require, environment, budget in invalid:
            with self.subTest(require=require, budget=budget, environment=environment):
                with mock.patch.object(perf_bench, "run_process") as child:
                    with self.assertRaisesRegex(ValueError, "terminal_trace_max_history"):
                        perf_bench.run(environment, self.log, exe=self.root / "fixture.exe",
                            cwd=self.root, samples_path=samples, require_terminal_ack=require,
                            terminal_trace_max_history=budget)
                    child.assert_not_called()
                self.assertEqual(self.log.read_bytes(), b"prior log")
                self.assertEqual(samples.read_bytes(), b"prior CSV")

    def test_bad_configuration_preserves_prior_outputs_and_never_launches(self):
        self.log.write_bytes(b"old log")
        old = self.root / "old-trace.json"; old.write_bytes(b"old trace")
        options = (({"JCE_TRACE_STREAM": str(old)}, True), ({"JCE_TRACE": "1"}, True),
                   ({"JCE_TRACE_EXPORT": "resident.json"}, True), ({}, "true"))
        for environment, require in options:
            with mock.patch.object(perf_bench, "run_process") as child:
                with self.subTest(environment=environment, require=require), self.assertRaises(ValueError):
                    perf_bench.run(environment, self.log, exe=self.root / "fixture.exe",
                        cwd=self.root, require_terminal_ack=require)
                child.assert_not_called()
                self.assertEqual(self.log.read_bytes(), b"old log")
                self.assertEqual(old.read_bytes(), b"old trace")

    def test_trace_output_alias_is_refused_before_launch(self):
        for trace in (self.log, self.root / "engine_process" / "stderr.log"):
            with mock.patch.object(perf_bench, "run_process") as child:
                with self.assertRaisesRegex(ValueError, "overlaps"):
                    perf_bench.run({"JCE_TRACE_STREAM": str(trace)}, self.log,
                        exe=self.root / "fixture.exe", require_terminal_ack=True)
                child.assert_not_called()

    def test_ordinary_omitted_consumer_gate_fails_same_missing_ack_assertion(self):
        source = Path(perf_bench.__file__).read_text(encoding="utf-8")
        wire = ('        terminal_observation = observe_terminal_ack(result, terminal_token,\n'
                '            expected_stderr_path=process_output / "stderr.log", trace_path=terminal_trace,\n'
                '            terminal_trace_max_history=terminal_trace_max_history)\n')
        self.assertEqual(source.count(wire), 1)
        import types
        owner = types.ModuleType("old_terminal_gate")
        owner.__file__ = perf_bench.__file__
        exec(compile(source.replace(wire, "        terminal_observation = None\n", 1),
                     perf_bench.__file__, "exec"), owner.__dict__)
        with self.assertRaises(AssertionError):
            with self.assertRaisesRegex(ValueError, "missing terminal ack"):
                self.invoke(owner=owner, emit=False)


if __name__ == "__main__":
    unittest.main()
