"""Negative controls for the raw benchmark measurement gate."""
import tempfile
import csv
import io
import math
import inspect
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


class EditorProfileIsolationControls(unittest.TestCase):
    class StopAtTiming(RuntimeError):
        pass

    def setUp(self):
        scratch = Path(__file__).resolve().parents[2] / "build/tests"
        scratch.mkdir(parents=True, exist_ok=True)
        holder = tempfile.TemporaryDirectory(prefix="benchmark-profile-", dir=scratch)
        self.addCleanup(holder.cleanup)
        self.root = Path(holder.name)
        self.home = self.root / "fake-home"
        self.user_profile = self.home / ".jce"
        self.user_profile.mkdir(parents=True)
        for name in ("editor-preferences.json", "editor-session.json", "imgui.ini"):
            (self.user_profile / name).write_bytes(b"Original user state\n")
        self.before = self.snapshot(self.home)

    @staticmethod
    def snapshot(directory):
        return {path.relative_to(directory).as_posix(): path.read_bytes()
                for path in directory.rglob("*") if path.is_file()}

    def boot_settings(self, environment):
        profile = Path(environment.get("JCE_EDITOR_PROFILE_DIR", str(self.user_profile)))
        profile.mkdir(parents=True, exist_ok=True)
        (profile / "editor-preferences.json").write_bytes(b"Benchmark process settings\n")
        return profile

    def test_editor_profiles_are_absolute_independent_and_shared_with_witness(self):
        with mock.patch.dict(os.environ, {"HOME": str(self.home),
                                          "JCE_EDITOR_PROFILE_DIR": str(self.user_profile)}):
            first = host_render_environment("editor", self.root / "run-a", witness=True)
            timing = host_render_environment("editor", self.root / "run-a")
            second = host_render_environment("editor", self.root / "run-b")
            profile = Path(first["JCE_EDITOR_PROFILE_DIR"])
            self.assertTrue(profile.is_absolute())
            self.assertTrue(profile.is_dir())
            self.assertEqual(profile, (self.root / "run-a/editor-profile").resolve())
            self.assertEqual(first["JCE_EDITOR_PROFILE_DIR"], timing["JCE_EDITOR_PROFILE_DIR"])
            self.assertNotEqual(first["JCE_EDITOR_PROFILE_DIR"], second["JCE_EDITOR_PROFILE_DIR"])
            self.assertEqual(os.environ["HOME"], str(self.home))
        self.assertEqual(self.snapshot(self.home), self.before)

    def test_control_and_measurement_do_not_touch_home_with_or_without_capture(self):
        from tools import benchmark_suite as suite
        for measurement in (False, True):
            for capture in (False, True):
                with self.subTest(measurement=measurement, capture=capture):
                    directory = self.root / ("measurement-%s-%s" % (measurement, capture))
                    calls = []

                    def launch(environment, log_path, **kwargs):
                        self.boot_settings(environment)
                        calls.append(dict(environment))
                        if Path(log_path).name == "visual.log":
                            Path(log_path).write_text("autoplay: entered Play\neditor_config: profile directory: "
                                                     + environment.get("JCE_EDITOR_PROFILE_DIR", str(self.user_profile))
                                                     + "\n", encoding="utf-8")
                            return {}
                        raise self.StopAtTiming()

                    kwargs = dict(exe=self.root / "editor.exe", scene=self.root / "scene.json",
                                  kind="entity", count=1000, warmup=120, sample_count=600,
                                  backend="d3d11", out=self.root, timeout=60, spread=0,
                                  directory=directory, host="editor", capture=capture)
                    if measurement:
                        kwargs.update(profile="publication", index=0, identity={}, hardware={},
                                      executable_sha256="test-only", control={})
                    with mock.patch.dict(os.environ, {"HOME": str(self.home),
                                                      "JCE_EDITOR_PROFILE_DIR": str(self.user_profile)}), \
                            mock.patch.object(suite, "run", side_effect=launch), \
                            mock.patch.object(suite, "png_dimensions", return_value=suite.GAME_SIZE):
                        with self.assertRaises(self.StopAtTiming):
                            (suite.run_one if measurement else suite.run_control)(**kwargs)
                    self.assertEqual(len(calls), 2 if capture else 1)
                    expected = str((directory / "editor-profile").resolve())
                    self.assertTrue(all(row.get("JCE_EDITOR_PROFILE_DIR") == expected for row in calls))
                    intent = json.loads((directory / "intent.json").read_text(encoding="utf-8"))
                    self.assertEqual(intent["environment"]["JCE_EDITOR_PROFILE_DIR"], expected)
                    self.assertEqual(self.snapshot(self.home), self.before)

    def test_explicit_profile_survives_shared_runner_environment_cleanup(self):
        from tools import perf_bench
        directory = self.root / "shared-runner"
        environment = host_render_environment("editor", directory)
        environment["JCE_MULTI_INSTANCE"] = "1"
        observed = {}

        def child(command, *, env, **kwargs):
            observed.update(env)
            self.boot_settings(env)
            raise self.StopAtTiming()

        with mock.patch.dict(os.environ, {"HOME": str(self.home),
                                          "JCE_EDITOR_PROFILE_DIR": str(self.user_profile)}), \
                mock.patch.object(perf_bench, "run_process", side_effect=child):
            with self.assertRaises(self.StopAtTiming):
                perf_bench.run(environment, directory / "engine.log", exe=self.root / "editor.exe")
        self.assertEqual(observed["JCE_EDITOR_PROFILE_DIR"], environment["JCE_EDITOR_PROFILE_DIR"])
        self.assertEqual(observed["HOME"], str(self.home))
        self.assertEqual(self.snapshot(self.home), self.before)

    def test_profile_confirmation_rejects_missing_wrong_prefix_and_conflicting_markers(self):
        from tools import benchmark_suite as suite
        expected = str((self.root / "confirmed/editor-profile").resolve())
        marker = "editor_config: profile directory: "
        valid = "INFO - " + marker + expected + " at jce_editor_config.cpp:42\n"
        self.assertTrue(suite.editor_profile_confirmed(valid, expected))
        self.assertTrue(suite.editor_profile_confirmed(valid + valid, expected))
        for log in ("", "profile directory: " + expected,
                    marker + expected + "-other\n", marker + expected + "/child\n",
                    valid + marker + str(self.user_profile) + "\n"):
            with self.subTest(log=log):
                self.assertFalse(suite.editor_profile_confirmed(log, expected))

    def test_profile_confirmation_preserves_at_directory_names(self):
        from tools import benchmark_suite as suite
        directory = self.root / "owner at location"
        selected = host_render_environment("editor", directory)["JCE_EDITOR_PROFILE_DIR"]
        marker = "editor_config: profile directory: "
        self.assertTrue(Path(selected).is_dir())
        self.assertTrue(suite.editor_profile_confirmed(marker + selected + "\n", selected))
        for source in ("jce_editor_config.cpp:42", "relative/jce_editor_config.cpp:17"):
            log = marker + selected + " at " + source + "\n"
            self.assertTrue(suite.editor_profile_confirmed(log, selected))
            self.assertFalse(suite.editor_profile_confirmed(log, str(self.user_profile)))
        self.assertEqual(self.snapshot(self.home), self.before)

    def test_visual_control_and_measurement_require_actual_profile_confirmation(self):
        from tools import benchmark_suite as suite
        for host_call in ("visual", "control", "measurement"):
            for reported in (None, str(self.user_profile)):
                with self.subTest(host_call=host_call, reported=reported):
                    directory = self.root / (host_call + ("-missing" if reported is None else "-wrong"))
                    calls = []

                    def launch(environment, log_path, **kwargs):
                        calls.append(dict(environment))
                        text = "autoplay: entered Play\n"
                        if reported is not None:
                            text += "editor_config: profile directory: " + reported + "\n"
                        Path(log_path).write_text(text, encoding="utf-8")
                        return {}

                    with mock.patch.object(suite, "run", side_effect=launch), \
                            mock.patch.object(suite, "png_dimensions", return_value=suite.GAME_SIZE), \
                            mock.patch.object(suite, "read_frames", return_value={}), \
                            mock.patch.object(suite, "frame_summary", return_value={}):
                        with self.assertRaisesRegex((ValueError, suite.CorrectnessFailure), "isolated editor profile"):
                            if host_call == "visual":
                                directory.mkdir()
                                suite.run_visual_witness({}, directory, exe=self.root / "editor.exe", timeout=60)
                            else:
                                kwargs = dict(exe=self.root / "editor.exe", scene=self.root / "scene.json",
                                              kind="entity", count=1000, warmup=120, sample_count=600,
                                              backend="d3d11", out=self.root, timeout=60, spread=0,
                                              directory=directory, host="editor", capture=False)
                                if host_call == "measurement":
                                    kwargs.update(profile="publication", index=0, identity={}, hardware={},
                                                  executable_sha256="test-only", control={})
                                (suite.run_one if host_call == "measurement" else suite.run_control)(**kwargs)
                    self.assertEqual(len(calls), 1)
                    self.assertEqual(self.snapshot(self.home), self.before)

    def test_player_has_no_editor_profile_or_profile_directory(self):
        directory = self.root / "player"
        for witness in (False, True):
            environment = host_render_environment("player", directory, witness=witness)
            self.assertNotIn("JCE_EDITOR_PROFILE_DIR", environment)
        self.assertFalse(directory.exists())


