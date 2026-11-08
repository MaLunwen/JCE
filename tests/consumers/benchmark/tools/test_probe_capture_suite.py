"""Ordinary offline controls for the probe witness; launches no application."""
import copy
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import probe_capture_suite as suite
from probe_capture_suite import (MAGIC, COLORS, WORKER, PROFILE_INPUT_BYTES, decode_ktx,
    profile_before_launch, validate_profile_after,
    validate_directions, validate_host, validate_scope_projection)

RUNTIME_CONFIG = """# Canonical Engine persistence, separate from authored input.
[window]
width = 1280
height = 720
fullscreen = false
maximized = false
resizable = true
title = JCE
[renderer]
backend = auto
backend_resolved = d3d11
vsync = false
debug_text = true
clear_color = 000000FF
[audio]
master_volume = 1.000
music_volume = 0.800
sfx_volume = 1.000
[app]
locale =
[logging]
level = info
colors = true
[performance]
machine_class = full
job_workers = 2
"""


def profile_pair(directory):
    authored, runtime = directory / "profile.input.ini", directory / "profile.ini"
    authored.write_bytes(PROFILE_INPUT_BYTES)
    runtime.write_bytes(PROFILE_INPUT_BYTES)
    return authored, runtime, profile_before_launch(authored, runtime)


def cube_bytes(hdr=True, sidecar=False, colors=COLORS):
    width, mips = (32, 1) if sidecar else (16, 5)
    kind, size, internal = (0x140B, 2, 0x881A) if hdr else (0x1401, 1, 0x8058)
    output = bytearray(MAGIC + struct.pack("<13I", 0x04030201, kind, size, 0x1908,
        internal, 0x1908, width, width, 0, 0, 6, mips, 0))
    for mip in range(mips):
        w = max(1, width >> mip)
        output += struct.pack("<I", w * w * 4 * size)
        for color in colors:
            values = (*color, 1)
            texel = struct.pack("<4e", *values) if hdr else bytes(round(min(1, v) * 255) for v in values)
            output += texel * (w * w)
    return bytes(output)


def witness(mode="hdr"):
    def event(name, frame=1, **values):
        return {"event": name, "frame": frame, "a": "0", "b": "0", "c": "0",
            "service": 1, "attached": 0, "retiring": 0, "destinations": 0,
            "bytes": 0, "stop_releases": 0, **values}
    cancelled = mode.startswith("cancel-")
    events = [event("zero_baseline"), event("capture_begin")]
    if cancelled:
        events += [event("capture_poll", c="1"), event("cancel_pending", c="1",
                   retiring=1, destinations=1, bytes=2048)]
        if mode == "cancel-ready":
            events += [event("collected_ready", frame=2, a="0", b="1"),
                       event("capture_begin", frame=2, a="1")]
    if mode != "cancel-exit":
        events += [event("capture_poll", frame=i + 3, c="2" if i == 11 else "1") for i in range(12)]
        events += [event("capture_faces_done_bake_submitted", frame=15)]
    workers = int(mode != "cancel-exit")
    host = {"schema": 1, "case": mode, "complete": True, "failed": False,
        "trace_lost": "0", "polls": sum(e["event"] == "capture_poll" for e in events),
        "pending_retained": cancelled, "collected_ready": mode == "cancel-ready",
        "worker_submits": workers, "worker_ends": workers}
    return host, events


