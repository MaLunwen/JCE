"""CPU controls for natural-session orchestration; no native renderer claims."""
import argparse
import hashlib
import struct
import zlib
import csv
import io
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

from tools import editor_play_diagnostic as diagnostic
from tools import editor_play_screenshots as screenshots

ROOT = Path(diagnostic.__file__).resolve().parents[1]


def context(frame, begin, duration=500, *, focused=True, game=True, width=960):
    viewport = {"source_id": 1, "flags": 255, "cpu_outcome": 3, "refusal_reason": 0,
                "requested_size": [width, 540], "allocated_size": [width, 540],
                "display_size": [width, 540], "texture_index": 7, "image_count": 1,
                "queued_commands": 21, "main_submits": 21, "recording_submits": 0}
    return {"v": 3, "window_present": True,
            "flags_begin": 0x200 if focused else 0, "flags_end": 0x200 if focused else 0,
            "point_size": [1600, 900], "drawable_size": [1600, 900],
            "native_size_begin": [1600, 900], "native_size_end": [1600, 900],
            "backend_frame_begin": frame, "backend_frame_end": frame + 1,
            "viewport_reported": game, "viewports": [viewport] if game else [],
            "clock": {"domain": "jce-performance-counter", "frequency_hz": "1000",
                      "begin": str(begin), "end": str(begin + duration)}}


def csv_fixture(*, omit_runtime=False, contexts=None):
    buffer = io.StringIO(newline="")
    writer = csv.writer(buffer, lineterminator="\n")
    writer.writerow(["frame_index", "phase", "ms", "context_json"])
    contexts = contexts or [context(i, i * 500) for i in range(63)]
    for i, packet in enumerate(contexts):
        duration = (int(packet["clock"]["end"]) - int(packet["clock"]["begin"]))
        writer.writerow([i, "frame_wall", duration, json.dumps(packet, separators=(",", ":"))])
        writer.writerow([i, "frame_cpu", 8, ""])
        writer.writerow([i, "frame_gpu", 0, ""])
        writer.writerow([i, "frame_backend_latched_wait_render", 0, ""])
        writer.writerow([i, "renderer_submit", 5, ""])
        if not omit_runtime and i >= 21:
            writer.writerow([i, "runtime_tick", 12 if i < 42 else 0, ""])
            writer.writerow([i, "physics", 3 if i < 42 else 0, ""])
    return buffer.getvalue().encode("utf-8"), len(contexts)


def writer_log(raw, count, *, missing=False, delta=0):
    size = len(raw) - raw.index(b"\n") - 1
    result = "jce_renderer: renderer: Direct3D 12 (graphics tier stable) at test:1\n"
    if not missing:
        result += (f"frame KPI capture complete: accepted={count} written={count} "
                   f"accepted_bytes={size + delta} written_bytes={size + delta} "
                   f"peak_bytes={size} peak_records={count}\n")
    return result


class DiagnosticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.authority = diagnostic.authorities(ROOT)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def sample(self, **kwargs):
        raw, count = csv_fixture(**kwargs)
        path = self.directory / "frames.csv"
        path.write_bytes(raw)
        return self.authority.read_frames(path, warmup=0, sample_count=None,
                                          allow_zero_wall=True)

    def process(self):
        return {"timed_out": False, "returncode": 0, "stdout": "", "stderr": ""}

    def test_child_environment_preserves_natural_workload_and_parent(self):
        parent = {"JCE_RENDER_BACKEND": "D3D12", "JCE_LAUNCH_VSYNC": "1",
                  "JCE_LAUNCH_TARGET_FPS": "60", "JCE_EDITOR_PROFILE_DIR": "local-profile",
                  "JCE_FRAME_DT_FIXED": "0.1", "JCE_INPUT_REPLAY": "old-replay",
                  "JCE_KPI_GAME_VIEW_WIDTH": "1280", "JCE_DBG_FOCUS_GAME": "1",
                  "JCE_DBG_AUTOPLAY": "1", "JCE_TRACE_STREAM": "secret-trace-path",
                  "JCE_TRACE_EXPORT": "secret-export-path", "JCE_CUSTOM_SECRET": "secret-value"}
        original = dict(parent)
        child, receipt = diagnostic.child_environment(parent, Path("scene"), self.directory, 120)
        self.assertEqual(parent, original)
        for key in ("JCE_RENDER_BACKEND", "JCE_LAUNCH_VSYNC", "JCE_LAUNCH_TARGET_FPS",
                    "JCE_EDITOR_PROFILE_DIR", "JCE_CUSTOM_SECRET"):
            self.assertEqual(child[key], parent[key])
        for key in ("JCE_FRAME_DT_FIXED", "JCE_INPUT_REPLAY", "JCE_KPI_GAME_VIEW_WIDTH",
                    "JCE_DBG_FOCUS_GAME", "JCE_DBG_AUTOPLAY", "JCE_TRACE_STREAM", "JCE_TRACE_EXPORT"):
            self.assertNotIn(key, child)
        self.assertEqual(child["JCE_KPI_FRAME_LOG_VIEWPORT"], "natural")
        self.assertEqual(child["JCE_FRAME_LOG_CONTEXT"], "3")
        self.assertEqual(child["JCE_TRACE"], "0")
        self.assertNotIn("secret-value", json.dumps(receipt))
        self.assertNotIn("secret-trace-path", json.dumps(receipt))

    def test_detail_options_are_explicit_exact_and_bounded(self):
        parent = {"JCE_PERF_SCRIPT_DETAIL": "1", "JCE_PERF_SCRIPT_PATH": "inherited.lua",
                  "JCE_PERF_ECULL_DETAIL": "1", "JCE_LAUNCH_VSYNC": "1"}
        child, changes = diagnostic.child_environment(parent, Path("scene"), self.directory, 120)
        for key in parent:
            if key.startswith("JCE_PERF_"):
                self.assertNotIn(key, child)
                self.assertIn(key, changes["removed_keys"])
        self.assertEqual(diagnostic.detail_options(), ({}, None))
        path = "scripts/Exact Case.lua"
        child, _ = diagnostic.child_environment(parent, Path("scene"), self.directory, 120,
                                                script_path=path, ecull_detail=True)
        self.assertEqual(child["JCE_PERF_SCRIPT_DETAIL"], "1")
        self.assertEqual(child["JCE_PERF_SCRIPT_PATH"], path)
        self.assertEqual(child["JCE_PERF_ECULL_DETAIL"], "1")
        self.assertEqual(child["JCE_LAUNCH_VSYNC"], "1")
        self.assertEqual(parent["JCE_PERF_SCRIPT_PATH"], "inherited.lua")
        _, metadata = diagnostic.detail_options(path, True)
        self.assertEqual(metadata["mode"], "detailed_cpu_attribution")
        self.assertEqual(metadata["script_path"], path)
        for invalid in ("", "a" * 256, "x\n.lua", "x\0.lua", "x\x7f.lua", 3):
            with self.subTest(invalid=repr(invalid)), self.assertRaises(ValueError):
                diagnostic.detail_options(invalid)
        diagnostic.detail_options("a" * 255)
        with self.assertRaises(ValueError):
            diagnostic.detail_options("\u4e2d" * 86)
        with self.assertRaises(ValueError):
            diagnostic.detail_options(None, "1")

    def test_invalid_detail_refuses_before_paths_outputs_or_process(self):
        args = argparse.Namespace(script_path="", ecull_detail=False)
        with self.assertRaisesRegex(ValueError, "script_path"):
            diagnostic.capture(args, SimpleNamespace())

    def test_cli_forwards_detail_selectors(self):
        with patch.object(diagnostic, "capture", return_value={"status": "CAPTURE_COMPLETE"}) as call:
            self.assertEqual(diagnostic.main(["--jce-root", "root", "--exe", "exe",
                                             "--scene", "scene", "--out", "out",
                                             "--script-path", "exact/path.lua", "--ecull-detail"]), 0)
        self.assertEqual(call.call_args.args[0].script_path, "exact/path.lua")
        self.assertIs(call.call_args.args[0].ecull_detail, True)

    def test_detail_observation_distinguishes_request_zero_and_actual_partition(self):
        _, request = diagnostic.detail_options("scripts/focus.lua", True)
        samples = self.sample()
        count = len(samples["frame_indices"])
        unavailable = diagnostic.observe_details(samples, "", request)
        self.assertEqual(unavailable["script"]["status"], "UNAVAILABLE")
        self.assertEqual(unavailable["ecull"]["status"], "UNAVAILABLE")
        for name in ("script", "script_focus", "script_other", *diagnostic.ECULL_DETAIL_PHASES):
            samples["phase_ms"][name] = [0.0] * count
        receipt = "script detail v1: selector=exact scope=on_update path_bytes=17 path=scripts/focus.lua at jce_rt_script_cost.c:32"
        observed = diagnostic.observe_details(samples, receipt, request)
        self.assertEqual(observed["script"]["status"], "OBSERVED")
        self.assertEqual(observed["script"]["same_frame_checked"], count)
        self.assertEqual(observed["script"]["max_abs_sum_error_ms"], 0)
        self.assertEqual(observed["ecull"]["status"], "OBSERVED")
        self.assertIn("script_focus", observed["script"]["observed_phase_names"])
        self.assertEqual(diagnostic.observe_details(samples, "", request)["script"]["status"], "INCOMPLETE")
        wrong = receipt.replace("scripts/focus.lua at", "scripts/focus.lua.extra at")
        self.assertEqual(diagnostic.observe_details(samples, wrong, request)["script"]["native_selector_receipts"], 0)
        self.assertEqual(diagnostic.observe_details(samples, receipt + "\n" + receipt, request)
                         ["script"]["status"], "INCOMPLETE")

    def test_detail_partition_rounding_partial_and_mismatch_remain_separate(self):
        _, request = diagnostic.detail_options("scripts/focus.lua", True)
        samples = self.sample()
        count = len(samples["frame_indices"])
        for name, value in (("script", 3.0001), ("script_focus", 1.0), ("script_other", 2.0)):
            samples["phase_ms"][name] = [value] * count
        receipt = "script detail v1: selector=exact scope=on_update path_bytes=17 path=scripts/focus.lua at jce_rt_script_cost.c:32"
        self.assertEqual(diagnostic.observe_details(samples, receipt, request)["script"]["status"], "OBSERVED")
        samples["phase_ms"]["script_focus"][2] = 2.0
        samples["phase_ms"]["script_other"][3] = math.nan
        samples["phase_ms"]["script_focus"][4] = math.nan
        samples["phase_ms"]["script_other"][4] = math.nan
        samples["phase_ms"]["ecull_world"] = [0.0] * count
        observed = diagnostic.observe_details(samples, receipt, request)
        self.assertEqual(observed["script"]["status"], "INCOMPLETE")
        self.assertEqual(observed["script"]["sum_mismatch_frames"], 1)
        self.assertEqual(observed["script"]["partial_partition_frames"], 2)
        self.assertEqual(observed["script"]["first_mismatch_frame_indices"], [2])
        self.assertEqual(observed["ecull"]["status"], "INCOMPLETE")
        self.assertEqual(observed["ecull"]["observed_phase_sample_counts"], {"ecull_world": count})

    def test_real_parser_spans_settle_and_missing_gpu(self):
        result = diagnostic.analyze_frames(self.sample())
        spans = result["steady_spans"]
        self.assertEqual([s["steady_frames"] for s in spans], [11, 11, 11])
        self.assertEqual([s["first_frame"] for s in spans], [10, 31, 52])
        self.assertEqual(result["comparison_status"], "OBSERVED_BOTH_TICK_MODES")
        self.assertEqual(spans[1]["wall"]["p50_ms"], 500)
        self.assertEqual(spans[1]["backend_cpu_snapshot"]["p50_ms"], 8)
        self.assertNotIn("cpu_frame", spans[1])
        self.assertIsNone(spans[0]["backend_gpu_snapshot"]["p50_ms"])
        self.assertEqual(spans[0]["backend_gpu_snapshot"]["zero_samples"], 11)
        self.assertIsNone(spans[0]["backend_latched"]["wait_render"]["p95_ms"])
        self.assertEqual(spans[1]["top_cpu_phases_not_additive"][0]["phase"], "runtime_tick")

    def test_actual_clock_not_frame_count_controls_settling(self):
        packets = [context(i, i * 6000, 10) for i in range(3)]
        spans = diagnostic.analyze_frames(self.sample(contexts=packets))["steady_spans"]
        self.assertEqual(spans[0]["first_frame"], 1)
        self.assertEqual(spans[0]["steady_frames"], 2)

    def test_lifecycle_and_other_view_transition_frames_are_excluded(self):
        samples = self.sample()
        for name, frame in (("ed_play_restore", 41), ("evp_scene_render", 62)):
            samples["phase_ms"][name] = [math.nan] * len(samples["frame_context"])
            samples["phase_ms"][name][frame] = 61.0
        result = diagnostic.analyze_frames(samples)
        self.assertEqual([s["steady_frames"] for s in result["steady_spans"]], [11, 10, 10])
        self.assertEqual([s["last_frame"] for s in result["steady_spans"]], [20, 40, 61])
        self.assertEqual(result["excluded_frame_counts"]["play_lifecycle_transition"], 1)
        self.assertEqual(result["excluded_frame_counts"]
                         ["scene_view_also_rendered_during_game_observation"], 1)

    def test_sparse_event_does_not_rank_as_recurring_frame_cost(self):
        samples = self.sample()
        samples["phase_ms"]["single_event"] = [math.nan] * len(samples["frame_context"])
        samples["phase_ms"]["single_event"][35] = 100.0
        span = diagnostic.analyze_frames(samples)["steady_spans"][1]
        self.assertNotIn("single_event", [row["phase"] for row in span["top_cpu_phases_not_additive"]])
        event = next(row for row in span["sparse_cpu_phases_not_additive"]
                     if row["phase"] == "single_event")
        self.assertEqual(event["finite_samples"], 1)
        self.assertEqual(event["mean_ms"], 100.0)
        self.assertEqual(event["unavailable_samples"], 10)
        self.assertAlmostEqual(event["observed_frame_fraction"], 1/11)

    def test_background_and_absent_game_reset_spans(self):
        packets = [context(i, i * 500, focused=i != 10, game=i != 31) for i in range(63)]
        result = diagnostic.analyze_frames(self.sample(contexts=packets))
        self.assertEqual(result["excluded_frame_counts"]["background_hidden_or_minimized"], 1)
        self.assertEqual(result["excluded_frame_counts"]["game_observation_unavailable"], 1)
        self.assertEqual([s["first_frame"] for s in result["steady_spans"]], [52])
        self.assertEqual(result["comparison_status"], "INSUFFICIENT_COMPARABLE_TICK_MODES")

    def test_different_render_extents_cannot_be_comparison(self):
        packets = [context(i, i * 500, width=1200 if 21 <= i < 42 else 960) for i in range(63)]
        result = diagnostic.analyze_frames(self.sample(contexts=packets))
        self.assertEqual(result["comparison_status"], "INSUFFICIENT_COMPARABLE_TICK_MODES")

    def test_writer_accounting_and_missing_writer_fail_closed(self):
        raw, count = csv_fixture()
        (self.directory / "frames.csv").write_bytes(raw)
        for log, expected in ((writer_log(raw, count, missing=True), "complete native KPI"),
                              (writer_log(raw, count, delta=1), "payload bytes"),
                              (writer_log(raw, count + 1), "CSV frame count"),
                              (writer_log(raw, count) * 2, "complete native KPI")):
            with self.subTest(expected=expected):
                (self.directory / "engine.log").write_text(log)
                with self.assertRaisesRegex(RuntimeError, expected):
                    diagnostic.validate_capture(self.authority, self.directory, self.process())
        (self.directory / "engine.log").write_text(writer_log(raw, count))
        parsed, integrity = diagnostic.validate_capture(self.authority, self.directory, self.process())
        self.assertEqual(len(parsed["frame_ms"]), count)
        self.assertEqual(integrity["actual_backend"], "Direct3D 12")

    def test_actual_csv_authority_rejects_gap_duplicate_and_clock_mismatch(self):
        raw, _ = csv_fixture()
        mutations = (raw.replace(b"\n1,frame_wall,", b"\n2,frame_wall,", 1),
                     raw + b"62,renderer_submit,5,\n",
                     raw.replace(b"0,frame_wall,500,", b"0,frame_wall,501,", 1))
        for altered in mutations:
            with self.subTest():
                path = self.directory / "bad.csv"
                path.write_bytes(altered)
                with self.assertRaises(ValueError):
                    self.authority.read_frames(path, warmup=0, sample_count=None)

    def test_incomplete_child_outcomes_fail_before_parsing(self):
        for process, expected in (({"timed_out": True, "returncode": 0}, "watchdog"),
                                  ({"timed_out": False, "returncode": 9}, "nonzero")):
            with self.assertRaisesRegex(RuntimeError, expected):
                diagnostic.validate_capture(self.authority, self.directory, process)

    def run_mock_child(self, *, missing_writer=False, detailed=False):
        raw, count = csv_fixture()
        fake = self.directory / "mock_editor.py"
        # The mock produces synthetic observations; actual process/log/CSV owners execute.
        fake.write_text("import os\nfrom pathlib import Path\n"
                        "assert os.environ['JCE_KPI_FRAME_LOG_VIEWPORT'] == 'natural'\n"
                        "assert 'JCE_DBG_AUTOPLAY' not in os.environ\n"
                        "assert os.environ['JCE_LAUNCH_VSYNC'] == '1'\n"
                        + ("assert os.environ['JCE_PERF_SCRIPT_DETAIL'] == '1'\n"
                           "assert os.environ['JCE_PERF_SCRIPT_PATH'] == 'scripts/focus.lua'\n"
                           "assert os.environ['JCE_PERF_ECULL_DETAIL'] == '1'\n" if detailed else "") +
                        f"Path(os.environ['JCE_KPI_FRAME_LOG']).write_bytes({raw!r})\n"
                        f"Path(os.environ['JCE_LOG_FILE']).write_text({writer_log(raw, count, missing=missing_writer)!r})\n")
        scene = self.directory / "fixture.scene.json"
        scene.write_text("{}\n")
        args = argparse.Namespace(jce_root=ROOT, exe=fake, scene=scene,
                                  out=self.directory / "capture", seconds=30,
                                  script_path="scripts/focus.lua" if detailed else None,
                                  ecull_detail=detailed)
        def launch(command, **kwargs):
            self.assertEqual(command, [str(fake)])
            self.assertEqual(kwargs["cwd"], ROOT)
            return self.authority.run_process([sys.executable, *command], **kwargs)
        api = SimpleNamespace(**vars(self.authority))
        api.run_process, api.editor_alive = launch, lambda: False
        with patch.dict(os.environ, {"JCE_DBG_AUTOPLAY": "1", "JCE_LAUNCH_VSYNC": "1"}):
            report = diagnostic.capture(args, api)
        return report, args, api

    def test_owned_child_capture_and_archive_only_session_files(self):
        report, args, api = self.run_mock_child()
        self.assertEqual(report["status"], "CAPTURE_COMPLETE")
        self.assertEqual(report["inputs"]["exe"]["sha256"], diagnostic.sha256(args.exe))
        self.assertGreater(report["process"]["process_identity"]["pid"], 0)
        self.assertIn("platform", report["host"])
        self.assertEqual(report["integrity"]["native_frame_clock"]["frequency_hz"], "1000")
        with zipfile.ZipFile(args.out.with_name("capture.zip")) as bundle:
            self.assertIn("frames.csv", bundle.namelist())
            self.assertNotIn("fixture.scene.json", bundle.namelist())
            self.assertNotIn("mock_editor.py", bundle.namelist())
        before = (args.out / "report.json").read_bytes()
        with self.assertRaises(FileExistsError):
            diagnostic.capture(args, api)
        self.assertEqual((args.out / "report.json").read_bytes(), before)

    def test_owned_detailed_child_retains_mode_in_request_and_final_archive(self):
        report, args, _ = self.run_mock_child(detailed=True)
        self.assertEqual(report["status"], "CAPTURE_COMPLETE")
        expected = report["detail_request"]
        self.assertEqual(expected["mode"], "detailed_cpu_attribution")
        self.assertEqual(expected["script_path"], "scripts/focus.lua")
        self.assertIs(expected["ecull_detail"], True)
        # This real synthetic child deliberately models an old executable:
        # complete CSV capture does not certify support for the new selectors.
        self.assertEqual(report["detail_observation"]["script"]["status"], "UNAVAILABLE")
        self.assertEqual(report["detail_observation"]["ecull"]["status"], "UNAVAILABLE")
        with zipfile.ZipFile(args.out.with_name("capture.zip")) as archive:
            for name in ("request.json", "report.json"):
                self.assertEqual(json.loads(archive.read(name))["detail_request"], expected)

    def test_real_child_missing_writer_retains_failed_capture(self):
        report, args, _ = self.run_mock_child(missing_writer=True)
        self.assertEqual(report["status"], "FAILED")
        self.assertIn("complete native KPI", report["failure"])
        self.assertNotIn("analysis", report)
        self.assertTrue(args.out.with_name("capture.zip").is_file())

    def test_help_without_checkout_dependencies(self):
        copied = self.directory / "standalone.py"
        copied.write_bytes(Path(diagnostic.__file__).read_bytes())
        result = subprocess.run([sys.executable, "-I", str(copied), "--help"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--jce-root", result.stdout)



# Generated PNG/protocol controls below are not native rendering evidence.
def synthetic_png(width=2, height=1):
    def chunk(kind, value):
        return struct.pack(">I", len(value)) + kind + value + struct.pack(">I", zlib.crc32(kind + value) & 0xffffffff)
    raw = (b"\0" + bytes((30, 90, 150, 255)) * width) * height
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def line(payload):
    return "2026-01-01 00:00:00.000 [MAIN] INFO - editor.game_view: " + payload + " at capture.cpp:10\n"


def receipts(shot, *, begin=16500, end=17200, frequency=1000, success=True, terminal=True):
    result = line(f"Game View capture admitted source_frame=33 submit_frame=34 due_seconds={shot['due_play_seconds']:.9f} "
                  f"admitted_seconds={shot['due_play_seconds'] + .2:.9f} counter={begin} frequency={frequency} path={shot['native_path']}")
    if terminal:
        result += line(f"Game View capture terminal success={int(success)} source_frame=33 submit_frame=34 "
                       f"counter={end} frequency={frequency} path={shot['native_path']}")
    return result


class ScreenshotTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.api = diagnostic.authorities(ROOT)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.out = Path(self.temp.name) / "capture at space"
        self.out.mkdir()
        (self.out / "screenshots").mkdir()
        self.shots = screenshots.plan(self.out)
        raw, _ = csv_fixture()
        path = self.out / "frames.csv"
        path.write_bytes(raw)
        self.samples = self.api.read_frames(path, warmup=0, sample_count=None, allow_zero_wall=True)

    def collect(self, log):
        return screenshots.collect(self.out, self.shots, log, self.samples["frame_context"])

    def write_png(self, shot=0, data=None):
        path = self.out / self.shots[shot]["relative_path"]
        path.write_bytes(synthetic_png() if data is None else data)
        return path

    def test_default_plan_is_bounded_natural_and_environment_stays_owned(self):
        self.assertEqual([s["due_play_seconds"] for s in self.shots], [6, 12, 24, 36, 42, 48, 54, 60])
        env = screenshots.environment(self.shots)
        self.assertEqual(env["JCE_KPI_GAME_CAPTURE_SIZE"], "natural")
        self.assertEqual(len(env["JCE_KPI_GAME_SHOTS"].split(";")), 8)
        parent = {"JCE_RENDER_BACKEND": "D3D12", "JCE_LAUNCH_TARGET_FPS": "60",
                  "JCE_LAUNCH_VSYNC": "1", "JCE_SHOT_PATH": "unowned.png", "JCE_WINCAP_FRAME": "10"}
        child, receipt = diagnostic.child_environment(parent, Path("scene"), self.out, 120, env)
        self.assertNotIn("JCE_SHOT_PATH", child)
        self.assertNotIn("JCE_WINCAP_FRAME", child)
        self.assertEqual(child["JCE_RENDER_BACKEND"], "D3D12")
        self.assertEqual(child["JCE_LAUNCH_VSYNC"], "1")
        self.assertEqual(child["JCE_LAUNCH_TARGET_FPS"], "60")
        self.assertNotIn("JCE_DBG_FOCUS_GAME", child)
        self.assertNotIn("unowned.png", json.dumps(receipt))
        with self.assertRaises(ValueError):
            screenshots.plan(self.out / "invalid;path")

    def test_completed_native_receipt_and_valid_png_are_both_required(self):
        path = self.write_png()
        log = receipts(self.shots[0]) + line("Game View capture schedule ended reason=stopped elapsed_seconds=7.000000000")
        result = self.collect(log)
        self.assertEqual(result["status"], "PARTIAL")
        self.assertEqual(result["complete_images"], 1)
        self.assertEqual(result["archive_members"], [self.shots[0]["relative_path"]])
        row = result["shots"][0]
        self.assertEqual(row["status"], "complete")
        self.assertEqual((row["file"]["width"], row["file"]["height"]), (2, 1))
        self.assertEqual(row["file"]["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
        self.assertEqual(row["observed_capture_latency_ms"], 700)
        self.assertTrue(all(r["status"] == "not_reached" for r in result["shots"][1:]))
        without_receipt = self.collect("")
        self.assertEqual(without_receipt["archive_members"], [self.shots[0]["relative_path"]])
        self.assertEqual(without_receipt["status"], "UNVERIFIED_IMAGES")
        self.assertEqual(without_receipt["unverified_images"], 1)
        self.assertEqual(without_receipt["shots"][0]["status"], "unobserved")

    def test_pending_and_failed_receipts_never_become_completed_images(self):
        self.write_png()
        pending = self.collect(receipts(self.shots[0], terminal=False))
        self.assertEqual(pending["shots"][0]["status"], "unresolved")
        self.assertEqual(pending["archive_members"], [self.shots[0]["relative_path"]])
        self.assertFalse(pending["shots"][0]["file"]["receipt_verified"])
        self.assertIn(62, pending["performance_exclusion"]["frame_indices"])
        failed = self.collect(receipts(self.shots[0], success=False))
        self.assertEqual(failed["shots"][0]["status"], "failed")
        self.assertEqual(failed["archive_members"], [self.shots[0]["relative_path"]])
        self.assertFalse(failed["shots"][0]["file"]["receipt_verified"])

    def test_missing_invalid_png_and_symlink_fail_without_losing_timing(self):
        log = receipts(self.shots[0])
        for data in (None, synthetic_png()[:-1], synthetic_png()[:35] + b"broken"):
            with self.subTest(data=data):
                path = self.out / self.shots[0]["relative_path"]
                if path.exists():
                    path.unlink()
                if data is not None:
                    path.write_bytes(data)
                result = self.collect(log)
                self.assertEqual(result["shots"][0]["status"], "file_missing_or_invalid")
                self.assertEqual(result["archive_members"], [])
                self.assertTrue(result["performance_exclusion"]["available"])
        outside = Path(self.temp.name) / "unowned.png"
        outside.write_bytes(synthetic_png())
        path.unlink()
        try:
            path.symlink_to(outside)
        except (OSError, NotImplementedError):
            return  # Platform lacks symlink creation; other ownership cases ran.
        result = self.collect(log)
        self.assertEqual(result["archive_members"], [])
        self.assertEqual(result["shots"][0]["file"]["status"], "invalid")

    def test_png_crc_and_deflate_are_checked_beyond_signature_and_dimensions(self):
        valid = synthetic_png()
        bad_crc = bytearray(valid)
        bad_crc[45] ^= 1
        with self.assertRaisesRegex(ValueError, "CRC"):
            screenshots.png_receipt(self.write_png(data=bad_crc))
        # Valid chunk CRC with an invalid compressed pixel stream.
        offset = valid.index(b"IDAT")
        length = struct.unpack_from(">I", valid, offset - 4)[0]
        payload = b"x" * length
        changed = valid[:offset + 4] + payload + struct.pack(">I", zlib.crc32(b"IDAT" + payload) & 0xffffffff) + valid[offset + 8 + length:]
        with self.assertRaises((ValueError, zlib.error)):
            screenshots.png_receipt(self.write_png(data=changed))

    def test_duplicate_identity_wrong_clock_and_unowned_paths_are_unavailable(self):
        self.write_png()
        base = receipts(self.shots[0])
        variants = (base + base, base.replace("submit_frame=34 counter=", "submit_frame=35 counter="),
                    base.replace("frequency=1000", "frequency=999"),
                    base.replace(self.shots[0]["native_path"], str(Path(self.temp.name) / "outside.png")),
                    base.replace("[MAIN]", "[OTHER]"))
        for log in variants:
            with self.subTest(log=log):
                result = self.collect(log)
                self.assertFalse(result["performance_exclusion"]["available"])
                self.assertTrue(result["issues"])

    def test_exclusion_splits_samples_without_a_second_five_second_settle(self):
        self.write_png()
        result = self.collect(receipts(self.shots[0], begin=16600, end=17200))
        excluded = result["performance_exclusion"]["frame_indices"]
        self.assertEqual(excluded, [33, 34])
        original = diagnostic.analyze_frames(self.samples)
        filtered = diagnostic.analyze_frames(self.samples, screenshot_frames=excluded)
        self.assertEqual([s["steady_frames"] for s in original["steady_spans"]], [11, 11, 11])
        self.assertEqual([s["steady_frames"] for s in filtered["steady_spans"]], [11, 2, 7, 11])
        self.assertEqual([s["first_frame"] for s in filtered["steady_spans"]], [10, 31, 35, 52])
        self.assertEqual(filtered["excluded_frame_counts"]["recorded_screenshot_activity"], 2)
        self.assertEqual(filtered["excluded_frame_counts"]["settling_after_state_extent_or_visibility_change"], 30)

    def test_terminal_after_stop_and_touching_interval_boundary_are_retained(self):
        self.write_png()
        emitted = receipts(self.shots[0], begin=16500, end=17000).splitlines(keepends=True)
        log = emitted[0] + line("Game View capture schedule ended reason=stopped elapsed_seconds=6.300000000") + emitted[1]
        result = self.collect(log)
        self.assertEqual(result["shots"][0]["status"], "complete")
        self.assertEqual(result["performance_exclusion"]["frame_indices"], [32, 33, 34])
        self.assertEqual(result["shots"][1]["status"], "not_reached")
        no_admission = self.collect(line("Game View capture schedule ended reason=shutdown elapsed_seconds=7.000000000"))
        self.assertEqual(no_admission["shots"][0]["status"], "not_admitted")
        self.assertEqual(no_admission["shots"][1]["status"], "not_reached")
        self.assertFalse(no_admission["performance_exclusion"]["available"])

    def test_explicit_timing_only_control_keeps_raw_selection(self):
        result = screenshots.collect(self.out, [], "", self.samples["frame_context"])
        self.assertEqual(result["status"], "DISABLED")
        self.assertEqual(result["archive_members"], [])
        self.assertTrue(result["performance_exclusion"]["available"])
        self.assertEqual(screenshots.environment([]), {})
        self.assertEqual(diagnostic.analyze_frames(self.samples), diagnostic.analyze_frames(
            self.samples, screenshot_frames=result["performance_exclusion"]["frame_indices"]))

    def test_owned_child_archives_verified_and_unverified_owned_pngs(self):
        output = Path(self.temp.name) / "native-style-output"
        raw, count = csv_fixture()
        first = screenshots.plan(output)[0]
        second = screenshots.plan(output)[1]
        observed = writer_log(raw, count) + receipts(first, begin=16600, end=17200) + line("Game View capture schedule ended reason=stopped elapsed_seconds=7.000000000")
        fake = Path(self.temp.name) / "generated_editor.py"
        fake.write_text("import os\nfrom pathlib import Path\n"
                        "assert os.environ['JCE_KPI_GAME_CAPTURE_SIZE'] == 'natural'\n"
                        "assert 'JCE_DBG_AUTOPLAY' not in os.environ\n"
                        "assert 'JCE_SHOT_PATH' not in os.environ\n"
                        f"Path(os.environ['JCE_KPI_FRAME_LOG']).write_bytes({raw!r})\n"
                        f"Path(os.environ['JCE_LOG_FILE']).write_text({observed!r})\n"
                        f"Path({first['native_path']!r}).write_bytes({synthetic_png()!r})\n"
                        f"Path({second['native_path']!r}).write_bytes({synthetic_png()!r})\n"
                        f"Path({str(output / 'screenshots/unowned.png')!r}).write_bytes({synthetic_png()!r})\n")
        scene = Path(self.temp.name) / "synthetic.scene.json"
        scene.write_text("{}\n")
        args = argparse.Namespace(jce_root=ROOT, exe=fake, scene=scene, out=output, seconds=30)
        api = SimpleNamespace(**vars(self.api))
        api.editor_alive = lambda: False
        api.run_process = lambda command, **kwargs: self.api.run_process([sys.executable, *command], **kwargs)
        with patch.dict(os.environ, {"JCE_SHOT_PATH": "unowned.png"}):
            report = diagnostic.capture(args, api)
        self.assertEqual(report["status"], "CAPTURE_COMPLETE")
        self.assertEqual(report["screenshots"]["status"], "PARTIAL")
        self.assertEqual(report["screenshots"]["complete_images"], 1)
        self.assertEqual(report["screenshots"]["archived_images"], 2)
        self.assertEqual(report["screenshots"]["unverified_images"], 1)
        self.assertEqual([s["steady_frames"] for s in report["analysis"]["steady_spans"]], [11, 11, 11])
        self.assertEqual(report["analysis_excluding_screenshots"]["status"], "UNAVAILABLE")
        with zipfile.ZipFile(output.with_name(output.name + ".zip")) as archive:
            self.assertIn("screenshots.json", archive.namelist())
            self.assertIn(first["relative_path"], archive.namelist())
            self.assertNotIn("screenshots/unowned.png", archive.namelist())
            self.assertIn(second["relative_path"], archive.namelist())
            manifest = json.loads(archive.read("screenshots.json"))
            self.assertFalse(manifest["shots"][1]["file"]["receipt_verified"])
            self.assertEqual(archive.read(first["relative_path"]), synthetic_png())


if __name__ == "__main__":
    unittest.main()