class IsolatedEngineConfigControls(unittest.TestCase):
    class StopAtLaunch(RuntimeError):
        pass

    def setUp(self):
        scratch = Path(__file__).resolve().parents[2] / "build" / "tests"
        scratch.mkdir(parents=True, exist_ok=True)
        holder = tempfile.TemporaryDirectory(prefix="benchmark-config-", dir=scratch)
        self.addCleanup(holder.cleanup)
        self.root = Path(holder.name)
        self.baseline = self.root / ".config" / "jce.ini"
        self.baseline.parent.mkdir()
        self.original = ("[window]\nTitle = Caf" + chr(233) + " 100%\nwidth = 640\n"
                         "[renderer]\nvsync = false\nbackend_resolved = d3d11\n"
                         "[custom]\nCaseSensitive = %(literal)s\n"
                         "[logging]\nlevel = info\ncolors = false\n").encode("utf-8")
        self.baseline.write_bytes(self.original)
        self.exe = self.root / "binary" / "benchmark.exe"
        self.exe.parent.mkdir()

    @staticmethod
    def values(path):
        import configparser
        parser = configparser.ConfigParser(interpolation=None, strict=False, delimiters=("=",))
        parser.optionxform = str
        parser.read_string(Path(path).read_text(encoding="utf-8"))
        return {(section, key): value for section in parser.sections()
                for key, value in parser.items(section)}

    def context(self, directory, **options):
        from tools import benchmark_suite as suite
        with mock.patch.object(suite, "ROOT", self.root):
            return suite.isolated_debug_context(directory, self.exe, **options)

    def test_root_config_clone_preserves_every_other_value_and_original_bytes(self):
        import hashlib
        fallback = self.exe.parent / ".config" / "jce.ini"
        fallback.parent.mkdir()
        fallback.write_text("[window]\nwidth=999\n", encoding="utf-8")
        context = self.context(self.root / "measurement")
        config = Path(context["engine_config_file"])
        intended = self.values(self.baseline)
        intended["logging", "level"] = "debug"
        self.assertEqual(self.values(config), intended)
        self.assertEqual(self.baseline.read_bytes(), self.original)
        self.assertEqual(fallback.read_text(encoding="utf-8"), "[window]\nwidth=999\n")
        self.assertEqual(context["effective_baseline"], str(self.baseline.resolve()))
        self.assertEqual(context["effective_baseline_sha256"], hashlib.sha256(self.original).hexdigest())
        self.assertEqual(context["engine_config_sha256"], hashlib.sha256(config.read_bytes()).hexdigest())
        self.assertEqual(context["selected_logging_level"], "debug")
        self.assertEqual(context["observer_mode"], "debug_instrumented_diagnostic")
        self.assertFalse(context["quiet_production_throughput"])
        self.assertTrue(config.is_absolute())
        self.assertTrue(Path(context["cwd"]).is_absolute())
        self.assertEqual(config, Path(context["cwd"]) / ".config" / "jce.ini")
        self.assertNotIn(b"\r", config.read_bytes())

    def test_executable_config_fallback_and_unconfigured_defaults(self):
        self.baseline.unlink()
        fallback = self.exe.parent / ".config" / "jce.ini"
        fallback.parent.mkdir()
        fallback.write_bytes(self.original)
        context = self.context(self.root / "fallback-run")
        self.assertEqual(context["effective_baseline"], str(fallback.resolve()))
        expected = self.values(fallback)
        expected["logging", "level"] = "debug"
        self.assertEqual(self.values(context["engine_config_file"]), expected)
        self.assertEqual(fallback.read_bytes(), self.original)
        fallback.unlink()
        default = self.context(self.root / "default-run")
        self.assertEqual(default["effective_baseline"], "engine_defaults")
        self.assertIsNone(default["effective_baseline_sha256"])
        self.assertEqual(self.values(default["engine_config_file"]), {("logging", "level"): "debug"})

    def test_fresh_contexts_are_independent_with_matched_settings_and_no_overwrite(self):
        first = self.context(self.root / "control")
        second = self.context(self.root / "measurement")
        self.assertNotEqual(first["cwd"], second["cwd"])
        for key in ("effective_baseline_sha256", "engine_config_sha256", "selected_logging_level", "observer_mode"):
            self.assertEqual(first[key], second[key])
        with self.assertRaises(FileExistsError):
            self.context(self.root / "control")
        self.assertEqual(self.baseline.read_bytes(), self.original)

    def test_c_ignored_rows_and_duplicate_assignments_remain_original_bytes(self):
        fixtures = (b"[window]\nwidth:999\nheight=480\n; original comment\n[logging]\nlevel:off\n",
                    b"[DEFAULT]\nwidth=640\n[window]\nheight=480\n",
                    b"[logging]\nlevel=info\nlevel=warn\n")
        for index, raw in enumerate(fixtures):
            with self.subTest(raw=raw):
                self.baseline.write_bytes(raw)
                context = self.context(self.root / ("preserved-%d" % index))
                copied = Path(context["engine_config_file"]).read_bytes()
                self.assertTrue(copied.startswith(raw))
                self.assertEqual(copied[len(raw):], b"[logging]\nlevel=debug\n")
                self.assertEqual(self.baseline.read_bytes(), raw)
                self.assertNotIn(b"width = 999", copied)
                self.assertNotIn(b"level = off", copied)

    def test_nul_config_is_rejected_without_creating_child_config(self):
        raw = b"[logging]\nlevel=info\x00ignored bytes"
        self.baseline.write_bytes(raw)
        directory = self.root / "nul-input"
        with self.assertRaisesRegex(ValueError, "NUL"):
            self.context(directory)
        self.assertFalse((directory / "process-cwd").exists())
        self.assertEqual(self.baseline.read_bytes(), raw)

    def test_clean_config_has_declared_fixed_backend_and_auto_is_rejected(self):
        from tools import benchmark_suite as suite
        self.baseline.unlink()
        context = self.context(self.root / "clean", backend="D3D11")
        self.assertEqual(context["requested_backend"], "d3d11")
        self.assertEqual(context["intended_overrides"], {"logging.level": "debug", "renderer.backend": "d3d11"})
        self.assertEqual(Path(context["engine_config_file"]).read_bytes(),
                         b"[logging]\nlevel=debug\n[renderer]\nbackend=d3d11\n")
        self.assertEqual(suite.validate_observer_config(context), context["engine_config_sha256"])
        for backend in ("auto", "unknown"):
            with self.subTest(backend=backend), self.assertRaisesRegex(ValueError, "fixed.*backend"):
                self.context(self.root / backend, backend=backend)

    def test_actual_config_guard_rejects_each_child_boundary_and_failed_child_mutation(self):
        from tools import benchmark_suite as suite
        for route in ("visual", "control", "measurement"):
            for boundary in ("before", "after", "failed-child"):
                with self.subTest(route=route, boundary=boundary):
                    directory = self.root / (route + "-" + boundary)
                    calls = []
                    if route == "visual":
                        directory.mkdir()
                        context = self.context(directory, backend="d3d11")
                        if boundary == "before":
                            Path(context["engine_config_file"]).write_bytes(b"changed")
                    original_host_environment = suite.host_render_environment

                    def host_environment(host, evidence, **kwargs):
                        environment = original_host_environment(host, evidence, **kwargs)
                        if boundary == "before":
                            (Path(evidence) / "process-cwd/.config/jce.ini").write_bytes(b"changed")
                        return environment

                    def child(environment, log_path, **kwargs):
                        calls.append(str(log_path))
                        (Path(kwargs["cwd"]) / ".config/jce.ini").write_bytes(b"changed")
                        if boundary == "failed-child":
                            raise RuntimeError("deliberate child failure")
                        return {}

                    with mock.patch.object(suite, "ROOT", self.root), \
                            mock.patch.object(suite, "host_render_environment", side_effect=host_environment), \
                            mock.patch.object(suite, "run", side_effect=child):
                        with self.assertRaisesRegex(suite.CorrectnessFailure, "configuration.*changed") as caught:
                            if route == "visual":
                                suite.run_visual_witness({"JCE_BACKEND": "d3d11"}, directory,
                                                         exe=self.exe, timeout=60, observer=context)
                            else:
                                options = dict(exe=self.exe, scene=self.root / "scene.json", kind="entity",
                                               count=1000, warmup=120, sample_count=600, backend="d3d11",
                                               out=self.root, timeout=60, spread=0, directory=directory,
                                               host="editor", capture=False)
                                if route == "measurement":
                                    options.update(profile="publication", index=0, identity={}, hardware={},
                                                   executable_sha256="test-only", control={})
                                (suite.run_one if route == "measurement" else suite.run_control)(**options)
                    self.assertEqual(len(calls), 0 if boundary == "before" else 1)
                    if boundary == "failed-child":
                        self.assertIn("deliberate child failure", str(caught.exception.__context__))
                    self.assertEqual(self.baseline.read_bytes(), self.original)

    def test_missing_child_config_is_rejected_without_reseeding(self):
        from tools import benchmark_suite as suite
        directory = self.root / "missing-copy"
        context = self.context(directory, backend="d3d11")
        config = Path(context["engine_config_file"])
        config.unlink()
        with mock.patch.object(suite, "run") as child:
            with self.assertRaisesRegex(suite.CorrectnessFailure, "unavailable"):
                suite.run_observer_child({}, directory / "engine.log", context,
                                         phase="timing", exe=self.exe)
            child.assert_not_called()
        self.assertFalse(config.exists())
        second = self.context(self.root / "deleted-after-child", backend="d3d11")

        def delete_config(*args, **kwargs):
            Path(second["engine_config_file"]).unlink()
            return {}

        with mock.patch.object(suite, "run", side_effect=delete_config):
            with self.assertRaisesRegex(suite.CorrectnessFailure, "unavailable"):
                suite.run_observer_child({}, self.root / "after.log", second,
                                         phase="timing", exe=self.exe)
        self.assertFalse(Path(second["engine_config_file"]).exists())
        self.assertEqual(self.baseline.read_bytes(), self.original)

    def test_auto_diagnostic_cli_is_rejected_before_child_launch(self):
        from tools import benchmark_suite as suite
        for option in ("--backend", "--compare-backend"):
            with self.subTest(option=option), mock.patch.object(suite, "run") as child:
                with self.assertRaises(SystemExit):
                    suite.main([option, "AUTO"])
                child.assert_not_called()

    def test_control_and_measurement_share_context_with_witness_on_both_hosts(self):
        from tools import benchmark_suite as suite
        for host in ("editor", "player"):
            for measurement in (False, True):
                with self.subTest(host=host, measurement=measurement):
                    directory = self.root / (host + ("-measurement" if measurement else "-control"))
                    calls = []

                    def launch(environment, log_path, **kwargs):
                        calls.append((dict(environment), Path(log_path), kwargs))
                        if Path(log_path).name != "visual.log":
                            raise self.StopAtLaunch()
                        text = {"editor": "autoplay: entered Play", "player": "benchmark-player: entered Play"}[host]
                        if host == "editor":
                            text += "\neditor_config: profile directory: " + environment["JCE_EDITOR_PROFILE_DIR"]
                        Path(log_path).write_text(text + "\n", encoding="utf-8")
                        return {}

                    kwargs = dict(exe=self.exe, scene=Path("relative-fixture.scene.json"), kind="entity",
                                  count=1000, warmup=120, sample_count=600, backend="d3d11",
                                  out=self.root, timeout=60, spread=0, directory=directory,
                                  host=host, capture=True)
                    if measurement:
                        kwargs.update(profile="publication", index=0, identity={}, hardware={},
                                      executable_sha256="test-only", control={})
                    with mock.patch.object(suite, "ROOT", self.root), \
                            mock.patch.object(suite, "run", side_effect=launch), \
                            mock.patch.object(suite, "png_dimensions", return_value=suite.GAME_SIZE):
                        with self.assertRaises(self.StopAtLaunch):
                            (suite.run_one if measurement else suite.run_control)(**kwargs)
                    self.assertEqual(len(calls), 2)
                    intent = json.loads((directory / "intent.json").read_bytes())
                    observer = intent["observer"]
                    for environment, log_path, options in calls:
                        self.assertEqual(options["cwd"], observer["cwd"])
                        self.assertTrue(log_path.is_absolute())
                        self.assertTrue(Path(environment["JCE_SCENE"]).is_absolute())
                        self.assertEqual(self.values(Path(options["cwd"]) / ".config" / "jce.ini")["logging", "level"], "debug")
                        if host == "editor":
                            self.assertTrue(Path(environment["JCE_EDITOR_PROFILE_DIR"]).is_absolute())
                    self.assertEqual(self.baseline.read_bytes(), self.original)

    def test_measurement_rejects_missing_or_changed_control_config_binding(self):
        from tools import benchmark_suite as suite
        keys = ("effective_baseline_sha256", "engine_config_sha256", "selected_logging_level", "observer_mode",
                "requested_backend", "intended_overrides", "config_validation_policy")
        for index, key in enumerate(("missing", *keys, None)):
            with self.subTest(key=key):
                directory = self.root / ("binding-%d" % index)
                control = {}

                def launch(environment, log_path, **kwargs):
                    selected = json.loads((directory / "intent.json").read_bytes())["observer"]
                    if key != "missing":
                        control["observer"] = dict(selected)
                        if key is not None:
                            control["observer"][key] = "different"
                    Path(log_path).write_text("editor_config: profile directory: "
                                             + environment["JCE_EDITOR_PROFILE_DIR"]
                                             + "\n] ERROR - deliberate parser stop\n", encoding="utf-8")
                    return {}

                with mock.patch.object(suite, "ROOT", self.root), \
                        mock.patch.object(suite, "run", side_effect=launch), \
                        mock.patch.object(suite, "read_frames", return_value={}), \
                        mock.patch.object(suite, "frame_summary", return_value={}):
                    message = "engine logged an ERROR" if key is None else "configuration differs"
                    with self.assertRaisesRegex(suite.CorrectnessFailure, message):
                        suite.run_one(exe=self.exe, scene=self.root / "scene.json", kind="entity",
                                      count=1000, profile="publication", warmup=120, sample_count=600,
                                      backend="d3d11", out=self.root, index=0, identity={}, hardware={},
                                      timeout=60, spread=0, executable_sha256="test-only", control=control,
                                      capture=False, directory=directory, host="editor")
                self.assertEqual(self.baseline.read_bytes(), self.original)

    def test_shared_runner_optional_cwd_delegation_retains_default_root(self):
        from tools import perf_bench
        selected = self.root / "child"
        selected.mkdir()
        for cwd in (None, selected):
            with self.subTest(cwd=cwd):
                observed = {}

                def child(command, *, env, **kwargs):
                    observed.update(kwargs)
                    observed["env"] = env
                    raise self.StopAtLaunch()

                options = {} if cwd is None else {"cwd": cwd}
                with mock.patch.object(perf_bench, "run_process", side_effect=child):
                    with self.assertRaises(self.StopAtLaunch):
                        perf_bench.run({"JCE_MULTI_INSTANCE": "1"}, self.root / "engine.log",
                                       exe=self.exe, samples_path=self.root / "samples.csv", **options)
                self.assertEqual(observed["cwd"], str(perf_bench.ROOT if cwd is None else selected.resolve()))
                self.assertTrue(Path(observed["env"]["JCE_LOG_FILE"]).is_absolute())
                self.assertTrue(Path(observed["env"]["JCE_KPI_FRAME_LOG"]).is_absolute())
                self.assertTrue(Path(observed["marker_path"]).is_absolute())


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
        directory = Path("run").resolve()
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
            child = mock.Mock(pid=42, returncode=0)
            child.poll.return_value = 0
            with mock.patch("tools.process_sampler.subprocess.Popen", return_value=child), \
                    mock.patch("tools.process_sampler._process_reader", return_value=None), \
                    mock.patch("tools.process_sampler._has_frame_marker", return_value=True) as marker:
                result = run_process(["unused"], env={}, cwd=holder, timeout=10,
                                     output_dir=holder, marker_path=Path(holder) / "raw.csv")
            marker.assert_not_called()
            self.assertIsNone(result["first_frame_s"])
            self.assertEqual(result["first_frame_probe"], "UNAVAILABLE")
            self.assertIs(type(result["process_identity"]["pid"]), int)
            self.assertEqual(result["process_identity"]["pid"], 42)

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

    def test_backend_latched_rows_stay_separate_and_unavailable_is_not_zero(self):
        path = self._csv(
            "frame_index,phase,ms\n0,frame_wall,10\n0,physics,2\n"
            "0,frame_backend_latched_cpu_submit,0\n"
            "0,frame_backend_latched_wait_submit,7\n"
            "1,frame_wall,20\n1,physics,3\n"
            "1,frame_backend_latched_wait_render,100\n"
            "2,frame_wall,30\n"
            "2,frame_backend_latched_cpu_submit,6\n")
        samples = read_frames(path, warmup=0, sample_count=None)
        latched = samples["backend_latched_ms"]
        self.assertEqual(set(latched), {"cpu_submit", "wait_submit", "wait_render"})
        self.assertEqual(latched["cpu_submit"][0], 0)
        self.assertTrue(math.isnan(latched["cpu_submit"][1]))
        self.assertEqual(latched["cpu_submit"][2], 6)
        self.assertEqual(latched["wait_render"][1], 100)
        self.assertEqual(set(samples["phase_ms"]), {"physics"})
        summary = frame_summary(samples)
        self.assertEqual(summary["frame_median_ms"], 20)
        self.assertEqual(summary["phase_median_ms"], {"physics": 2.5})
        self.assertEqual(summary["backend_latched_median_ms"],
                         {"cpu_submit": 3, "wait_submit": 7, "wait_render": 100})
        self.assertIn("zero", summary["backend_latched_semantics"])
        self.assertIn("not frame-aligned", summary["backend_latched_semantics"])
        windows = soak_windows(samples, seconds=0.025)
        self.assertEqual(windows[0]["frame"]["backend_latched_median_ms"]["cpu_submit"], 0)
        self.assertEqual(windows[1]["frame"]["backend_latched_median_ms"]["cpu_submit"], 6)
        self.assertIsNone(windows[1]["frame"]["backend_latched_median_ms"]["wait_render"])

    def test_backend_latched_legacy_missing_warmup_and_invalid_controls(self):
        legacy = self._csv("frame_index,phase,ms\n0,frame_wall,10\n")
        samples = read_frames(legacy, warmup=0, sample_count=None)
        self.assertNotIn("backend_latched_ms", samples)
        self.assertNotIn("backend_latched_median_ms", frame_summary(samples))
        path = self._csv("frame_index,phase,ms\n0,frame_wall,10\n"
                         "0,frame_backend_latched_cpu_submit,99\n"
                         "1,frame_wall,20\n1,frame_backend_latched_cpu_submit,0\n")
        self.assertEqual(list(read_frames(path, warmup=1, sample_count=1)
                              ["backend_latched_ms"]["cpu_submit"]), [0])
        late = self._csv("frame_index,phase,ms\n0,frame_wall,10\n"
                         "1,frame_wall,20\n1,frame_backend_latched_wait_render,0\n"
                         "2,frame_wall,30\n")
        series = read_frames(late, warmup=0, sample_count=None)["backend_latched_ms"]["wait_render"]
        self.assertTrue(math.isnan(series[0])); self.assertEqual(series[1], 0)
        self.assertTrue(math.isnan(series[2]))
        extended = self._context_csv([self._context()]) + "0,frame_backend_latched_wait_render,0,\n"
        parsed = read_frames(self._csv(extended), warmup=0, sample_count=None)
        self.assertEqual(parsed["backend_latched_ms"]["wait_render"][0], 0)
        self.assertEqual(parsed["frame_context"], [self._context()])
        for value in ("nan", "inf", "-1"):
            path = self._csv("frame_index,phase,ms\n0,frame_wall,10\n"
                             "0,frame_backend_latched_wait_render," + value + "\n")
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "invalid value"):
                read_frames(path, warmup=0, sample_count=None)

    def test_backend_latched_diagnostics_cannot_change_ordinary_frame_metrics(self):
        base = ("frame_index,phase,ms\n0,frame_wall,10\n0,physics,1\n"
                "0,frame_cpu,3\n0,frame_gpu,2\n1,frame_wall,20\n1,physics,2\n"
                "1,frame_cpu,4\n1,frame_gpu,3\n")
        extended = base.replace("0,physics,1\n", "0,physics,1\n"
                                "0,frame_backend_latched_wait_render,10000\n")
        extended += "1,frame_backend_latched_cpu_submit,-0.0\n"
        original = frame_summary(read_frames(self._csv(base), warmup=0, sample_count=None))
        diagnostic = frame_summary(read_frames(self._csv(extended), warmup=0, sample_count=None))
        self.assertEqual(diagnostic.pop("backend_latched_median_ms"),
                         {"cpu_submit": 0, "wait_submit": None, "wait_render": 10000})
        self.assertIn("different threads", diagnostic.pop("backend_latched_semantics"))
        self.assertEqual(diagnostic, original)

    def test_backend_latched_invalid_warmup_and_unselected_tail_are_not_hidden(self):
        base = "frame_index,phase,ms\n0,frame_wall,10\n1,frame_wall,20\n2,frame_wall,30\n"
        for index in (0, 2):
            for value in ("nan", "inf", "-1"):
                with self.subTest(index=index, value=value):
                    bad = base.replace(f"{index},frame_wall,", f"{index},frame_backend_latched_cpu_submit,"
                                       f"{value}\n{index},frame_wall,")
                    with self.assertRaisesRegex(ValueError, "invalid value"):
                        read_frames(self._csv(bad), warmup=1, sample_count=1)
            row = f"{index},frame_backend_latched_wait_submit,0\n"
            duplicate = base.replace(f"{index},frame_wall,", row + row + f"{index},frame_wall,")
            with self.subTest(index=index), self.assertRaisesRegex(ValueError, "duplicate KPI phase"):
                read_frames(self._csv(duplicate), warmup=1, sample_count=1)

    def test_backend_latched_context_does_not_join_timers_to_backend_frame_ids(self):
        packet = self._context(backend_frame_begin=900, backend_frame_end=901)
        text = self._context_csv([packet]) + "0,frame_backend_latched_wait_render,1000,\n"
        samples = read_frames(self._csv(text), warmup=0, sample_count=None)
        self.assertEqual(samples["frame_context"], [packet])
        self.assertEqual(samples["backend_latched_ms"]["wait_render"][0], 1000)
        self.assertEqual(samples["frame_indices"][0], 0)
        self.assertEqual(samples["phase_ms"], {"physics": samples["phase_ms"]["physics"]})

    def _csv(self, text):
        holder = tempfile.TemporaryDirectory()
        self.addCleanup(holder.cleanup)
        path = Path(holder.name) / "samples.csv"
        path.write_text(text, encoding="utf-8", newline="\n")
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

    def test_default_steady_samples_still_reject_zero_wall(self):
        path = self._csv("frame_index,phase,ms\n0,frame_wall,0\n"
                         "1,frame_wall,10\n")
        with self.assertRaisesRegex(ValueError, "frame time is zero"):
            read_frames(path, warmup=0, sample_count=None)
        samples = read_frames(path, warmup=1, sample_count=1)
        self.assertEqual(list(samples["frame_ms"]), [10])

    def test_explicit_raw_capture_retains_zero_wall_intervals(self):
        path = self._csv("frame_index,phase,ms\n0,frame_wall,0\n"
                         "0,physics,0\n1,frame_wall,10\n"
                         "2,frame_wall,0\n3,frame_wall,0\n")
        # BEFORE must reach the actual zero rejection, not an unknown keyword.
        options = ({"allow_zero_wall": True}
                   if "allow_zero_wall" in inspect.signature(read_frames).parameters else {})
        samples = read_frames(path, warmup=0, sample_count=None, **options)
        self.assertEqual(list(samples["frame_indices"]), [0, 1, 2, 3])
        self.assertEqual(list(samples["frame_ms"]), [0, 10, 0, 0])
        self.assertEqual(samples["phase_ms"]["physics"][0], 0)

    def test_raw_zero_policy_still_rejects_negative_nonfinite_and_gaps(self):
        options = ({"allow_zero_wall": True}
                   if "allow_zero_wall" in inspect.signature(read_frames).parameters else {})
        for body in ("0,frame_wall,-1\n", "0,frame_wall,nan\n",
                     "0,frame_wall,inf\n", "0,frame_wall,1\n0,physics,-1\n",
                     "0,frame_wall,1\n2,frame_wall,2\n"):
            with self.subTest(body=body):
                path = self._csv("frame_index,phase,ms\n" + body)
                with self.assertRaisesRegex(ValueError, "invalid value|gaps"):
                    read_frames(path, warmup=0, sample_count=None, **options)

    @staticmethod
    def _context(**updates):
        packet = {"v": 1, "window_present": True, "flags_begin": 0x200,
                  "flags_end": 0x200, "point_size": [1280, 720],
                  "drawable_size": [2560, 1440], "native_size_begin": [2560, 1440],
                  "native_size_end": [2560, 1440], "backend_frame_begin": 9,
                  "backend_frame_end": 10}
        packet.update(updates)
        return packet

    def _context_csv(self, packets, *, values=None):
        stream = io.StringIO(newline="")
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(["frame_index", "phase", "ms", "context_json"])
        for index, packet in enumerate(packets):
            writer.writerow([index, "frame_wall", (values or [10] * len(packets))[index],
                             json.dumps(packet, separators=(",", ":"))])
            writer.writerow([index, "physics", 1, ""])
        return stream.getvalue()

    def test_context_schema_preserves_all_timing_and_selected_packet_alignment(self):
        packets = [self._context(backend_frame_begin=i, backend_frame_end=i + 1)
                   for i in range(4)]
        text = self._context_csv(packets, values=[10, 1, 90, 2])
        path = self._csv(text)
        result = read_frames(path, warmup=1, sample_count=2)
        self.assertEqual(list(result["frame_ms"]), [1, 90])
        self.assertEqual(result["frame_context"], packets[1:3])
        self.assertEqual(result["context_schema"], "window_v1")
        self.assertEqual(result["csv_header_bytes"], len(text.splitlines(keepends=True)[0]))
        # Extra metadata does not break time-window summaries or erase a spike.
        self.assertEqual(soak_windows(result, seconds=0.05)[0]["frame"]["frame_max_ms"], 90)
        legacy = self._csv("frame_index,phase,ms\n1,frame_wall,1\n1,physics,1\n"
                           "2,frame_wall,90\n2,physics,1\n")
        old = read_frames(legacy, warmup=0, sample_count=None)
        self.assertEqual(frame_summary(old), frame_summary(result))
        self.assertIsNone(old["frame_context"])
        self.assertIsNone(old["context_schema"])
        self.assertEqual(old["csv_header_bytes"], 21)

    def test_context_unknown_and_known_zero_flags_are_distinct(self):
        packets = [self._context(flags_begin=None, flags_end=None,
                                 native_size_end=None, backend_frame_end=None),
                   self._context(flags_begin=0, flags_end=0)]
        result = read_frames(self._csv(self._context_csv(packets)), warmup=0, sample_count=None)
        self.assertIsNone(result["frame_context"][0]["flags_begin"])
        self.assertEqual(result["frame_context"][1]["flags_begin"], 0)

    def test_header_accounting_keeps_actual_crlf_bytes(self):
        text = self._context_csv([self._context()]).replace("\n", "\r\n")
        result = read_frames(self._csv(text), warmup=0, sample_count=None)
        self.assertEqual(result["csv_header_bytes"], 35)
        quoted = '"frame_index",phase,ms\n0,frame_wall,1\n'
        with self.assertRaisesRegex(ValueError, "physical encoding"):
            read_frames(self._csv(quoted), warmup=0, sample_count=None)

    def test_context_absent_window_and_resize_are_retained_without_claiming_visibility(self):
        absent = self._context(window_present=False, flags_begin=None, flags_end=None,
                               point_size=None, drawable_size=None,
                               native_size_begin=None, native_size_end=None)
        resized = self._context(native_size_end=[1440, 900], flags_end=0x40)
        packets = [absent, resized]
        result = read_frames(self._csv(self._context_csv(packets)), warmup=0, sample_count=None)
        self.assertEqual(result["frame_context"], packets)
        self.assertEqual(list(result["frame_ms"]), [10, 10])

    def test_context_rejects_unknown_missing_duplicate_and_invalid_types(self):
        cases = [self._context(v=True), self._context(v=2), self._context(window_present=1),
                 self._context(flags_begin=True), self._context(flags_end=-1),
                 self._context(backend_frame_end=2 ** 32), self._context(backend_frame_begin=1.0),
                 self._context(point_size=[0, 1]), self._context(drawable_size=[True, 2]),
                 self._context(native_size_begin=[1]), self._context(native_size_end="2560x1440"),
                 self._context(window_present=False), self._context(extra=1)]
        missing = self._context()
        del missing["flags_end"]
        cases.append(missing)
        for packet in cases:
            with self.subTest(packet=packet), self.assertRaises(ValueError):
                read_frames(self._csv(self._context_csv([packet])), warmup=0, sample_count=None)
        duplicate = self._context_csv([self._context()]).replace('{""v"":1,', '{""v"":1,""v"":1,')
        with self.assertRaisesRegex(ValueError, "duplicate frame context"):
            read_frames(self._csv(duplicate), warmup=0, sample_count=None)

    def test_context_cannot_hide_invalid_tail_or_attach_to_another_phase(self):
        valid = self._context_csv([self._context(), self._context(v=2)])
        with self.assertRaises(ValueError):
            read_frames(self._csv(valid), warmup=0, sample_count=1)
        valid = self._context_csv([self._context()])
        for text in (valid.replace('0,physics,1,\n', '0,physics,1,{}\n'),
                     'frame_index,phase,ms,context_json\n0,frame_wall,1,\n',
                     valid.replace('context_json', 'context_other'),
                     valid.replace('0,physics,1,\n', '0,physics,1\n')):
            with self.subTest(text=text), self.assertRaises(ValueError):
                read_frames(self._csv(text), warmup=0, sample_count=1)

    @staticmethod
    def _viewport_context(**updates):
        packet = {"source_id": 1, "flags": 0, "cpu_outcome": 0,
                  "refusal_reason": 0, "requested_size": None,
                  "allocated_size": None, "display_size": None,
                  "texture_index": None, "image_count": None,
                  "queued_commands": None, "main_submits": None,
                  "recording_submits": None}
        packet.update(updates)
        return packet

    def _context_v2(self, **updates):
        packet = self._context(v=2, viewport_reported=False, viewports=[])
        packet.update(updates)
        return packet

    def test_context_v2_defaults_and_explicit_empty_are_distinct(self):
        packets = [self._context_v2(), self._context_v2(viewport_reported=True)]
        result = read_frames(self._csv(self._context_csv(packets)), warmup=0, sample_count=None)
        self.assertEqual(result["frame_context"], packets)
        self.assertEqual(result["context_schema"], "window_viewports_v2")
        self.assertFalse(result["frame_context"][0]["viewport_reported"])
        self.assertTrue(result["frame_context"][1]["viewport_reported"])
        self.assertEqual(result["frame_context"][1]["viewports"], [])

    def test_context_v2_preserves_frames_phases_and_selected_alignment(self):
        viewport = self._viewport_context(flags=255, cpu_outcome=3, refusal_reason=2 ** 32 - 1,
                                          requested_size=[1, 2 ** 32 - 1],
                                          allocated_size=[2560, 1440], display_size=[1280, 720],
                                          texture_index=65534, image_count=2 ** 32 - 1,
                                          queued_commands=2, main_submits=1, recording_submits=0)
        packets = [self._context_v2(viewport_reported=True, viewports=[viewport],
                                    backend_frame_begin=index, backend_frame_end=index + 1)
                   for index in range(4)]
        text = self._context_csv(packets, values=[10, 1, 90, 2])
        result = read_frames(self._csv(text), warmup=1, sample_count=2)
        baseline = read_frames(self._csv(self._context_csv([self._context()] * 4,
                                                           values=[10, 1, 90, 2])),
                               warmup=1, sample_count=2)
        self.assertEqual(result["frame_context"], packets[1:3])
        self.assertEqual(list(result["frame_indices"]), [1, 2])
        self.assertEqual(list(result["phase_ms"]["physics"]), [1, 1])
        self.assertEqual(list(result["frame_ms"]), [1, 90])
        self.assertEqual(frame_summary(result), frame_summary(baseline))
        self.assertEqual(result["csv_header_bytes"], baseline["csv_header_bytes"])
        self.assertEqual(soak_windows(result, seconds=0.05)[0]["frame"]["frame_max_ms"], 90)

    def test_context_v2_unknown_counts_and_measured_zero_remain_distinct(self):
        unknown = self._viewport_context()
        zero = self._viewport_context(flags=0xf0, texture_index=0, image_count=0,
                                      queued_commands=0, main_submits=0, recording_submits=0)
        packet = self._context_v2(viewport_reported=True, viewports=[unknown, dict(zero, source_id=2)])
        result = read_frames(self._csv(self._context_csv([packet])), warmup=0, sample_count=None)
        retained = result["frame_context"][0]["viewports"]
        for field in ("texture_index", "image_count", "queued_commands", "main_submits",
                      "recording_submits"):
            self.assertIsNone(retained[0][field])
            self.assertEqual(retained[1][field], 0)

    def test_context_v2_availability_facts_are_independent(self):
        facts = [(1, {"requested_size": [1, 1]}), (2, {"allocated_size": [2, 3]}),
                 (4, {}), (8, {"display_size": [4, 5]}), (16, {"texture_index": 0}),
                 (32, {"image_count": 0}), (64, {"queued_commands": 0}),
                 (128, {"main_submits": 0, "recording_submits": 0})]
        for flag, fields in facts:
            for outcome in range(4):
                with self.subTest(flag=flag, outcome=outcome):
                    viewport = self._viewport_context(flags=flag, cpu_outcome=outcome,
                                                      refusal_reason=99, **fields)
                    packet = self._context_v2(viewport_reported=True, viewports=[viewport])
                    result = read_frames(self._csv(self._context_csv([packet])),
                                         warmup=0, sample_count=None)
                    self.assertEqual(result["frame_context"], [packet])
        # Per-frame source uniqueness does not prohibit reuse in later frames.
        viewports = [self._viewport_context(source_id=source) for source in (1, 2, 3, 2 ** 32 - 1)]
        packet = self._context_v2(viewport_reported=True, viewports=viewports)
        result = read_frames(self._csv(self._context_csv([packet, packet])), warmup=0, sample_count=None)
        self.assertEqual(result["frame_context"], [packet, packet])

    def test_context_v2_rejects_invalid_top_level_shape_and_version(self):
        packets = [self._context_v2(extra=1), self._context_v2(viewport_reported=0),
                   self._context_v2(viewport_reported=None), self._context_v2(viewports=None),
                   self._context_v2(viewports={}), self._context_v2(viewports=[self._viewport_context()]),
                   self._context_v2(viewport_reported=True,
                                    viewports=[self._viewport_context(source_id=n) for n in range(1, 6)])]
        for version in (True, False, 0, 1, 3, 2.0, "2", None):
            packet = self._context_v2()
            packet["v"] = version
            packets.append(packet)
        for key in self._context_v2():
            packet = self._context_v2()
            del packet[key]
            packets.append(packet)
        for packet in packets:
            with self.subTest(packet=packet), self.assertRaises(ValueError):
                read_frames(self._csv(self._context_csv([packet])), warmup=0, sample_count=None)

    def test_context_v2_independent_validator_requires_its_own_version_and_object(self):
        from tools import benchmark_suite as suite
        for packet in (None, [], 2, "2"):
            with self.subTest(packet=packet), self.assertRaises(ValueError):
                suite._frame_context_v2(packet)
        for version in (True, False, 0, 1, 3, 2.0, "2", None):
            with self.subTest(version=version), self.assertRaisesRegex(ValueError, "version"):
                suite._frame_context_v2(self._context_v2(v=version))
        packet = self._context_v2()
        self.assertIs(suite._frame_context_v2(packet), packet)

    def test_context_v2_rejects_invalid_window_facts(self):
        for fields in ({"window_present": 1}, {"flags_begin": True}, {"flags_end": -1},
                       {"backend_frame_begin": 2 ** 32}, {"backend_frame_end": 1.0},
                       {"point_size": [0, 1]}, {"drawable_size": [True, 2]},
                       {"native_size_begin": [1]}, {"native_size_end": "1x2"},
                       {"window_present": False}):
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                read_frames(self._csv(self._context_csv([self._context_v2(**fields)])),
                            warmup=0, sample_count=None)
        packet = self._context_v2(window_present=False, flags_begin=None, flags_end=None,
                                  point_size=None, drawable_size=None, native_size_begin=None,
                                  native_size_end=None, viewport_reported=True,
                                  viewports=[self._viewport_context(flags=4)])
        self.assertEqual(read_frames(self._csv(self._context_csv([packet])), warmup=0,
                                     sample_count=None)["frame_context"], [packet])

    def test_context_v2_rejects_invalid_viewport_identity_shape_flags_and_outcome(self):
        viewports = [None, [], 1, self._viewport_context(extra=1)]
        for key in self._viewport_context():
            viewport = self._viewport_context()
            del viewport[key]
            viewports.append(viewport)
        ranges = {"source_id": (0, -1, 2 ** 32, True, 1.0, "1", None),
                  "flags": (-1, 256, 2 ** 32, True, 1.0, "0", None),
                  "cpu_outcome": (-1, 4, True, 0.0, "0", None),
                  "refusal_reason": (-1, 2 ** 32, True, 0.0, "0", None)}
        for key, values in ranges.items():
            viewports.extend(self._viewport_context(**{key: value}) for value in values)
        for viewport in viewports:
            packet = self._context_v2(viewport_reported=True, viewports=[viewport])
            with self.subTest(viewport=viewport), self.assertRaises(ValueError):
                read_frames(self._csv(self._context_csv([packet])), warmup=0, sample_count=None)
        duplicate = self._context_v2(viewport_reported=True,
                                     viewports=[self._viewport_context(), self._viewport_context()])
        with self.assertRaisesRegex(ValueError, "source"):
            read_frames(self._csv(self._context_csv([duplicate])), warmup=0, sample_count=None)

    def test_context_v2_rejects_size_availability_type_and_range_mismatches(self):
        for flag, key in ((1, "requested_size"), (2, "allocated_size"), (8, "display_size")):
            cases = [(0, [1, 1])] + [(flag, value) for value in
                     (None, [], [1], [1, 2, 3], [0, 1], [-1, 1], [2 ** 32, 1],
                      [True, 1], [1.0, 1], ["1", 1], "1x2", {"x": 1, "y": 2})]
            for flags, value in cases:
                packet = self._context_v2(viewport_reported=True,
                                          viewports=[self._viewport_context(flags=flags, **{key: value})])
                with self.subTest(key=key, flags=flags, value=value), self.assertRaises(ValueError):
                    read_frames(self._csv(self._context_csv([packet])), warmup=0, sample_count=None)

    def test_context_v2_rejects_count_availability_type_and_range_mismatches(self):
        fields = ((16, "texture_index", 65535), (32, "image_count", 2 ** 32),
                  (64, "queued_commands", 2 ** 32), (128, "main_submits", 2 ** 32),
                  (128, "recording_submits", 2 ** 32))
        for flag, key, limit in fields:
            cases = [(0, 0)] + [(flag, value) for value in (None, -1, limit, True, 0.0, "0", [])]
            for flags, value in cases:
                viewport = self._viewport_context(flags=flags)
                if flags == 128:
                    viewport.update(main_submits=0, recording_submits=0)
                viewport[key] = value
                packet = self._context_v2(viewport_reported=True, viewports=[viewport])
                with self.subTest(key=key, flags=flags, value=value), self.assertRaises(ValueError):
                    read_frames(self._csv(self._context_csv([packet])), warmup=0, sample_count=None)

    def test_context_v2_rejects_duplicate_json_keys_at_both_levels(self):
        packet = self._context_v2(viewport_reported=True, viewports=[self._viewport_context()])
        valid = self._context_csv([packet])
        for old, new in (('{""v"":2,', '{""v"":2,""v"":2,'),
                         ('{""source_id"":1,', '{""source_id"":1,""source_id"":1,')):
            self.assertIn(old, valid)
            with self.subTest(key=old), self.assertRaisesRegex(ValueError, "duplicate frame context"):
                read_frames(self._csv(valid.replace(old, new)), warmup=0, sample_count=None)

    def test_context_v2_rejects_mixed_schemas_in_warmup_and_unselected_tail(self):
        for packets in ([self._context(), self._context_v2(), self._context_v2()],
                        [self._context_v2(), self._context(), self._context()],
                        [self._context_v2(), self._context_v2(), self._context()],
                        [self._context(), self._context(), self._context_v2()]):
            with self.subTest(versions=[packet["v"] for packet in packets]), \
                    self.assertRaisesRegex(ValueError, "mixed frame context"):
                read_frames(self._csv(self._context_csv(packets)), warmup=1, sample_count=1)

    def test_context_v2_cannot_hide_invalid_warmup_tail_or_phase_attachment(self):
        for index in (0, 2):
            for changes in ({"flags": 256}, {"image_count": 0}, {"source_id": 0}):
                packets = [self._context_v2() for _ in range(3)]
                packets[index] = self._context_v2(viewport_reported=True,
                                                  viewports=[self._viewport_context(**changes)])
                with self.subTest(index=index, changes=changes), self.assertRaises(ValueError):
                    read_frames(self._csv(self._context_csv(packets)), warmup=1, sample_count=1)
        valid = self._context_csv([self._context_v2()])
        for text in (valid.replace('0,physics,1,\n', '0,physics,1,{}\n'),
                     valid.replace('0,physics,1,\n', '0,physics,1\n')):
            with self.subTest(text=text), self.assertRaises(ValueError):
                read_frames(self._csv(text), warmup=0, sample_count=1)

    @staticmethod
    def _clock_context(**updates):
        clock = {"domain": "jce-performance-counter", "frequency_hz": "1000000",
                 "begin": "0", "end": "10000"}
        clock.update(updates)
        return clock

    def _context_v3(self, **updates):
        packet = self._context_v2(v=3, clock=self._clock_context())
        packet.update(updates)
        return packet

    def test_context_v3_preserves_exact_clock_gaps_and_selected_timing_alignment(self):
        begin = 9007199254740993
        packets = []
        for duration in (10, 1, 90, 2):
            end = begin + duration * 1000
            packets.append(self._context_v3(clock=self._clock_context(begin=str(begin), end=str(end)),
                viewport_reported=True, viewports=[self._viewport_context(flags=4)]))
            begin = end + 1234
        values = [10, 1, 90, 2]
        result = read_frames(self._csv(self._context_csv(packets, values=values)), warmup=1, sample_count=2)
        baseline = read_frames(self._csv(self._context_csv([self._context_v2()] * 4, values=values)),
                               warmup=1, sample_count=2)
        self.assertEqual(result["context_schema"], "window_viewports_clock_v3")
        self.assertEqual(result["frame_context"], packets[1:3])
        self.assertEqual(list(result["frame_indices"]), [1, 2])
        self.assertEqual(list(result["frame_ms"]), [1, 90])
        self.assertEqual(list(result["phase_ms"]["physics"]), [1, 1])
        self.assertEqual(frame_summary(result), frame_summary(baseline))
        self.assertEqual(result["csv_header_bytes"], baseline["csv_header_bytes"])
        self.assertEqual(soak_windows(result, seconds=0.05)[0]["frame"]["frame_max_ms"], 90)

    def test_context_v3_preserves_uint64_max_and_small_delta_above_float_precision(self):
        maximum = "18446744073709551615"
        cases = [(self._clock_context(frequency_hz=maximum, begin="0", end=maximum), 1000),
                 (self._clock_context(frequency_hz="1000", begin="18446744073709551614",
                                      end=maximum), 1),
                 (self._clock_context(frequency_hz=maximum, begin=maximum, end=maximum), 0),
                 (self._clock_context(frequency_hz="1", begin="0", end=maximum),
                  "18446744073709551616000.0000")]
        for clock, wall in cases:
            packet = self._context_v3(clock=clock)
            with self.subTest(clock=clock):
                result = read_frames(self._csv(self._context_csv([packet], values=[wall])),
                                     warmup=0, sample_count=None, allow_zero_wall=True)
                self.assertEqual(result["frame_context"], [packet])

    def test_context_v3_rejects_noncanonical_or_nonstring_uint64_fields(self):
        invalid = (True, False, 0, 1, -1, 1.0, None, [], "", "00", "01", " 1", "1 ",
                   "+1", "-1", "1.0", "1e3", "\u0661", "\uff11", "18446744073709551616",
                   "100000000000000000000")
        for key in ("frequency_hz", "begin", "end"):
            for value in invalid:
                packet = self._context_v3(clock=self._clock_context(**{key: value}))
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    read_frames(self._csv(self._context_csv([packet])), warmup=0, sample_count=None)

    def test_context_v3_requires_exact_shape_domain_positive_frequency_and_order(self):
        from tools import benchmark_suite as suite
        packets = [self._context_v3(extra=1), self._context_v3(clock=None),
                   self._context_v3(clock=[]), self._context_v3(clock=self._clock_context(extra=1)),
                   self._context_v3(clock=self._clock_context(frequency_hz="0")),
                   self._context_v3(clock=self._clock_context(begin="2", end="1"))]
        for domain in (None, True, 1, "", "unix", "gpu", "process-perf-ns"):
            packets.append(self._context_v3(clock=self._clock_context(domain=domain)))
        for key in self._context_v3():
            packet = self._context_v3()
            del packet[key]
            packets.append(packet)
        for key in self._clock_context():
            clock = self._clock_context()
            del clock[key]
            packets.append(self._context_v3(clock=clock))
        for packet in packets:
            with self.subTest(packet=packet), self.assertRaises(ValueError):
                read_frames(self._csv(self._context_csv([packet])), warmup=0, sample_count=None)
        for packet in (None, [], 3, "3"):
            with self.subTest(packet=packet), self.assertRaises(ValueError):
                suite._frame_context_v3(packet)
        for version in (True, False, 0, 1, 2, 4, 3.0, "3", None):
            with self.subTest(version=version), self.assertRaisesRegex(ValueError, "version"):
                suite._frame_context_v3(self._context_v3(v=version))
        packet = self._context_v3()
        self.assertIs(suite._frame_context_v3(packet), packet)

    def test_context_v3_reuses_strict_v2_window_and_viewport_validation(self):
        packets = [self._context_v3(window_present=1), self._context_v3(flags_begin=True),
                   self._context_v3(native_size_end=[0, 1]), self._context_v3(window_present=False),
                   self._context_v3(viewport_reported=1), self._context_v3(viewports=None)]
        for updates in ({"source_id": 0}, {"flags": 256}, {"image_count": 0},
                        {"cpu_outcome": 4}, {"display_size": [1, 1]}, {"extra": 1}):
            packets.append(self._context_v3(viewport_reported=True,
                                            viewports=[self._viewport_context(**updates)]))
        for packet in packets:
            with self.subTest(packet=packet), self.assertRaises(ValueError):
                read_frames(self._csv(self._context_csv([packet])), warmup=0, sample_count=None)

    def test_context_v3_rejects_duplicate_clock_keys_and_phase_attachment(self):
        valid = self._context_csv([self._context_v3()])
        for key, value in self._clock_context().items():
            token = json.dumps(key) + ":" + json.dumps(value)
            token = token.replace('"', '""')
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "duplicate frame context"):
                read_frames(self._csv(valid.replace(token, token + "," + token)), warmup=0, sample_count=None)
        duplicate = valid.replace('{""v"":3,', '{""v"":3,""clock"":{},')
        with self.assertRaisesRegex(ValueError, "duplicate frame context"):
            read_frames(self._csv(duplicate), warmup=0, sample_count=None)
        for text in (valid.replace('0,physics,1,\n', '0,physics,1,{}\n'),
                     valid.replace('0,physics,1,\n', '0,physics,1\n')):
            with self.subTest(text=text), self.assertRaises(ValueError):
                read_frames(self._csv(text), warmup=0, sample_count=None)

    def test_context_v3_accepts_serialized_rounding_and_rejects_wrong_wall_duration(self):
        packet = self._context_v3(clock=self._clock_context(frequency_hz="1000000000", end="10000050"))
        for wall in ("10.0000", "10.0001"):
            result = read_frames(self._csv(self._context_csv([packet], values=[wall])), warmup=0, sample_count=None)
            self.assertEqual(result["frame_context"], [packet])
        for wall in ("9.9999", "10.0002", "0.0000"):
            with self.subTest(wall=wall), self.assertRaisesRegex(ValueError, "clock duration"):
                read_frames(self._csv(self._context_csv([packet], values=[wall])),
                            warmup=0, sample_count=None, allow_zero_wall=True)

    def test_context_v3_all_raw_rows_reject_bad_clock_duration_and_mixed_schemas(self):
        for index in range(3):
            for kind in ("canonical", "duration", "domain", "mixed"):
                packets = [self._context_v3(clock=self._clock_context(begin=str(n * 10000),
                                                                      end=str((n + 1) * 10000)))
                           for n in range(3)]
                if kind == "mixed":
                    packets[index] = self._context_v2()
                else:
                    changes = {"canonical": {"end": "01"}, "domain": {"domain": "unix"},
                               "duration": {"end": str(index * 10000 + 9000)}}
                    packets[index]["clock"].update(changes[kind])
                with self.subTest(index=index, kind=kind), self.assertRaises(ValueError):
                    read_frames(self._csv(self._context_csv(packets)), warmup=1, sample_count=1)
            packets = [self._context_v2() for _ in range(3)]
            packets[index] = self._context_v3()
            with self.subTest(index=index), self.assertRaisesRegex(ValueError, "mixed frame context"):
                read_frames(self._csv(self._context_csv(packets)), warmup=1, sample_count=1)

    def test_context_v3_frequency_and_nonoverlap_cover_warmup_selected_and_tail(self):
        for index in range(3):
            packets = [self._context_v3(clock=self._clock_context(begin=str(n * 20000),
                                                                  end=str(n * 20000 + 10000)))
                       for n in range(3)]
            packets[index]["clock"].update(frequency_hz="2000000", end=str(index * 20000 + 20000))
            with self.subTest(frequency_index=index), self.assertRaisesRegex(ValueError, "frequency changed"):
                read_frames(self._csv(self._context_csv(packets)), warmup=1, sample_count=1)
        for index in (1, 2):
            packets = [self._context_v3(clock=self._clock_context(begin=str(n * 10000),
                                                                  end=str((n + 1) * 10000)))
                       for n in range(3)]
            packets[index]["clock"].update(begin=str(index * 10000 - 1), end=str((index + 1) * 10000 - 1))
            with self.subTest(overlap_index=index), self.assertRaisesRegex(ValueError, "overlap"):
                read_frames(self._csv(self._context_csv(packets)), warmup=1, sample_count=1)

    def test_context_v3_zero_counter_and_interval_preserve_existing_raw_zero_policy(self):
        zero = self._context_v3(clock=self._clock_context(end="0"))
        path = self._csv(self._context_csv([zero], values=[0]))
        with self.assertRaisesRegex(ValueError, "steady-state frame time is zero"):
            read_frames(path, warmup=0, sample_count=None)
        self.assertEqual(read_frames(path, warmup=0, sample_count=None, allow_zero_wall=True)
                         ["frame_context"], [zero])
        tail = self._context_v3(clock=self._clock_context(begin="10000", end="10000"))
        result = read_frames(self._csv(self._context_csv([zero, self._context_v3(), tail], values=[0, 10, 0])),
                             warmup=1, sample_count=1)
        self.assertEqual(result["frame_context"], [self._context_v3()])

    def test_context_v3_omitted_clock_consumer_fails_ordinary_assertions(self):
        from tools import benchmark_suite as suite
        source = Path(suite.__file__).read_text(encoding="utf-8")
        before = 'if context["v"] == 3:'
        self.assertEqual(source.count(before), 1)
        root = Path(__file__).resolve().parents[2]
        scratch_parent = root / "build/tests"
        scratch_parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch_parent) as scratch:
            owner = Path(scratch) / "omitted_clock_owner.py"
            owner.write_text(source.replace(before, 'if False:', 1), encoding="utf-8", newline="\n")
            script = ("import importlib.util, sys, unittest\n"
                      "from tools.tests import test_benchmark_suite as controls\n"
                      "sys.path.insert(0, str(controls.Path(controls.__file__).resolve().parents[1]))\n"
                      "spec=importlib.util.spec_from_file_location('omitted_clock_owner',sys.argv[1])\n"
                      "owner=importlib.util.module_from_spec(spec)\n"
                      "spec.loader.exec_module(owner)\n"
                      "controls.read_frames=owner.read_frames\n"
                      "tests=unittest.TestSuite([controls.BenchmarkSamplesTest(\n"
                      " 'test_context_v3_accepts_serialized_rounding_and_rejects_wrong_wall_duration'),\n"
                      " controls.BenchmarkSamplesTest(\n"
                      " 'test_context_v3_frequency_and_nonoverlap_cover_warmup_selected_and_tail')])\n"
                      "result=unittest.TextTestRunner(verbosity=2).run(tests)\n"
                      "raise SystemExit(0 if result.wasSuccessful() else 1)\n")
            result = subprocess.run([sys.executable, "-B", "-c", script, str(owner)],
                                    cwd=root, capture_output=True, text=True, timeout=30)
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 1, output)
        self.assertIn("ValueError not raised", output)
        self.assertNotIn("ERROR:", output)

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
