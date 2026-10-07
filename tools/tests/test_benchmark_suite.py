"""Negative controls for the raw benchmark measurement gate."""
import tempfile
import math
import unittest
from unittest import mock
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.benchmark_suite import (aggregate_runs, capture_frame, compare_variants,
                                   fixed_game_render_environment, game_capture_environment,
                                   frame_summary, read_frames, soak_windows,
                                   visual_delta_fraction)
from tools.benchmark_suite import main as suite_main
from tools.benchmark_suite import (source_from_manifest, simulation_started,
                                   host_render_environment, draw_activity_valid,
                                   scatter_workload_valid, player_counters)
from tools.benchmark_scene import scene_document, write_scene
from tools.perf_bench import has_exact_workload, read_execution_log
from tools.perf_bench import default_editor_exe, _editor_alive
from tools.process_sampler import run_process
import json
import os
import subprocess


class ExecutionLogControls(unittest.TestCase):
    def test_rotated_log_uses_fresh_stderr_without_mirror_duplication(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "engine.log"
            marker = "benchmark-spawn: kind=0 requested=1000 spawned=1000"
            renderer = "jce_renderer: renderer: Direct3D 11 (graphics tier stable)"
            current = "JCE_MAX_SECONDS reached"
            path.write_text(current)
            path.with_suffix(".log.1.zst").write_text("jce_renderer: renderer: OpenGL\nold failure")
            full = renderer + "\n" + marker + "\n" + current
            text = read_execution_log(path, {"stdout": "", "stderr": "\x1b[96m" + full + "\x1b[0m"})
            self.assertTrue(has_exact_workload(text, 0, 1000))
            from tools.jce_determinism import assert_backend
            assert_backend(text, "d3d11")
            self.assertNotIn("old failure", text)
            self.assertEqual(text.count(current), 1)
            repeated = read_execution_log(path, {"stdout": full, "stderr": full + "\n" + marker})
            self.assertFalse(has_exact_workload(repeated, 0, 1000))

    def test_missing_and_conflicting_backend_and_stderr_error_remain_visible(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "engine.log"
            path.write_text("steady perf")
            from tools.jce_determinism import assert_backend
            with self.assertRaises(RuntimeError):
                assert_backend(read_execution_log(path, {"stdout": "", "stderr": ""}), "d3d11")
            with self.assertRaises(RuntimeError):
                read_execution_log(path, {"stdout": "jce_renderer: renderer: Direct3D 11", "stderr": "jce_renderer: renderer: OpenGL"})
            text = read_execution_log(path, {"stdout": "", "stderr": "[MAIN] ERROR - failed"})
            self.assertIn("] ERROR -", text)

    def test_shared_reader_uses_exact_execution_capture_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "engine.log"
            path.write_text("last perf")
            captures = path.parent / "engine_process"
            captures.mkdir()
            (captures / "stdout.log").write_text("")
            (captures / "stderr.log").write_text("benchmark-player: entered Play\nlast perf")
            text = read_execution_log(path)
            self.assertIn("benchmark-player: entered Play", text)
            self.assertEqual(text.count("last perf"), 1)


class BenchmarkSamplesTest(unittest.TestCase):
    def test_player_resources_require_actual_ordered_lifecycle_samples(self):
        initial = {"frame": 0, "simulation_seconds": 0, "monotonic_ms": 100,
                   "event": "initialized", "gpu_available": False,
                   "engine": {"entity_count_available": True, "entity_count": 1003,
                              "memory_current_bytes": 1000, "memory_peak_bytes": 1500}}
        final = dict(initial, frame=421, simulation_seconds=6.8, monotonic_ms=3000, event="exit")
        def logs(rows):
            return "\n".join(json.dumps(row) for row in rows)
        self.assertEqual(len(player_counters(logs([initial, final]))), 2)
        for rows in ([initial], [final, initial], [initial, initial, final],
                     [initial, dict(final, frame=0)], [initial, dict(final, simulation_seconds=-1)],
                     [initial, dict(final, engine={"entity_count_available": False})]):
            with self.subTest(rows=rows), self.assertRaises(RuntimeError):
                player_counters(logs(rows))

    def test_scatter_chunks_require_full_actual_count_for_each_view(self):
        def logs(counts):
            return "\n".join("%d instances scattered" % count for count in counts)
        self.assertTrue(scatter_workload_valid(logs([65536, 34464]), 100000))
        self.assertTrue(scatter_workload_valid(logs([65536, 34464] * 2), 100000))
        self.assertTrue(scatter_workload_valid(logs([65536]), 65536))
        for counts in ([], [65536], [34464], [65536, 34463], [65536, 34464, 65536]):
            self.assertFalse(scatter_workload_valid(logs(counts), 100000))

    def test_direct_cli_import_does_not_collide_with_installed_build_package(self):
        result = subprocess.run([sys.executable,
                                 str(Path(__file__).resolve().parents[1] / "benchmark_suite.py"),
                                 "--help"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--host", result.stdout)

    def test_default_editor_path_uses_build_matrix_for_native_architecture(self):
        with mock.patch("tools.perf_bench.HOST", "darwin"), \
                mock.patch("tools.perf_bench.host_arch", return_value="aarch64"):
            self.assertEqual(default_editor_exe().parts[-4:],
                             ("desktop", "macos-arm64", "release", "jce_editor"))
        with mock.patch("tools.perf_bench.HOST", "windows"), \
                mock.patch("tools.perf_bench.host_arch", return_value="i686"):
            self.assertEqual(default_editor_exe().parts[-4:],
                             ("desktop", "windows-x86", "release", "jce_editor.exe"))

    def test_posix_lock_probe_does_not_call_windows_tasklist(self):
        with mock.patch.dict(sys.modules, {"psutil": None}), \
                mock.patch("tools.perf_bench.os.name", "posix"), \
                mock.patch("tools.perf_bench.subprocess.run") as command:
            command.return_value.stdout = "other_app\njce_editor\n"
            self.assertTrue(_editor_alive())
            self.assertEqual(command.call_args.args[0], ["ps", "-A", "-o", "comm="])

    def test_draw_gate_honours_instancing_without_accepting_inactive_work(self):
        self.assertFalse(draw_activity_valid("drawcall", 13, 10, 1000))
        self.assertTrue(draw_activity_valid("drawcall", 1010, 10, 1000))
        self.assertTrue(draw_activity_valid("entity", 13, 10, 1000))
        self.assertTrue(draw_activity_valid("physics", 13, 10, 1000))
        self.assertFalse(draw_activity_valid("entity", 10, 10, 1000))
        self.assertFalse(draw_activity_valid("physics", 0, 10, 1000))

    def test_host_marker_cannot_mislabel_editor_as_player(self):
        self.assertTrue(simulation_started("autoplay: entered Play", "editor"))
        self.assertFalse(simulation_started("autoplay: entered Play", "player"))
        self.assertTrue(simulation_started("benchmark-player: entered Play", "player"))
        self.assertFalse(simulation_started("benchmark-player: entered Play", "editor"))

    def test_player_timing_has_no_capture_and_witness_uses_player_target(self):
        directory = Path("run")
        counters = {"JCE_BENCH_COUNTERS": str(directory / "engine_counters.jsonl")}
        self.assertEqual(host_render_environment("player", directory), counters)
        self.assertEqual(host_render_environment("player", directory, witness=True),
                         {**counters, "JCE_BENCH_PLAYER_SHOT": str(directory / "frame.png")})

    def test_player_cannot_implicitly_use_editor_executable(self):
        with mock.patch("tools.benchmark_suite.run") as process:
            with self.assertRaises(SystemExit):
                suite_main(["--host", "player"])
            process.assert_not_called()

    def test_baseline_identity_accepts_existing_flat_manifests(self):
        identity = source_from_manifest({"executable_sha256": "abc",
                                         "engine_commit": "baseline",
                                         "engine_version": "0.12.4",
                                         "source_dirty": False}, "abc")
        self.assertEqual(identity["commit"], "baseline")
        self.assertFalse(identity["source_dirty"])
        with self.assertRaises(ValueError):
            source_from_manifest({"executable_sha256": "abc"}, "abc")

    def test_executable_comparison_rejects_misattributed_source_manifest(self):
        with tempfile.TemporaryDirectory() as holder:
            directory = Path(holder)
            a, b = directory / "a.exe", directory / "b.exe"
            a.write_bytes(b"build A")
            b.write_bytes(b"build B")
            manifest = directory / "manifest.json"
            manifest.write_text(json.dumps({"executable_sha256": "wrong",
                                             "source": {"commit": "old"}}))
            with mock.patch("tools.benchmark_suite.source_identity", return_value={}), \
                    mock.patch("tools.benchmark_suite.machine_manifest", return_value={}), \
                    mock.patch("tools.benchmark_suite.run_control") as launch:
                with self.assertRaises(SystemExit):
                    suite_main(["--exe", str(a), "--compare-exe", str(b),
                                "--compare-manifest", str(manifest)])
            launch.assert_not_called()

    def test_exit_only_marker_cannot_claim_first_frame_latency(self):
        with tempfile.TemporaryDirectory() as holder:
            child = mock.Mock(returncode=0)
            child.poll.return_value = 0
            with mock.patch("tools.process_sampler.subprocess.Popen", return_value=child), \
                    mock.patch("tools.process_sampler._process_reader", return_value=None), \
                    mock.patch("tools.process_sampler._has_frame_marker", return_value=True) as marker:
                result = run_process(["unused"], env={}, cwd=holder, timeout=10,
                                     output_dir=holder, marker_path=Path(holder) / "raw.csv")
            marker.assert_not_called()
            self.assertIsNone(result["first_frame_s"])
            self.assertEqual(result["first_frame_probe"], "UNAVAILABLE")

    def test_witness_and_timing_keep_size_with_separate_capture_clocks(self):
        witness = game_capture_environment(30, Path("frame.png"))
        timing = fixed_game_render_environment(Path("timing"))
        self.assertEqual(witness["JCE_KPI_GAME_CAPTURE_SIZE"],
                         timing["JCE_KPI_GAME_CAPTURE_SIZE"])
        self.assertLess(float(witness["JCE_KPI_GAME_SHOTS"].split("|")[0]), 1)
        self.assertGreater(float(timing["JCE_KPI_GAME_SHOTS"].split("|")[0]), 86400)

    def test_failed_process_retains_exit_artifacts(self):
        with tempfile.TemporaryDirectory() as holder:
            directory = Path(holder)
            result = run_process([sys.executable, "-c", "print('failed control'); raise SystemExit(7)"],
                                 env=os.environ.copy(), cwd=directory, timeout=10,
                                 output_dir=directory, interval=0.02)
            self.assertEqual(result["returncode"], 7)
            self.assertFalse(result["timed_out"])
            stored = json.loads((directory / "result.json").read_text())
            self.assertEqual(stored["returncode"], 7)
            self.assertTrue((directory / "samples.json").is_file())

    def test_timed_out_process_is_reaped_and_recorded(self):
        with tempfile.TemporaryDirectory() as holder:
            result = run_process([sys.executable, "-c", "import time; time.sleep(5)"],
                                 env=os.environ.copy(), cwd=holder, timeout=0.1,
                                 output_dir=holder, interval=0.02)
            self.assertTrue(result["timed_out"])
            self.assertNotEqual(result["returncode"], 0)

    def test_workload_marker_rejects_prefix_and_duplicate_spawn(self):
        marker = "benchmark-spawn: kind=0 requested=1000 spawned=1000"
        self.assertTrue(has_exact_workload(marker + " at source.c:1", 0, 1000))
        self.assertFalse(has_exact_workload(marker + "0", 0, 1000))
        self.assertFalse(has_exact_workload(marker + "\n" + marker, 0, 1000))
        self.assertFalse(has_exact_workload(marker, 1, 1000))

    def test_camera_contains_scatter_field(self):
        for count in (1000, 5000, 100000):
            camera = scene_document("instancing", count)["entities"][0]
            # Scatter's authored X/Z extent is +/- sqrt(count), plus mesh bounds.
            view_half_height = camera["components"][1]["orthoSize"] * 0.5
            self.assertGreater(view_half_height, math.sqrt(count) + 1.0)

    def test_soak_windows_retain_every_sample(self):
        samples = {"frame_ms": [1000, 2000, 500, 1000],
                   "cpu_frame_ms": [None] * 4, "gpu_frame_ms": [None] * 4,
                   "frame_indices": [0, 1, 2, 3], "phase_ms": {}}
        windows = soak_windows(samples, seconds=3)
        self.assertEqual([x["frame"]["sample_count"] for x in windows], [2, 2])
        self.assertEqual(windows[-1]["end_s"], 4.5)

    def test_capture_stays_outside_steady_window(self):
        for warmup in (30, 120):
            self.assertGreaterEqual(capture_frame(warmup), 20)
            self.assertLess(capture_frame(warmup) + 1, warmup)
        with self.assertRaises(ValueError):
            capture_frame(10)

    def test_ab_pairs_require_matching_indices(self):
        rows = [{"kind": "physics", "variant": variant, "run_index": index,
                 "status": "PASS", "frame": {"frame_median_ms": value}}
                for variant, index, value in (("A", 0, 10), ("B", 0, 15),
                                               ("A", 1, 20), ("B", 1, 10))]
        result = compare_variants(rows, ["physics"], 2)["physics"]
        self.assertEqual(result["paired_b_over_a"], [1.5, 0.5])
        rows[-1]["run_index"] = 2
        self.assertEqual(compare_variants(rows, ["physics"], 2)["physics"]
                         ["status"], "UNAVAILABLE")

    def test_phase_series_stay_aligned_and_missing_wall_fails(self):
        path = self._csv("frame_index,phase,ms\n0,frame_wall,10\n"
                         "1,frame_wall,10\n1,physics,2\n2,frame_wall,10\n")
        samples = read_frames(path, warmup=0, sample_count=None)
        physics = samples["phase_ms"]["physics"]
        self.assertTrue(math.isnan(physics[0]))
        self.assertEqual(physics[1], 2)
        self.assertTrue(math.isnan(physics[2]))
        path = self._csv("frame_index,phase,ms\n0,physics,1\n")
        with self.assertRaisesRegex(ValueError, "no frame_wall"):
            read_frames(path, warmup=0, sample_count=1)

    def _csv(self, text):
        holder = tempfile.TemporaryDirectory()
        self.addCleanup(holder.cleanup)
        path = Path(holder.name) / "samples.csv"
        path.write_text(text, encoding="utf-8")
        return path

    def test_complete_samples_and_percentiles(self):
        path = self._csv(
            "frame_index,phase,ms\n"
            "0,frame_wall,99\n"
            "1,frame_wall,10\n"
            "1,frame_cpu,8\n"
            "2,frame_wall,20\n"
            "2,frame_cpu,9\n")
        samples = read_frames(path, warmup=1, sample_count=2)
        self.assertEqual(list(samples["frame_ms"]), [10, 20])
        self.assertTrue(all(math.isnan(x) for x in samples["gpu_frame_ms"]))
        summary = frame_summary(samples)
        self.assertEqual(summary["frame_p99_ms"], 20)
        self.assertEqual(summary["frame_median_ms"], 15)
        self.assertEqual(summary["one_percent_low_fps"], 50)

    def test_stale_or_short_output_fails(self):
        path = self._csv("frame_index,phase,ms\n0,frame_wall,10\n")
        with self.assertRaisesRegex(ValueError, "only 1 frame"):
            read_frames(path, warmup=1, sample_count=1)

    def test_gap_fails(self):
        path = self._csv(
            "frame_index,phase,ms\n0,frame_wall,10\n2,frame_wall,12\n")
        with self.assertRaisesRegex(ValueError, "gaps"):
            read_frames(path, warmup=0, sample_count=2)

    def test_duplicate_fails(self):
        path = self._csv(
            "frame_index,phase,ms\n0,frame_wall,10\n0,frame_wall,11\n")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            read_frames(path, warmup=0, sample_count=1)

    def test_nonfinite_fails(self):
        path = self._csv("frame_index,phase,ms\n0,frame_wall,nan\n")
        with self.assertRaisesRegex(ValueError, "invalid value"):
            read_frames(path, warmup=0, sample_count=1)

    def test_aggregate_never_invents_missing_repeats(self):
        rows = [{"kind": "physics", "run_id": "a", "status": "PASS",
                 "frame": {"frame_median_ms": 10, "frame_p99_ms": 14}},
                {"kind": "physics", "run_id": "b", "status": "PASS",
                 "frame": {"frame_median_ms": 12, "frame_p99_ms": 16}}]
        self.assertEqual(aggregate_runs(rows, ["physics"], 3)["physics"]
                         ["status"], "UNAVAILABLE")
        result = aggregate_runs(rows, ["physics"], 2)["physics"]
        self.assertEqual(result["median_of_frame_medians_ms"], 11)
        self.assertTrue(result["noise_warning"])

    def test_visual_liveness_rejects_identical_capture(self):
        blank = bytearray(100 * 100 * 3)
        workload = bytearray(blank)
        for y in range(20, 55):
            for x in range(30, 65):
                offset = (y * 100 + x) * 3
                workload[offset:offset + 3] = b"\xff\xff\xff"
        control = (100, 100, 3, blank)
        changed = (100, 100, 3, workload)
        self.assertEqual(visual_delta_fraction(control, control), 0)
        self.assertGreater(visual_delta_fraction(control, changed), 0.02)

    def test_fixture_is_immutable(self):
        holder = tempfile.TemporaryDirectory()
        self.addCleanup(holder.cleanup)
        path = Path(holder.name) / "fixture.scene.json"
        first = write_scene(path, "drawcall", 100)
        self.assertEqual(first.read_bytes(), write_scene(
            path, "drawcall", 100).read_bytes())
        with self.assertRaisesRegex(ValueError, "different content"):
            write_scene(path, "drawcall", 200)


if __name__ == "__main__":
    unittest.main()