class ProbeCaptureControls(unittest.TestCase):
    def test_profile_input_and_runtime_seed_are_identical_before_launch(self):
        with tempfile.TemporaryDirectory() as temporary:
            authored, runtime, before = profile_pair(Path(temporary))
            self.assertEqual(before["input_sha256_before"], before["runtime_sha256_before"])
            self.assertEqual(before["input_path"], "profile.input.ini")
            runtime.write_bytes(PROFILE_INPUT_BYTES + b"# Changed seed\n")
            with self.assertRaisesRegex(ValueError, "seed differs"):
                profile_before_launch(authored, runtime)

    def test_default_persistence_and_expected_resolved_backend_are_allowed(self):
        with tempfile.TemporaryDirectory() as temporary:
            for backend in ("d3d11", "d3d12", "vulkan", "gl"):
                with self.subTest(backend=backend):
                    authored, runtime, observation = profile_pair(Path(temporary))
                    resolved = "opengl" if backend == "gl" else backend
                    runtime.write_text(RUNTIME_CONFIG.replace("backend_resolved = d3d11",
                        "backend_resolved = " + resolved), encoding="utf-8", newline="\n")
                    validate_profile_after(authored, runtime, observation, backend)
                    self.assertTrue(observation["input_snapshot_unchanged"])
                    self.assertTrue(observation["runtime_mutated"])
                    self.assertEqual(observation["status"], "PERSISTED_REQUEST_FIELDS_PRESERVED")
                    self.assertEqual(observation["resolved_backend"], resolved)
                    self.assertNotEqual(observation["runtime_sha256_after"], observation["input_sha256_after"])

    def test_unchanged_runtime_is_observed_without_invented_persistence(self):
        with tempfile.TemporaryDirectory() as temporary:
            authored, runtime, observation = profile_pair(Path(temporary))
            validate_profile_after(authored, runtime, observation, "d3d11")
            self.assertFalse(observation["runtime_mutated"])
            self.assertIsNone(observation["resolved_backend"])

    def test_changed_or_missing_request_fields_fail_ordinary_validation(self):
        changes = (("vsync = false", "vsync = true", "renderer.vsync"),
                   ("job_workers = 2", "job_workers = 3", "performance.job_workers"),
                   ("machine_class = full", "machine_class = low", "performance.machine_class"),
                   ("vsync = false", "", "renderer.vsync"))
        with tempfile.TemporaryDirectory() as temporary:
            for old, new, key in changes:
                with self.subTest(key=key, new=new):
                    authored, runtime, observation = profile_pair(Path(temporary))
                    runtime.write_text(RUNTIME_CONFIG.replace(old, new), encoding="utf-8", newline="\n")
                    with self.assertRaisesRegex(ValueError, "field changed or missing: " + key):
                        validate_profile_after(authored, runtime, observation, "d3d11")
                    self.assertTrue(observation["runtime_mutated"])
                    self.assertEqual(observation["runtime_sha256_after"], suite.sha256(runtime))

    def test_input_snapshot_mutation_fails_even_with_identical_request_fields(self):
        with tempfile.TemporaryDirectory() as temporary:
            authored, runtime, observation = profile_pair(Path(temporary))
            authored.write_bytes(PROFILE_INPUT_BYTES + b"# Mutation does not change parsed fields\n")
            runtime.write_text(RUNTIME_CONFIG, encoding="utf-8", newline="\n")
            with self.assertRaisesRegex(ValueError, "input snapshot changed"):
                validate_profile_after(authored, runtime, observation, "d3d11")
            self.assertFalse(observation["input_snapshot_unchanged"])

    def test_wrong_resolved_unknown_or_nondefault_mutation_fails(self):
        for content in (RUNTIME_CONFIG.replace("backend_resolved = d3d11", "backend_resolved = vulkan"),
                        RUNTIME_CONFIG.replace("width = 1280", "width = 640"),
                        RUNTIME_CONFIG + "unrecognized = 1\n"):
            with self.subTest(content=content), tempfile.TemporaryDirectory() as temporary:
                authored, runtime, observation = profile_pair(Path(temporary))
                runtime.write_text(content, encoding="utf-8", newline="\n")
                with self.assertRaisesRegex(ValueError, "resolved profile backend|unexpected runtime"):
                    validate_profile_after(authored, runtime, observation, "d3d11")

    def test_actual_runner_consumes_profile_ownership_without_launching_app(self):
        # All process/trace observations below are explicit ports. This tests
        # the real runner's file ownership wires, not hardware or CPU closure.
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            exe = directory / "fixture.exe"
            exe.write_bytes(b"Not an executable; process port only")
            host, events = witness("cancel-exit")

            def process_port(command, *, env, **kwargs):
                output = Path(env["JCE_BENCH_PROBE_CAPTURE_DIR"])
                self.assertEqual(Path(env["JCE_BENCH_CONFIG_FILE"]), output / "profile.ini")
                self.assertEqual((output / "profile.input.ini").read_bytes(), PROFILE_INPUT_BYTES)
                self.assertEqual((output / "profile.ini").read_bytes(), PROFILE_INPUT_BYTES)
                (output / "profile.ini").write_text(RUNTIME_CONFIG, encoding="utf-8", newline="\n")
                suite.write(output / "probe-host.json", host)
                (output / "probe-events.jsonl").write_text(
                    "".join(json.dumps(event) + "\n" for event in events), encoding="utf-8", newline="\n")
                suite.write(output / "trace.json", {})
                return {"scope": "PROCESS_PORT_ONLY"}

            with patch.object(suite, "run_process", side_effect=process_port) as run, \
                 patch.object(suite, "validate_process"), \
                 patch.object(suite, "read_execution_log", return_value="jce_renderer: renderer: Direct3D 11\n"), \
                 patch.object(suite, "validate_trace", return_value={"scope": "TRACE_PORT_ONLY"}):
                result = suite.run_case(exe, "cancel-exit", "d3d11", directory / "case", 90)
            run.assert_called_once()
            self.assertEqual(result["status"], "ACTUAL_PROBE_WORKFLOW_OBSERVED")
            self.assertEqual(result["identity"]["config"], result["profile"]["input_sha256_before"])
            self.assertTrue(result["profile"]["runtime_mutated"])
            self.assertTrue(result["profile"]["input_snapshot_unchanged"])
            self.assertIn("profile.input.ini", result["artifacts"])
            self.assertNotEqual(result["artifacts"]["profile.ini"], result["artifacts"]["profile.input.ini"])
            self.assertTrue(any(text.startswith("when a legacy") for text in result["limitations"]))
            self.assertFalse(any(text.startswith("pinned writer") for text in result["limitations"]))

    def test_hdr_and_ldr_faces_mips_and_directions(self):
        for hdr in (True, False):
            self.assertEqual(len(validate_directions(decode_ktx(cube_bytes(hdr), hdr=hdr))), 6)
            self.assertEqual(decode_ktx(cube_bytes(hdr, True), hdr=hdr, sidecar=True)["width"], 32)

    def test_swapped_faces_fail_ordinary_direction_assertion(self):
        colors = list(COLORS)
        colors[0], colors[1] = colors[1], colors[0]
        with self.assertRaisesRegex(ValueError, "directional|face order"):
            validate_directions(decode_ktx(cube_bytes(colors=colors), hdr=True))

    def test_missing_hdr_radiance_is_not_a_format_only_pass(self):
        colors = [tuple(min(1, x) for x in color) for color in COLORS]
        with self.assertRaisesRegex(ValueError, "clamped HDR"):
            validate_directions(decode_ktx(cube_bytes(colors=colors), hdr=True))

    def test_nonfinite_truncated_wrong_format_and_trailing_fail(self):
        raw = cube_bytes()
        bad = bytearray(raw)
        struct.pack_into("<e", bad, 68, float("nan"))
        for value, hdr in ((bytes(bad), True), (raw[:-1], True), (raw + b"x", True), (raw, False)):
            with self.assertRaises(ValueError):
                decode_ktx(value, hdr=hdr)

    def test_exact_pinned_bimg_header_convention_is_visible(self):
        for hdr in (True, False):
            raw = bytearray(cube_bytes(hdr))
            struct.pack_into("<3I", raw, 16, 0, 1, 0)
            decoded = decode_ktx(bytes(raw), hdr=hdr)
            self.assertEqual(decoded["header_layout"], "pinned_bimg_zero_gl_type")
            validate_directions(decoded)
            struct.pack_into("<I", raw, 24, 0x1908)
            with self.assertRaises(ValueError):
                decode_ktx(bytes(raw), hdr=hdr)

    def test_all_four_host_workflows(self):
        for mode in ("hdr", "ldr", "cancel-ready", "cancel-exit"):
            validate_host(mode, *witness(mode))

    def test_implicit_submit_missing_done_and_worker_fail(self):
        for mutation in ("submit", "done", "worker"):
            host, events = witness()
            if mutation == "submit":
                next(e for e in events if e["event"] == "capture_poll")["b"] = "1"
            elif mutation == "done":
                events = [e for e in events if e["event"] != "capture_faces_done_bake_submitted"]
            else:
                host["worker_ends"] = 0
            with self.assertRaises(ValueError):
                validate_host("hdr", host, events)

    def test_cancel_must_retain_pending_and_collect_before_stop(self):
        for mutation in ("pending", "stopped", "no_advance"):
            host, events = witness("cancel-ready")
            if mutation == "pending":
                next(e for e in events if e["event"] == "cancel_pending")["bytes"] = 0
            else:
                row = next(e for e in events if e["event"] == "collected_ready")
                if mutation == "stopped": row["stop_releases"] = 1
                else: row["b"] = row["a"]
            with self.assertRaises(ValueError):
                validate_host("cancel-ready", host, events)

    def test_scope_projection_keeps_async_identity_and_one_central_submit(self):
        # This group tests only the consumer projection. The actual complete
        # analyzer is called separately by validate_trace and has its own tests.
        host = {"turns": 1, "worker_task": "7"}
        analysis = {"status": "CPU_FIXTURE_ONLY", "counter_controls": {}, "capture": {},
            "scopes": [{"name": WORKER, "category": "task", "task_kind": 1, "state": 0, "owner": 7}]}
        document = {"traceEvents": [
            {"ph": "C", "name": "bench.probe.turn", "tid": 1, "ts": 10},
            {"ph": "E", "name": "renderer.frame_advance_cpu", "tid": 1, "ts": 20},
            {"ph": "C", "name": "bench.probe.exit", "tid": 1, "ts": 30},
            {"ph": "C", "name": "readback.backend_stop_cpu_releases", "args": {"value": 0}}]}
        self.assertEqual(validate_scope_projection(document, analysis, "hdr", host)["async_bakes"], 1)
        for mutation in ("no_worker", "failed_worker", "service", "wrong_owner", "extra_submit", "missing_submit"):
            raw, scopes = copy.deepcopy(document), copy.deepcopy(analysis)
            if mutation == "no_worker": scopes["scopes"] = []
            elif mutation == "failed_worker": scopes["scopes"][0]["state"] = 1
            elif mutation == "service": scopes["scopes"][0]["task_kind"] = 2
            elif mutation == "wrong_owner": scopes["scopes"][0]["owner"] = 8
            elif mutation == "extra_submit": raw["traceEvents"].insert(2, dict(raw["traceEvents"][1], ts=21))
            else: raw["traceEvents"].pop(1)
            with self.assertRaises(ValueError):
                validate_scope_projection(raw, scopes, "hdr", host)

    def test_cancel_exit_classifies_actual_ready_or_backend_stop_release(self):
        # Projection-only synthetic records qualify the classifier, never a
        # real backend-stop receipt. Actual runs still use the full analyzer.
        names = ("readback.cpu_service_open", "readback.cpu_attached_queues",
            "readback.cpu_retiring_queues", "readback.cpu_owned_destinations",
            "readback.cpu_owned_destination_bytes", "readback.backend_stop_cpu_releases")
        def ledger(ts, values):
            return [{"ph": "C", "name": name, "ts": ts, "tid": 1,
                     "args": {"value": value}} for name, value in zip(names, values)]
        def document(stopped):
            rows = [{"ph": "C", "name": "bench.probe.turn", "tid": 1, "ts": 1},
                    {"ph": "C", "name": "bench.probe.cancel", "tid": 1, "ts": 2}]
            rows += ledger(3, (1, 0, 1, 1, 2048, 0))
            rows += [{"ph": "E", "name": "renderer.frame_advance_cpu", "tid": 1, "ts": 4}]
            if not stopped: rows += ledger(5, (1, 0, 0, 0, 0, 0))
            rows += [{"ph": "C", "name": "bench.probe.exit", "tid": 1, "ts": 6},
                     {"ph": "B", "name": "engine.renderer_destroy", "tid": 1, "ts": 7}]
            rows += ledger(8, (0, 0, 0, 0, 0, int(stopped)))
            rows += [{"ph": "E", "name": "engine.renderer_destroy", "tid": 1, "ts": 9}]
            return {"traceEvents": rows}
        analysis = {"status": "CPU_FIXTURE_ONLY", "scopes": [], "counter_controls": {}, "capture": {}}
        host = {"turns": 1, "worker_task": "0"}
        self.assertEqual(validate_scope_projection(document(True), analysis, "cancel-exit", host)
            ["cpu_reclamation_path"], "BACKEND_STOP_CPU_RELEASE_OBSERVED")
        self.assertEqual(validate_scope_projection(document(False), analysis, "cancel-exit", host)
            ["cpu_reclamation_path"], "READY_COLLECT_BEFORE_STOP_OBSERVED_AFTER_STOP_BRANCH_UNAVAILABLE")
        for mutation in ("zero_only", "release_outside_destroy", "no_pending"):
            raw = document(True)
            if mutation == "zero_only":
                raw["traceEvents"] = [e for e in raw["traceEvents"] if e.get("ts") != 3]
                next(e for e in raw["traceEvents"] if e["name"] == names[-1])["args"]["value"] = 0
            elif mutation == "release_outside_destroy":
                next(e for e in raw["traceEvents"] if e["name"] == "engine.renderer_destroy" and e["ph"] == "B")["ts"] = 8.5
            else:
                next(e for e in raw["traceEvents"] if e["name"] == names[4])["args"]["value"] = 0
            with self.assertRaises(ValueError):
                validate_scope_projection(raw, analysis, "cancel-exit", host)


if __name__ == "__main__":
    unittest.main()
