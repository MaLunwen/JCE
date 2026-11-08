"""Lifecycle evidence must reject missing phases, broken restoration and incomplete runs."""
import copy
import io
import inspect
import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest.mock import patch
import zlib
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import editor_lifecycle_probe as lifecycle
from editor_lifecycle_probe import canonical_snapshot, lifecycle_environment, validate_errors, validate_phases, validate_scenes
from editor_portability_probe import validate_process
from tools import terminal_ack_observation as terminal
from tools import lifecycle_trace_report as trace_reader
from tools.tests.test_shader_provenance_observation import create as shader_create, link as shader_link
from tools.tests.test_filesystem_read_observation import sample as filesystem_sample
from tools.tests.test_terminal_ack_observation import TOKEN, encoded, packet, trace_document


def snapshot(first=1, second=2):
    return {"contract": {"version":1}, "scene":{"version":1,"entities":[
        {"id":first,"parentId":0,"name":"root","components":[{"type":"Transform","x":0}]},
        {"id":second,"parentId":first,"name":"child","components":[{"type":"Custom","value":7}]}]}}


def samples():
    phases = {}
    for name, frame in (("ed_play_capture",19),("ed_play_create",19),
                        ("ed_play_teardown",179),("ed_play_restore",179)):
        phases[name] = [math.nan] * 240
        phases[name][frame] = .125
    return {"frame_indices":list(range(240)), "phase_ms":phases}

class LifecycleTests(unittest.TestCase):
    def test_scene_handle_renaming_preserves_hierarchy_and_every_payload_field(self):
        before, after = snapshot(), snapshot(101,102)
        during = snapshot()
        during["scene"]["entities"][1]["components"][0]["value"] = 8
        result = validate_scenes(before,during,after,True)
        self.assertTrue(result["during_mutation_observed"])
        self.assertEqual(before,snapshot())
        for changed in ("component","name","parent","contract"):
            bad = copy.deepcopy(after)
            if changed == "component": bad["scene"]["entities"][1]["components"][0]["value"] = 9
            if changed == "name": bad["scene"]["entities"][1]["name"] = "different"
            if changed == "parent": bad["scene"]["entities"][1]["parentId"] = 0
            if changed == "contract": bad["contract"]["version"] = 2
            with self.assertRaises(ValueError): validate_scenes(before,during,bad)
    def test_nearshore_reference_is_renamed_but_retargeting_is_detected(self):
        before, after = snapshot(), snapshot(101, 102)
        for value, terrain in ((before, 1), (after, 101)):
            value["scene"]["entities"][1]["components"].append(
                {"type": "WaterNearshore", "properties": {"version": 1, "terrain": terrain}}
            )
        self.assertTrue(validate_scenes(before, before, after)["restored"])
        for invalid in (0, 102, 999, True):
            after["scene"]["entities"][1]["components"][-1]["properties"]["terrain"] = invalid
            with self.assertRaises(ValueError):
                validate_scenes(before, before, after)
    def test_script_entity_parameter_renames_only_the_typed_reference(self):
        before, after = snapshot(), snapshot(2**32 + 101, 2**32 + 102)
        for value, target in ((before, 1), (after, 2**32 + 101)):
            value["scene"]["entities"][1]["components"].append({"type": "Script", "params": [
                {"name": "target", "kind": 3, "entity": target, "number": 1},
                {"name": "optional", "kind": 3, "entity": 0},
                {"name": "number", "kind": 0, "entity": 1, "number": 1},
            ]})
        self.assertTrue(validate_scenes(before, before, after)["restored"])
        params = after["scene"]["entities"][1]["components"][-1]["params"]
        self.assertEqual(params[0]["entity"], 2**32 + 101)
        for invalid in (0, 2**32 + 102, 999, True, 1.0, -1):
            params[0]["entity"] = invalid
            with self.assertRaises(ValueError):
                validate_scenes(before, before, after)
        params[0]["entity"] = 2**32 + 101
        params[2]["number"] = 2**32 + 101
        with self.assertRaises(ValueError):
            validate_scenes(before, before, after)

    def test_static_scene_does_not_claim_mutation_or_script_execution(self):
        result = validate_scenes(snapshot(),snapshot(),snapshot(101,102))
        self.assertFalse(result["during_mutation_observed"])
        self.assertIn("UNAVAILABLE",result["script_execution"])
        with self.assertRaises(ValueError): validate_scenes(snapshot(),snapshot(),snapshot(),True)
    def test_invalid_duplicate_or_missing_parent_handles_fail(self):
        for value in (snapshot(1,1),snapshot()):
            if value["scene"]["entities"][0]["id"] != value["scene"]["entities"][1]["id"]:
                value["scene"]["entities"][1]["parentId"] = 999
            with self.assertRaises(ValueError): canonical_snapshot(value)
    def test_phase_presence_positive_values_order_and_complete_frames(self):
        self.assertEqual(validate_phases(samples())["ed_play_restore"]["frame"],179)
        for kind in ("missing","zero","early_stop","late_create","split","duplicate","truncated"):
            value = samples()
            if kind == "missing": del value["phase_ms"]["ed_play_restore"]
            if kind == "zero": value["phase_ms"]["ed_play_capture"][19] = 0
            if kind == "early_stop": value["phase_ms"]["ed_play_teardown"] = [math.nan]*240;value["phase_ms"]["ed_play_teardown"][9]=.1
            if kind == "late_create": value["phase_ms"]["ed_play_create"] = [math.nan]*240;value["phase_ms"]["ed_play_create"][200]=.1
            if kind == "split": value["phase_ms"]["ed_play_restore"][179]=math.nan;value["phase_ms"]["ed_play_restore"][180]=.1
            if kind == "duplicate": value["phase_ms"]["ed_play_create"][20]=.1
            if kind == "truncated": value["frame_indices"] = list(range(239))
            with self.assertRaises(ValueError): validate_phases(value)
    def test_production_zero_reset_series_is_not_repeated_execution(self):
        value = samples()
        for series in value["phase_ms"].values():
            for index in range(len(series)):
                if math.isnan(series[index]): series[index] = 0.0
        self.assertEqual(validate_phases(value)["ed_play_capture"]["frame"],19)
        for bad in (-.1,math.inf,-math.inf):
            changed = copy.deepcopy(value)
            changed["phase_ms"]["ed_play_create"][200] = bad
            with self.assertRaises(ValueError): validate_phases(changed)
        changed = copy.deepcopy(value)
        changed["phase_ms"]["ed_play_create"][200] = .1
        with self.assertRaises(ValueError): validate_phases(changed)
        changed = copy.deepcopy(value)
        changed["phase_ms"]["ed_play_create"] = [0.] * 240
        with self.assertRaises(ValueError): validate_phases(changed)

    def test_failed_exit_and_error_log_never_pass(self):
        validate_process({"timed_out":False,"returncode":0})
        for process in ({"timed_out":True,"returncode":0},{"timed_out":False,"returncode":1}):
            with self.assertRaises(ValueError): validate_process(process)
        for log in ("] ERROR - constructor failed", "] FATAL - bad state", "single-instance lock already held"):
            with self.assertRaises(ValueError): validate_errors(log)
    def test_opt_in_trace_environment_is_isolated_and_default_off(self):
        root = Path("build/lifecycle-trace-control").resolve()
        scene = Path("scene.jbundle").resolve()
        with patch.dict("os.environ", {"JCE_TRACE": "1", "JCE_TRACE_STREAM": "inherited.json",
                                      "JCE_RESOURCE_TRACE": "1", "JCE_RESOURCE_TRACE_MS": "250"}):
            default = lifecycle_environment(scene, root)
            enabled = lifecycle_environment(scene, root, trace=True)
        self.assertNotIn("JCE_TRACE", default)
        self.assertNotIn("JCE_TRACE_STREAM", default)
        self.assertNotIn("JCE_RESOURCE_TRACE", default)
        self.assertNotIn("JCE_RESOURCE_TRACE", enabled)
        self.assertNotIn("JCE_RESOURCE_TRACE_MS", enabled)
        self.assertEqual(enabled["JCE_TRACE"], "1")
        self.assertEqual(enabled["JCE_TRACE_STREAM"], str(root / "lifecycle-trace.json"))
        self.assertEqual(enabled["JCE_TRACE_EXPORT"], "")
        self.assertNotIn("JCE_TRACE_WORKER", enabled)

    def test_renderer_selectors_are_explicit_normalized_and_inherited_values_are_removed(self):
        self.assertIn("backend", inspect.signature(lifecycle_environment).parameters)
        root, scene = Path("build/selector-control"), Path("scene.json")
        inherited = {"JCE_BACKEND": "noop", "JCE_GPU_ADAPTER": "0x1234",
                     "JCE_GPU_TIER": "low", "JCE_ARBITRARY": "unsupported"}
        with patch.dict("os.environ", inherited):
            default = lifecycle_environment(scene, root)
            requested = lifecycle_environment(scene, root, backend=" Vulkan ",
                                              gpu_adapter=" NVIDIA ", gpu_tier=" HIGH ")
        for key in inherited:
            self.assertNotIn(key, default)
        for key, value in (("JCE_BACKEND", "vulkan"), ("JCE_GPU_ADAPTER", "nvidia"),
                           ("JCE_GPU_TIER", "high")):
            self.assertEqual(requested.pop(key), value)
        self.assertEqual(requested, default)
        for key, values, env_name in (
            ("backend", ("d3d11", "d3d12", "vulkan", "opengl"), "JCE_BACKEND"),
            ("gpu_adapter", ("auto", "intel", "nvidia", "amd"), "JCE_GPU_ADAPTER"),
            ("gpu_tier", ("low", "medium", "high", "ultra"), "JCE_GPU_TIER")):
            for value in values:
                with self.subTest(key=key, value=value):
                    self.assertEqual(lifecycle_environment(scene, root, **{key: value})[env_name], value)

    def test_invalid_renderer_selectors_refuse_before_path_environment_or_child_access(self):
        self.assertIn("gpu_adapter", inspect.signature(lifecycle.probe).parameters)
        for key in ("backend", "gpu_adapter", "gpu_tier"):
            for value in ("", " ", "unsupported", True, 1, [], {}):
                with self.subTest(key=key, value=value), patch.object(Path, "resolve") as resolve, \
                     patch.object(Path, "mkdir") as mkdir, patch.object(Path, "open") as opened, \
                     patch.object(lifecycle, "probe_environment") as environment, patch.object(lifecycle, "run_process") as run:
                    with self.assertRaises(ValueError):
                        lifecycle_environment(Path("absent.scene"), Path("unused"), **{key: value})
                    with self.assertRaises(ValueError):
                        lifecycle.probe("absent.exe", "absent.scene", "unused", **{key: value})
                    for operation in (resolve, mkdir, opened, environment, run):
                        operation.assert_not_called()

    def test_vsync_omitted_keeps_parent_sanitized_and_receipt_default_shape(self):
        with patch.dict("os.environ", {"JCE_LAUNCH_VSYNC": "0", "JCE_ARBITRARY": "parent-only"}):
            report, observed = self._scheduled_cli_probe()
        self.assertNotIn("JCE_LAUNCH_VSYNC", observed["environment"])
        self.assertNotIn("JCE_ARBITRARY", observed["environment"])
        self.assertNotIn("vsync_requested", report)
        self.assertEqual(report["environment"], {key: value for key, value in observed["environment"].items()
                                               if key.startswith("JCE_")})

    def test_vsync_api_adds_only_explicit_flag_after_sanitization(self):
        self.assertIn("vsync", inspect.signature(lifecycle_environment).parameters)
        self.assertIsNone(inspect.signature(lifecycle_environment).parameters["vsync"].default)
        self.assertIsNone(inspect.signature(lifecycle.probe).parameters["vsync"].default)
        root, scene = Path("build/vsync-control"), Path("scene.json")
        with patch.dict("os.environ", {"JCE_LAUNCH_VSYNC": "inherited-invalid", "JCE_ARBITRARY": "blocked"}):
            default = lifecycle_environment(scene, root)
            self.assertEqual(default, lifecycle_environment(scene, root, vsync=None))
            self.assertNotIn("JCE_LAUNCH_VSYNC", default)
            for request, value in (("on", "1"), ("off", "0")):
                actual = lifecycle_environment(scene, root, vsync=request)
                self.assertEqual(actual.pop("JCE_LAUNCH_VSYNC"), value)
                self.assertEqual(actual, default)

    def test_vsync_cli_reaches_counted_child_and_records_request_and_actual_environment(self):
        baseline, _ = self._scheduled_cli_probe()
        for request in ("on", "off"):
            with self.subTest(request=request), patch.dict("os.environ", {"JCE_LAUNCH_VSYNC": "parent-only"}):
                try:
                    report, observed = self._scheduled_cli_probe(vsync=request)
                except SystemExit as error:
                    self.fail("explicit --vsync was rejected before its child: %s" % error)
                self.assertEqual(report["vsync_requested"], request)
                self.assertEqual(report["environment"]["JCE_LAUNCH_VSYNC"], "1" if request == "on" else "0")
                self.assertEqual(report["environment"], {key: value for key, value in observed["environment"].items()
                                                       if key.startswith("JCE_")})
                self.assertEqual(set(report) - {"vsync_requested"}, set(baseline))
                self.assertEqual(observed["preferences_text"], '{"startup_mode":1}\n')
                self.assertTrue(any("VSync" in text and "request" in text for text in report["limitations"]))

    def test_vsync_invalid_api_refuses_before_io_environment_or_child(self):
        self.assertIn("vsync", inspect.signature(lifecycle.probe).parameters)
        for value in (True, False, 0, 1, "", " ", "auto", "OFF", " on ", [], {}):
            with self.subTest(value=value), patch.object(Path, "resolve") as resolve, \
                 patch.object(Path, "mkdir") as mkdir, patch.object(Path, "open") as opened, \
                 patch.object(lifecycle, "probe_environment") as environment, patch.object(lifecycle, "run_process") as run:
                with self.assertRaisesRegex(ValueError, "--vsync must be on or off"):
                    lifecycle_environment(Path("absent.scene"), Path("unused"), vsync=value)
                with self.assertRaisesRegex(ValueError, "--vsync must be on or off"):
                    lifecycle.probe("absent.exe", "absent.scene", "unused", vsync=value)
                for operation in (resolve, mkdir, opened, environment, run):
                    operation.assert_not_called()

    def test_vsync_invalid_cli_usage_never_reaches_probe(self):
        for value in ("0", "1", "auto", "OFF", " on "):
            argv = ["editor_lifecycle_probe.py", "--exe", "absent.exe", "--scene", "absent.scene",
                    "--out", "unused", "--vsync", value]
            with self.subTest(value=value), patch.object(sys, "argv", argv), \
                 patch.object(sys, "stderr", io.StringIO()), patch.object(lifecycle, "probe") as run, \
                 patch.object(Path, "mkdir") as mkdir:
                with self.assertRaises(SystemExit) as error:
                    lifecycle.main()
                self.assertEqual(error.exception.code, 2)
                run.assert_not_called()
                mkdir.assert_not_called()

    def test_vsync_missing_forwarders_fail_ordinary_child_assertions(self):
        for name, wire in (("main", "vsync=args.vsync, "), ("probe", "vsync=vsync, ")):
            with self.subTest(owner=name):
                source = inspect.getsource(getattr(lifecycle, name))
                self.assertEqual(source.count(wire), 1)
                namespace = dict(lifecycle.__dict__)
                exec(compile(source.replace(wire, "", 1), lifecycle.__file__, "exec"), namespace)
                compiled = namespace[name]
                mutant = types.FunctionType(compiled.__code__, lifecycle.__dict__, argdefs=compiled.__defaults__)
                mutant.__kwdefaults__ = compiled.__kwdefaults__
                with patch.object(lifecycle, name, mutant), self.assertRaisesRegex(AssertionError, "explicit VSync"):
                    self._scheduled_cli_probe(vsync="off")

    def test_visible_nonactivating_environment_changes_only_window_requests(self):
        self.assertIs(inspect.signature(lifecycle_environment).parameters["visible_not_focusable"].default, False)
        self.assertIs(inspect.signature(lifecycle.probe).parameters["visible_not_focusable"].default, False)
        root, scene = Path("build/window-control"), Path("scene.json")
        with patch.dict("os.environ", {"JCE_WINDOW_HIDDEN": "0", "JCE_WINDOW_NOT_FOCUSABLE": "1"}):
            default = lifecycle_environment(scene, root)
            explicit_default = lifecycle_environment(scene, root, visible_not_focusable=False)
            visible = lifecycle_environment(scene, root, visible_not_focusable=True)
        self.assertEqual(default, explicit_default)
        self.assertEqual(default["JCE_WINDOW_HIDDEN"], "1")
        self.assertNotIn("JCE_WINDOW_NOT_FOCUSABLE", default)
        self.assertEqual(visible.pop("JCE_WINDOW_NOT_FOCUSABLE"), "1")
        self.assertEqual(visible.pop("JCE_WINDOW_HIDDEN"), "0")
        default.pop("JCE_WINDOW_HIDDEN")
        self.assertEqual(visible, default)

    def test_invalid_visible_request_refuses_before_path_environment_or_child_access(self):
        self.assertIn("visible_not_focusable", inspect.signature(lifecycle.probe).parameters)
        for value in (None, 0, 1, "false", "true", [], {}):
            with self.subTest(value=value), patch.object(Path, "resolve") as resolve, \
                 patch.object(Path, "mkdir") as mkdir, patch.object(Path, "open") as opened, \
                 patch.object(lifecycle, "probe_environment") as environment, patch.object(lifecycle, "run_process") as run:
                with self.assertRaisesRegex(ValueError, "visible_not_focusable must be boolean"):
                    lifecycle_environment(Path("absent.scene"), Path("unused"), visible_not_focusable=value)
                with self.assertRaisesRegex(ValueError, "visible_not_focusable must be boolean"):
                    lifecycle.probe("absent.exe", "absent.scene", "unused", visible_not_focusable=value)
                for operation in (resolve, mkdir, opened, environment, run):
                    operation.assert_not_called()

    def test_visible_cli_reaches_actual_child_environment_and_receipt(self):
        baseline, _ = self._scheduled_cli_probe()
        report, observed = self._scheduled_cli_probe(visible_not_focusable=True)
        self.assertEqual(report["status"], "PLAY_STOP_AUTHORED_SCENE_RESTORATION_OBSERVED")
        self.assertEqual(report["environment"]["JCE_WINDOW_HIDDEN"], "0")
        self.assertEqual(report["environment"]["JCE_WINDOW_NOT_FOCUSABLE"], "1")
        self.assertEqual(report["environment"], {key: value for key, value in observed["environment"].items()
                                               if key.startswith("JCE_")})
        self.assertEqual(set(report), set(baseline))
        self.assertEqual(observed["preferences_text"], '{"startup_mode":1}\n')
        self.assertEqual(report["limitations"][:-1], baseline["limitations"])
        self.assertIn("visible nonactivating window request", report["limitations"][-1])
        self.assertIn("not physical UI interaction or presentation proof", report["limitations"][-1])

    def test_visible_missing_forwarders_fail_ordinary_child_assertions(self):
        for name, wire in (("main", "visible_not_focusable=args.visible_not_focusable, "),
                           ("probe", "visible_not_focusable=visible_not_focusable, ")):
            with self.subTest(owner=name):
                source = inspect.getsource(getattr(lifecycle, name))
                self.assertEqual(source.count(wire), 1)
                namespace = dict(lifecycle.__dict__)
                exec(compile(source.replace(wire, "", 1), lifecycle.__file__, "exec"), namespace)
                compiled = namespace[name]
                mutant = types.FunctionType(compiled.__code__, lifecycle.__dict__, argdefs=compiled.__defaults__)
                mutant.__kwdefaults__ = compiled.__kwdefaults__
                with patch.object(lifecycle, name, mutant), self.assertRaises(AssertionError):
                    self._scheduled_cli_probe(visible_not_focusable=True)

    def test_visible_cli_refuses_string_boolean_before_probe(self):
        argv = ["editor_lifecycle_probe.py", "--exe", "absent.exe", "--scene", "absent.scene",
                "--out", "unused", "--visible-not-focusable=false"]
        with patch.object(sys, "argv", argv), patch.object(sys, "stderr", io.StringIO()), \
             patch.object(lifecycle, "probe") as run:
            with self.assertRaises(SystemExit) as error:
                lifecycle.main()
        self.assertEqual(error.exception.code, 2)
        run.assert_not_called()

    def test_selector_cli_uses_owned_profile_and_records_requests_separately(self):
        self.assertIn("backend", inspect.signature(lifecycle.probe).parameters)
        report, observed = self._scheduled_cli_probe(selectors={
            "backend": " OpenGL ", "gpu_adapter": " AMD ", "gpu_tier": " ULTRA "})
        expected = dict(backend="opengl", gpu_adapter="amd", gpu_tier="ultra")
        self.assertEqual(report["renderer_selectors_requested"], expected)
        self.assertEqual(observed["preferences"], {"startup_mode": 1, "renderer": "OpenGL"})
        self.assertEqual(observed["session"], {"ui_state_int": {"editor.gpu_adapter": 3}})
        self.assertEqual(observed["environment"]["JCE_BACKEND"], "opengl")
        self.assertEqual(observed["environment"]["JCE_GPU_ADAPTER"], "amd")
        self.assertEqual(observed["environment"]["JCE_GPU_TIER"], "ultra")
        self.assertEqual(report["backend_actual"], "UNAVAILABLE_COMPILED_OUT_INFO")
        self.assertTrue(any("requested selectors" in text for text in report["limitations"]))
        for adapter, choice in (("auto", 0), ("intel", 1), ("nvidia", 2)):
            with self.subTest(adapter=adapter):
                report, observed = self._scheduled_cli_probe(selectors={"gpu_adapter": adapter})
                self.assertEqual(report["renderer_selectors_requested"], {"gpu_adapter": adapter})
                self.assertEqual(observed["session"], {"ui_state_int": {"editor.gpu_adapter": choice}})
                self.assertEqual(observed["preferences_text"], '{"startup_mode":1}\n')
                self.assertNotIn("JCE_BACKEND", observed["environment"])
                self.assertNotIn("JCE_GPU_TIER", observed["environment"])

    def test_backend_profile_requests_and_failed_results_keep_request_semantics(self):
        for backend, preference in (("d3d11", "D3D11"), ("d3d12", "D3D12"), ("vulkan", "Vulkan")):
            with self.subTest(backend=backend):
                report, observed = self._scheduled_cli_probe(selectors={"backend": backend})
                self.assertEqual(observed["preferences"], {"startup_mode": 1, "renderer": preference})
                self.assertIsNone(observed["session"])
                self.assertEqual(report["renderer_selectors_requested"], {"backend": backend})
                self.assertNotIn("JCE_GPU_ADAPTER", observed["environment"])
                self.assertNotIn("JCE_GPU_TIER", observed["environment"])
        report, _ = self._scheduled_cli_probe("truncated", selectors={"gpu_tier": "high"})
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["renderer_selectors_requested"], {"gpu_tier": "high"})

    def test_d3d12_cli_keeps_normalized_profile_environment_and_request_parity(self):
        report, observed = self._scheduled_cli_probe(selectors={"backend": " D3D12 "})
        self.assertEqual(observed["preferences"], {"startup_mode": 1, "renderer": "D3D12"})
        self.assertIsNone(observed["session"])
        self.assertEqual(report["renderer_selectors_requested"], {"backend": "d3d12"})
        self.assertEqual(observed["environment"]["JCE_BACKEND"], "d3d12")
        self.assertEqual(report["environment"]["JCE_BACKEND"], "d3d12")
        for key in ("JCE_GPU_ADAPTER", "JCE_GPU_TIER"):
            self.assertNotIn(key, observed["environment"])
        self.assertEqual(report["status"], "PLAY_STOP_AUTHORED_SCENE_RESTORATION_OBSERVED")
        self.assertEqual(report["backend_actual"], "UNAVAILABLE_COMPILED_OUT_INFO")

    def test_d3d12_missing_cli_forwarding_and_wrong_profile_fail_ordinary_assertions(self):
        for name, old, replacement in (
            ("main", "**selectors, **history_options", "**{}, **history_options"),
            ("probe", '"d3d12": "D3D12"', '"d3d12": "D3D11"')):
            with self.subTest(mutation=name):
                original = getattr(lifecycle, name)
                source = inspect.getsource(original)
                self.assertEqual(source.count(old), 1)
                namespace = dict(lifecycle.__dict__)
                exec(compile(source.replace(old, replacement), lifecycle.__file__, "exec"), namespace)
                compiled = namespace[name]
                mutant = types.FunctionType(compiled.__code__, lifecycle.__dict__, argdefs=compiled.__defaults__)
                mutant.__kwdefaults__ = compiled.__kwdefaults__
                with patch.object(lifecycle, name, mutant):
                    with self.assertRaisesRegex(AssertionError, "renderer"):
                        self.test_d3d12_cli_keeps_normalized_profile_environment_and_request_parity()

    def test_absent_selectors_keep_existing_profile_environment_and_result_shape(self):
        report, observed = self._scheduled_cli_probe()
        self.assertEqual(observed["preferences_text"], '{"startup_mode":1}\n')
        self.assertIsNone(observed["session"])
        self.assertNotIn("renderer_selectors_requested", report)
        for key in ("JCE_BACKEND", "JCE_GPU_ADAPTER", "JCE_GPU_TIER"):
            self.assertNotIn(key, observed["environment"])

    def test_invalid_selector_cli_is_usage_error_before_probe(self):
        for flag, value in (("--backend", "dx11"), ("--backend", "noop"),
                            ("--gpu-adapter", "0x10de"), ("--gpu-adapter", "discrete"),
                            ("--gpu-tier", "2"), ("--gpu-tier", "")):
            with self.subTest(flag=flag, value=value), patch.object(sys, "argv", ["editor_lifecycle_probe.py",
                 "--exe", "absent.exe", "--scene", "absent.scene", "--out", "unused", flag, value]), \
                 patch.object(sys, "stderr", io.StringIO()), patch.object(lifecycle, "probe") as run:
                with self.assertRaises(SystemExit) as error:
                    lifecycle.main()
                self.assertEqual(error.exception.code, 2)
                run.assert_not_called()

    def test_cli_trace_reaches_existing_probe_without_new_loop(self):
        for flag, expected in (([], False), (["--trace"], True)):
            argv = ["editor_lifecycle_probe.py", "--exe", "application.exe",
                    "--scene", "scene.jbundle", "--out", "build/fixture", *flag]
            with patch.object(sys, "argv", argv), patch.object(lifecycle, "probe", return_value="result.json") as run, patch("builtins.print"):
                lifecycle.main()
            self.assertEqual(run.call_args.args[-1], expected)
            self.assertFalse(run.call_args.kwargs["resource_trace"])
            self.assertEqual(run.call_args.args[:3], (Path("application.exe"), Path("scene.jbundle"), Path("build/fixture")))

    def test_resource_trace_only_adds_explicit_default_rate_flag_to_normalized_environment(self):
        root = Path("build/resource-control").resolve()
        scene = Path("scene.jbundle").resolve()
        with patch.dict("os.environ", {"JCE_RESOURCE_TRACE": "0", "JCE_RESOURCE_TRACE_MS": "250"}):
            trace = lifecycle_environment(scene, root, trace=True)
            resources = lifecycle_environment(scene, root, trace=True, resource_trace=True)
        self.assertEqual(resources.pop("JCE_RESOURCE_TRACE"), "1")
        self.assertEqual(resources, trace)
        self.assertNotIn("JCE_RESOURCE_TRACE_MS", resources)

    def test_resource_without_trace_refuses_before_environment_and_child_launch(self):
        with patch.object(lifecycle, "probe_environment") as environment:
            with self.assertRaisesRegex(ValueError, "--resource-trace requires --trace"):
                lifecycle_environment(Path("scene.jbundle"), Path("build/unused"), resource_trace=True)
        environment.assert_not_called()
        with patch.object(Path, "resolve") as resolve, patch.object(lifecycle, "run_process") as run:
            with self.assertRaisesRegex(ValueError, "--resource-trace requires --trace"):
                lifecycle.probe("absent.exe", "absent.scene", "build/unused", resource_trace=True)
        resolve.assert_not_called()
        run.assert_not_called()

    def test_cli_resource_trace_reaches_same_probe_and_preserves_trace_parameter(self):
        argv = ["editor_lifecycle_probe.py", "--exe", "application.exe", "--scene", "scene.jbundle",
                "--out", "build/fixture", "--trace", "--resource-trace"]
        with patch.object(sys, "argv", argv), patch.object(lifecycle, "probe", return_value="result.json") as run, patch("builtins.print"):
            lifecycle.main()
        self.assertTrue(run.call_args.args[-1])
        self.assertTrue(run.call_args.kwargs["resource_trace"])

    def test_cli_resource_trace_without_trace_is_usage_error_before_probe(self):
        argv = ["editor_lifecycle_probe.py", "--exe", "absent.exe", "--scene", "absent.scene",
                "--out", "build/unused", "--resource-trace"]
        stderr = io.StringIO()
        with patch.object(sys, "argv", argv), patch.object(sys, "stderr", stderr), patch.object(lifecycle, "probe") as run:
            with self.assertRaises(SystemExit) as error:
                lifecycle.main()
        self.assertEqual(error.exception.code, 2)
        self.assertIn("--resource-trace requires --trace", stderr.getvalue())
        run.assert_not_called()

    def test_explicit_environment_retains_stop_playing_tick_semantics(self):
        root = Path("build/lifecycle-control").resolve()
        env = lifecycle_environment(Path("scene.jbundle").resolve(),root)
        self.assertEqual(env["JCE_MAX_FRAMES"],"240")
        self.assertEqual(env["JCE_DBG_SCENE_OBSERVE_AT"],"10,80,220")
        self.assertEqual(env["JCE_DBG_PLAY_STOP_AT"],"160")
        self.assertEqual(env["JCE_WINDOW_HIDDEN"],"1")
        self.assertEqual(env["JCE_FRAME_DT_FIXED"],"0.016666")

    def _scheduled_cli_probe(self, mode="valid", *, plan=(840, 20, 780, 10, 600, 820, 640), selectors=None,
                             visible_not_focusable=False, vsync=None):
        """Only the owned native child is replaced; parsers/validators run intact."""
        frames, play_at, stop_at, before_at, during_at, after_at, shot_at = plan
        during_frames = (during_at,) if isinstance(during_at, int) else tuple(during_at)
        observe_frames = (before_at, *during_frames, after_at)
        with tempfile.TemporaryDirectory(prefix="jce-lifecycle-schedule-") as temporary:
            folder = Path(temporary)
            exe, scene, output = folder / "editor.exe", folder / "scene.json", folder / "evidence"
            exe.write_bytes(b"typed child-port fixture; never executed")
            scene.write_text(json.dumps(snapshot()), encoding="utf-8")
            observed = {}

            def child(command, *, env, cwd, timeout, output_dir, marker_path):
                self.assertEqual(command, [str(exe.resolve())])
                self.assertEqual(cwd, str(folder.resolve()))
                self.assertEqual(timeout, 180)
                observed.update(environment=dict(env), root=Path(output_dir).parent)
                root = observed["root"]
                observed["preferences_text"] = (root / "profile" / "editor-preferences.json").read_text(encoding="utf-8")
                observed["preferences"] = json.loads(observed["preferences_text"])
                session = root / "profile" / "editor-session.json"
                observed["session"] = json.loads(session.read_text(encoding="utf-8")) if session.exists() else None
                self.assertEqual(marker_path, root / "frames.csv")
                self.assertEqual([env[key] for key in ("JCE_MAX_FRAMES", "JCE_DBG_AUTOPLAY",
                    "JCE_DBG_PLAY_STOP_AT", "JCE_DBG_SCENE_OBSERVE_AT", "JCE_WINCAP_FRAME")],
                    [str(frames), str(play_at), str(stop_at), ",".join(map(str, observe_frames)), str(shot_at)])
                self.assertEqual(env["JCE_FRAME_DT_FIXED"], "0.016666")
                self.assertEqual(env["JCE_WINDOW_HIDDEN"], "0" if visible_not_focusable else "1")
                if visible_not_focusable:
                    self.assertEqual(env["JCE_WINDOW_NOT_FOCUSABLE"], "1")
                else:
                    self.assertNotIn("JCE_WINDOW_NOT_FOCUSABLE", env)
                if vsync is None:
                    self.assertNotIn("JCE_LAUNCH_VSYNC", env, "inherited VSync must remain sanitized")
                else:
                    self.assertEqual(env.get("JCE_LAUNCH_VSYNC"), "1" if vsync == "on" else "0",
                                     "explicit VSync must reach the actual child environment")
                self.assertNotIn("JCE_KPI_GAME_SHOTS", env)
                Path(output_dir).mkdir()
                (root / "engine.log").write_text("fresh schedule fixture\n", encoding="utf-8")
                count = frames - 1 if mode == "truncated" else frames
                admitted_play = play_at + (30 if mode == "deferred_play" else 0)
                create = during_frames[0] + 10 if mode == "late_create" else admitted_play - 1
                stop_frame = admitted_play + stop_at - 1
                if mode == "early_teardown":
                    stop_frame = during_frames[-1] - 10
                phase_frames = dict(zip(lifecycle.PHASES, (admitted_play - 1, create, stop_frame, stop_frame)))
                rows = ["frame_index,phase,ms\n"]
                for frame in range(count):
                    rows.append("%d,frame_wall,1\n" % frame)
                    rows.extend("%d,%s,0.125\n" % (frame, name)
                                for name, at in phase_frames.items() if frame == at)
                marker_path.write_text("".join(rows), encoding="utf-8")
                during = snapshot()
                during["scene"]["entities"][1]["components"][0]["value"] = 8
                scene_values = [(before_at, snapshot()), (after_at, snapshot(101, 102))]
                for index, frame in enumerate(during_frames):
                    value = snapshot()
                    value["scene"]["entities"][1]["components"][0]["value"] = 8 + index
                    scene_values.append((frame, value))
                for frame, value in scene_values:
                    if mode == "missing_snapshot" and frame == during_frames[0]:
                        frame = 80  # A stale default name must not satisfy this plan.
                    if mode == "second_static" and frame == during_frames[-1]:
                        value = snapshot()
                    if mode == "bad_restore" and frame == after_at:
                        value = during
                    (root / ("scene-%d.json" % frame)).write_text(json.dumps(value), encoding="utf-8")
                def chunk(tag, payload):
                    return (struct.pack(">I", len(payload)) + tag + payload +
                            struct.pack(">I", zlib.crc32(tag + payload)))
                pixels = bytes(channel for x in range(320) for channel in (x % 256,) * 3)
                png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 320, 240, 8, 2, 0, 0, 0))
                png += chunk(b"IDAT", zlib.compress((b"\0" + pixels) * 240)) + chunk(b"IEND", b"")
                Path(env["JCE_WINCAP_PATH"]).write_bytes(png)
                return dict(timed_out=False, returncode=0, elapsed_s=.1, stdout="", stderr="")

            argv = ["editor_lifecycle_probe.py", "--exe", str(exe), "--scene", str(scene), "--out", str(output),
                    "--frames", str(frames), "--play-at", str(play_at), "--stop-at", str(stop_at),
                    "--observe-at", *map(str, observe_frames), "--shot-at", str(shot_at), "--require-mutation"]
            for name, value in (selectors or {}).items():
                argv.extend(("--" + name.replace("_", "-"), value))
            if visible_not_focusable:
                argv.append("--visible-not-focusable")
            if vsync is not None:
                argv.extend(("--vsync", vsync))
            with patch.object(sys, "argv", argv), patch.object(lifecycle, "run_process", side_effect=child) as run, \
                 patch.object(sys, "stdout", io.StringIO()):
                self.assertIs(lifecycle.probe.__globals__["run_process"], run)
                if mode in ("valid", "deferred_play"):
                    try:
                        lifecycle.main()
                    except (ValueError, FileNotFoundError) as error:
                        self.fail("extended schedule was rejected: %s" % error)
                else:
                    with self.assertRaises((ValueError, FileNotFoundError)):
                        lifecycle.main()
                self.assertEqual(run.call_count, 1)
            return json.loads((observed["root"] / "result.json").read_text(encoding="utf-8")), observed

    def test_nondefault_cli_schedule_reaches_actual_environment_readers_and_validators(self):
        for plan in ((840, 20, 780, 10, 600, 820, 640), (900, 20, 780, 10, 620, 880, 660),
                     (900, 500, 60, 10, 520, 600, 540)):
            with self.subTest(plan=plan):
                frames, play_at, stop_at, before, during, after, shot_at = plan
                report, observed = self._scheduled_cli_probe(plan=plan)
                self.assertEqual(report["status"], "PLAY_STOP_AUTHORED_SCENE_RESTORATION_OBSERVED")
                self.assertEqual(report["schedule"], dict(frames=frames, play_at=play_at, stop_at=stop_at,
                    observe_at=[before, during, after], shot_at=shot_at, earliest_stop_editor_tick=play_at + stop_at,
                    stop_editor_tick_basis="requested play_at + stop_at; deferred Play admission may delay Stop; "
                                           "actual positive phase observations remain authoritative",
                    stop_at_unit="PLAYING ticks", other_frame_unit="editor ticks"))
                self.assertEqual(report["complete_frames"], frames)
                self.assertEqual(report["phases"]["ed_play_restore"]["frame"], play_at + stop_at - 1)
                self.assertEqual(report["window_size"], [320, 240])
                self.assertTrue(report["scene_control"]["during_mutation_observed"])
                self.assertNotIn("scene_controls", report)
                self.assertEqual(report["environment"], {key: value for key, value in observed["environment"].items()
                                                       if key.startswith("JCE_")})

    def test_multiple_during_cli_snapshots_use_actual_phase_and_scene_validators(self):
        plan = (900, 20, 780, 10, (600, 660), 880, 640)
        for mode in ("valid", "deferred_play"):
            with self.subTest(mode=mode):
                report, _ = self._scheduled_cli_probe(mode, plan=plan)
                self.assertEqual(report["status"], "PLAY_STOP_AUTHORED_SCENE_RESTORATION_OBSERVED")
                self.assertEqual(report["schedule"]["observe_at"], [10, 600, 660, 880])
                self.assertEqual(report["schedule"]["earliest_stop_editor_tick"], 800)
                self.assertEqual(report["phases"]["ed_play_restore"]["frame"], 829 if mode == "deferred_play" else 799)
                self.assertEqual(len(report["scene_controls"]), 2)
                self.assertEqual(report["scene_control"], report["scene_controls"][0])
                self.assertTrue(all(control["during_mutation_observed"] for control in report["scene_controls"]))
        for mode, reason in (("second_static", "required runtime scene mutation was not observed"),
                             ("early_teardown", "out of order")):
            with self.subTest(mode=mode):
                report, _ = self._scheduled_cli_probe(mode, plan=plan)
                self.assertEqual(report["status"], "FAIL")
                self.assertIn(reason, report["error"])

    def test_nondefault_cli_rejects_incomplete_phases_snapshots_and_restoration(self):
        for mode, reason in (("truncated", "840 contiguous"), ("late_create", "out of order"),
                             ("missing_snapshot", "scene-600.json"), ("bad_restore", "not restored")):
            with self.subTest(mode=mode):
                report, _ = self._scheduled_cli_probe(mode)
                self.assertEqual(report["status"], "FAIL")
                self.assertIn(reason, report["error"])
                self.assertEqual(report["schedule"]["observe_at"], [10, 600, 820])

    def test_old_default_phase_wire_is_rejected_by_nondefault_cli_assertion(self):
        source = inspect.getsource(lifecycle.probe)
        old = "validate_phases(frame_samples, **schedule)"
        self.assertEqual(source.count(old), 1)
        namespace = dict(lifecycle.__dict__)
        exec(compile(source.replace(old, "validate_phases(frame_samples)"), lifecycle.__file__, "exec"), namespace)
        compiled = namespace["probe"]
        mutant = types.FunctionType(compiled.__code__, lifecycle.__dict__, argdefs=compiled.__defaults__)
        mutant.__kwdefaults__ = compiled.__kwdefaults__
        with patch.object(lifecycle, "probe", mutant):
            with self.assertRaisesRegex(AssertionError, "exactly 240 contiguous frames"):
                self._scheduled_cli_probe()

    def test_schedule_validation_refuses_before_any_path_environment_or_child_access(self):
        invalid = [dict(frames=value) for value in (239, 10001, True, 240.0, None, "240")]
        invalid += [dict(**{key: value}) for key in ("play_at", "stop_at", "shot_at")
                    for value in (True, 20.0, "20", None)]
        invalid += [dict(observe_at=value) for value in (None, "10,80,220", (), (10, 80),
                    (10, 120, 80, 220), (10, 80, 80, 220), (10, 80, 200, 220),
                    (10, *range(30, 45), 220), (True, 80, 220), (10, 80.0, 220), (0, 80, 220),
                    (20, 80, 220), (10, 20, 220), (10, 180, 220), (10, 80, 160), (10, 80, 240))]
        invalid += [dict(shot_at=20), dict(shot_at=180), dict(stop_at=0), dict(stop_at=-1),
                    dict(play_at=60, stop_at=170),
                    dict(frames=900, play_at=500, stop_at=60, observe_at=(10, 520, 560), shot_at=540)]
        for options in invalid:
            with self.subTest(options=options), patch.object(Path, "resolve") as resolve, \
                 patch.object(Path, "mkdir") as mkdir, patch.object(Path, "open") as opened, \
                 patch.object(lifecycle, "probe_environment") as environment, patch.object(lifecycle, "run_process") as run:
                with self.assertRaises(ValueError):
                    lifecycle_environment(Path("absent.scene"), Path("unused"), **options)
                with self.assertRaises(ValueError):
                    lifecycle.probe("absent.exe", "absent.scene", "unused", **options)
                for operation in (resolve, mkdir, opened, environment, run):
                    operation.assert_not_called()
        for frames in (240, 10000):
            self.assertEqual(lifecycle_environment(Path("scene"), Path("root"), frames=frames)["JCE_MAX_FRAMES"], str(frames))
        maximum_observations = (10, *range(30, 44), 220)
        env = lifecycle_environment(Path("scene"), Path("root"), observe_at=maximum_observations)
        self.assertEqual(env["JCE_DBG_SCENE_OBSERVE_AT"], ",".join(map(str, maximum_observations)))

    def test_invalid_cli_schedule_is_usage_error_before_probe(self):
        for flags in (["--frames", "239"], ["--frames", "10001"], ["--frames", "840.0"],
                      ["--shot-at", "20"], ["--observe-at", "10", "80"],
                      ["--observe-at", "10", "220", "80"],
                      ["--observe-at", "10", "120", "80", "220"],
                      ["--observe-at", "10", *map(str, range(30, 45)), "220"]):
            with self.subTest(flags=flags), patch.object(sys, "argv", ["editor_lifecycle_probe.py", "--exe", "absent.exe",
                 "--scene", "absent.scene", "--out", "unused", *flags]), \
                 patch.object(sys, "stderr", io.StringIO()), patch.object(lifecycle, "probe") as run:
                with self.assertRaises(SystemExit) as error:
                    lifecycle.main()
                self.assertEqual(error.exception.code, 2)
                run.assert_not_called()

    def _terminal_probe(self, *, enabled=True, trace=False, resource_trace=False, shader_provenance=False, filesystem_open_failures=False, scope_witnesses=False, scope_prefixes=(), prefix_activity=True, history=False, mode="valid", cli=False):
        captured = {}
        with tempfile.TemporaryDirectory(prefix="jce-lifecycle-terminal-") as temporary:
            folder = Path(temporary)
            exe, scene, output = folder / "editor.exe", folder / "scene.json", folder / "evidence"
            exe.write_bytes(b"fixture executable; process launch is mocked")
            scene.write_text(json.dumps(snapshot()), encoding="utf-8")
            def child(command, **kwargs):
                env = kwargs["env"]
                captured["environment"] = dict(env)
                process_root = Path(kwargs["output_dir"])
                root = process_root.parent
                process_root.mkdir()
                (root / "engine.log").write_text("fresh lifecycle fixture\n", encoding="utf-8")
                (root / "frames.csv").write_text("parsed through the existing mocked reader\n", encoding="utf-8")
                (root / "window.png").write_bytes(b"image validation is isolated in this ACK control")
                for frame, value in ((10, snapshot()), (80, snapshot()), (220, snapshot(101, 102))):
                    (root / ("scene-%d.json" % frame)).write_text(json.dumps(value), encoding="utf-8")
                value = packet(token=env.get("JCE_TERMINAL_ACK_TOKEN", TOKEN))
                if trace:
                    value.update(cleanup_scope="10", trace_status="complete", trace_published=True, trace_write_ok=True)
                    document = trace_document()
                    if shader_provenance or filesystem_open_failures or scope_witnesses or scope_prefixes:
                        # Reuse typed receipt fixtures; no second trace producer/parser.
                        for event in document["traceEvents"]:
                            if event["ts"] >= 100:
                                event["ts"] += 1000
                        if shader_provenance:
                            document["traceEvents"] += (shader_create(100, 5, 10, parent=1, thread=77) +
                                shader_create(101, 6, 45, parent=1, thread=77) +
                                shader_link(102, 100, 101, 80, parent=1, thread=77))
                        if filesystem_open_failures:
                            document["traceEvents"] += filesystem_sample(200, parent=1, thread=77, timestamp=200)
                        if scope_witnesses:
                            for task, name, start in ((300, "editor.play.begin", 400), (301, "editor.play.stop", 500)):
                                document["traceEvents"].extend([
                                    dict(ph="i", cat="task", pid=1, tid=77, ts=start-1, name=name, args=dict(task=task, parent=0, kind=2)),
                                    dict(ph="B", cat="task", pid=1, tid=77, ts=start, name=name, args=dict(id=task, parent=task)),
                                    dict(ph="E", cat="task", pid=1, tid=77, ts=start+5, name=name, args=dict(id=task, parent=task, ns=5000, state=0))])
                        if scope_prefixes and prefix_activity:
                            for identity, start in ((400, 600), (401, 620)):
                                document["traceEvents"].extend([
                                    dict(ph="B", cat="render", pid=1, tid=77, ts=start, name="bgfx.cpu.submit", args=dict(id=identity, parent=0)),
                                    dict(ph="E", cat="render", pid=1, tid=77, ts=start+10, name="bgfx.cpu.submit", args=dict(id=identity, parent=0, ns=9000))])
                        document["traceEvents"].sort(key=lambda event: event["ts"])
                        document["jceTrace"].update(recorded=len(document["traceEvents"]),
                                                  captured=len(document["traceEvents"]))
                    (root / "lifecycle-trace.json").write_text(json.dumps(document), encoding="utf-8")
                if mode == "bad_token": value["token"] = "f" * 32
                if mode == "trace_mismatch": value["session"] = "2"
                if mode == "trace_unavailable": value.update(trace_status="unavailable", trace_published=False, trace_write_ok=False)
                raw = encoded(value) if mode != "missing" else b"no terminal packet\n"
                if mode == "duplicate": raw += raw
                stdout, stderr = process_root / "stdout.log", process_root / "stderr.log"
                stdout.write_bytes(b"child stdout\n")
                stderr.write_bytes(raw)
                captured["root"], captured["stderr"] = root, stderr
                result = dict(timed_out=False, returncode=0, elapsed_s=.1,
                    stdout_path=str(stdout), stderr_path=str(stderr), stdout="child stdout\n",
                    # A forged cached packet cannot substitute for the original owned stderr.
                    stderr=encoded(packet(token=TOKEN)).decode(),
                    process_identity=dict(pid=123, creation_time_unix_s=100.0, creation_time_source="psutil.create_time"))
                if mode == "foreign_stderr": result["stderr_path"] = str(root / "foreign.log")
                return result
            failure = mode != "valid"
            options = dict(trace=trace, resource_trace=resource_trace, require_terminal_ack=enabled)
            if history:
                captured["history_path"] = folder / "history.sqlite"
                options.update(trace_history_db_path=captured["history_path"],
                               disk_history_max_records=10000, disk_history_max_bytes=2 * 1024 * 1024)
            if shader_provenance:
                options["shader_provenance"] = True
            if filesystem_open_failures:
                options["filesystem_open_failures"] = True
            if scope_witnesses:
                options["scope_witnesses"] = True
            if scope_prefixes:
                options["scope_prefixes"] = scope_prefixes
            def invoke():
                if not cli:
                    return lifecycle.probe(exe, scene, output, **options)
                argv = ["editor_lifecycle_probe.py", "--exe", str(exe), "--scene", str(scene), "--out", str(output)]
                if trace: argv.append("--trace")
                if enabled: argv.append("--require-terminal-ack")
                for prefix in scope_prefixes: argv.extend(("--scope-prefix", prefix))
                with patch.object(sys, "argv", argv), patch.object(sys, "stdout", io.StringIO()):
                    return lifecycle.main()
            with patch.object(lifecycle, "run_process", side_effect=child), \
                 patch.object(lifecycle, "read_frames", return_value=samples()) as frames, \
                 patch.object(lifecycle, "read_png", return_value=(640, 480, 4, b"")), \
                 patch.object(lifecycle, "validate_image", return_value=[640, 480]), \
                 patch.object(lifecycle.secrets, "token_hex", return_value=TOKEN), \
                 patch.object(terminal, "observe_terminal_ack", wraps=terminal.observe_terminal_ack) as observer, \
                 patch.object(trace_reader, "analyze_trace_file", wraps=trace_reader.analyze_trace_file) as trace_analysis, \
                 patch.object(lifecycle, "sha256", wraps=lifecycle.sha256) as hashes, \
                 patch.dict("os.environ", {"JCE_TERMINAL_ACK_TOKEN": "e" * 32}):
                if failure:
                    with self.assertRaisesRegex(ValueError, "terminal ack|terminal session|resource samples"):
                        invoke()
                else:
                    try:
                        invoke()
                    except TypeError as error:
                        if scope_witnesses and "scope_witnesses" in str(error):
                            self.fail("required lifecycle scope witness API unavailable: %s" % error)
                        raise
                captured["observer_kwargs"] = observer.call_args.kwargs if observer.call_args else None
                captured["observer_calls"] = observer.call_count
                captured["trace_analysis_calls"] = trace_analysis.call_count
                captured["trace_analysis_kwargs"] = trace_analysis.call_args.kwargs if trace_analysis.call_args else None
                captured["frame_reader_calls"] = frames.call_count
                captured["hashed_paths"] = [Path(call.args[0]) for call in hashes.call_args_list]
            report = json.loads((captured["root"] / "result.json").read_text(encoding="utf-8"))
            captured["trace_path"] = captured["root"] / "lifecycle-trace.json"
            return report, captured

    def assert_prefix_probe(self, report, observed, enabled):
        self.assertEqual(report["status"], "PLAY_STOP_AUTHORED_SCENE_RESTORATION_OBSERVED")
        self.assertEqual(report["scope_prefixes_requested"], ["bgfx.cpu.", "absent."])
        self.assertEqual(observed["trace_analysis_calls"], 1)
        self.assertIn("scope_prefixes", observed["trace_analysis_kwargs"])
        self.assertEqual(observed["trace_analysis_kwargs"]["scope_prefixes"], ("bgfx.cpu.", "absent."))
        analyzed = report["terminal_ack"]["trace_report"] if enabled else report["trace_report"]
        self.assertIn("scope_prefix_observations", analyzed)
        row, = analyzed["scope_prefix_observations"]["aggregates"]
        self.assertEqual((row["name"], row["thread"], row["observed_count"], row["measured_ns_sum"]),
                         ("bgfx.cpu.submit", 77, 2, 18000))
        self.assertEqual(analyzed["scope_prefix_observations"]["prefixes"][-1]["status"], "NO_ACTIVITY")
        self.assertEqual(report["artifacts"]["lifecycle-trace.json"], analyzed["sha256"])
        self.assertNotIn(observed["trace_path"], observed["hashed_paths"])

    def test_scope_prefixes_both_routes_use_original_canonical_reader_and_keep_environment(self):
        baseline, base_observed = self._terminal_probe(trace=True)
        for enabled in (False, True):
            with self.subTest(terminal_ack=enabled):
                report, observed = self._terminal_probe(enabled=enabled, trace=True, scope_prefixes=["bgfx.cpu.", "absent."])
                self.assert_prefix_probe(report, observed, enabled)
                expected_env = dict(base_observed["environment"])
                for key in expected_env:
                    expected_env[key] = expected_env[key].replace(str(base_observed["root"]), str(observed["root"]))
                    expected_env[key] = expected_env[key].replace(str(base_observed["root"].parent.parent),
                                                                 str(observed["root"].parent.parent))
                if not enabled: expected_env.pop("JCE_TERMINAL_ACK_TOKEN")
                self.assertEqual(observed["environment"], expected_env)
                analyzed = report["terminal_ack"]["trace_report"] if enabled else report["trace_report"]
                self.assertEqual(analyzed["required"], ["engine.log_quiesce"])
        self.assertNotIn("scope_prefixes_requested", baseline)
        self.assertNotIn("scope_prefix_observations", baseline["terminal_ack"]["trace_report"])

    def test_scope_prefixes_no_activity_remains_explicit_and_composes_with_witnesses(self):
        for enabled in (False, True):
            report, observed = self._terminal_probe(enabled=enabled, trace=True, scope_prefixes=["bgfx.cpu."],
                prefix_activity=False, scope_witnesses=True)
            analyzed = report["terminal_ack"]["trace_report"] if enabled else report["trace_report"]
            self.assertEqual(analyzed["scope_prefix_observations"]["status"], "NO_ACTIVITY")
            self.assertIn("required_scope_witnesses", analyzed)
            self.assertEqual(observed["trace_analysis_calls"], 1)
            self.assertEqual(report["status"], "PLAY_STOP_AUTHORED_SCENE_RESTORATION_OBSERVED")

    def test_actual_scope_prefix_cli_both_routes_reach_single_complete_reader(self):
        for enabled in (False, True):
            with self.subTest(terminal_ack=enabled):
                report, observed = self._terminal_probe(enabled=enabled, trace=True,
                    scope_prefixes=["bgfx.cpu.", "absent."], cli=True)
                self.assert_prefix_probe(report, observed, enabled)
                self.assertEqual(observed["observer_calls"], int(enabled))

    def test_scope_prefix_invalid_or_absent_trace_refuses_before_io_environment_or_child(self):
        requests = [(True, value) for value in (None, True, "x", [""], [" x"], ["x\n"], [1],
                    ["x" * 129], ["x", "x"], ["p.%d" % i for i in range(129)])]
        requests.append((False, ["bgfx.cpu."]))
        for trace, prefixes in requests:
            with self.subTest(trace=trace, prefixes=prefixes), patch.object(Path, "resolve") as resolve, \
                    patch.object(Path, "open") as opened, patch.object(Path, "mkdir") as mkdir, \
                    patch.object(lifecycle, "probe_environment") as environment, patch.object(lifecycle, "run_process") as run:
                with self.assertRaisesRegex(ValueError, "scope prefix|scope-prefix"):
                    lifecycle.probe("absent.exe", "absent.scene", "unused", trace=trace, scope_prefixes=prefixes)
                for operation in (resolve, opened, mkdir, environment, run): operation.assert_not_called()
                with self.assertRaisesRegex(ValueError, "scope prefix|scope-prefix"):
                    lifecycle.lifecycle_environment(Path("scene"), Path("root"), trace=trace, scope_prefixes=prefixes)
                environment.assert_not_called()

    def test_scope_prefix_cli_is_repeatable_and_invalid_requests_never_reach_probe(self):
        base = ["editor_lifecycle_probe.py", "--exe", "absent.exe", "--scene", "scene", "--out", "unused"]
        flags = ["--trace", "--scope-prefix", "bgfx.cpu.", "--scope-prefix", "absent."]
        with patch.object(sys, "argv", base + flags), patch.object(lifecycle, "probe", return_value="result.json") as run, \
                patch.object(sys, "stdout", io.StringIO()):
            lifecycle.main()
        self.assertEqual(run.call_args.kwargs["scope_prefixes"], ("bgfx.cpu.", "absent."))
        for flags in (["--scope-prefix", "x"], ["--trace", "--scope-prefix", ""],
                      ["--trace", "--scope-prefix", "x", "--scope-prefix", "x"]):
            with self.subTest(flags=flags), patch.object(sys, "argv", base + flags), \
                    patch.object(sys, "stderr", io.StringIO()), patch.object(lifecycle, "probe") as run:
                with self.assertRaises(SystemExit) as failed: lifecycle.main()
                self.assertEqual(failed.exception.code, 2)
                run.assert_not_called()

    def test_ordinary_missing_probe_prefix_forward_fails_the_same_activity_assertion(self):
        source = Path(lifecycle.__file__).read_text(encoding="utf-8")
        wire = '        analysis_options["scope_prefixes"] = scope_prefixes\n'
        self.assertEqual(source.count(wire), 1)
        namespace = dict(__name__="editor_lifecycle_missing_prefix", __file__=lifecycle.__file__)
        exec(compile(source.replace(wire, "        pass\n", 1), lifecycle.__file__, "exec"), namespace)
        # Keep actual probe logic/readers; only its isolated process/PNG/frame ports
        # must refer to the same counted dependencies installed by the fixture.
        def mutated(*args, **kwargs):
            for name in ("run_process", "read_frames", "read_png", "validate_image", "sha256"):
                namespace[name] = getattr(lifecycle, name)
            return namespace["probe"](*args, **kwargs)
        with patch.object(lifecycle, "probe", mutated):
            report, observed = self._terminal_probe(trace=True, scope_prefixes=["bgfx.cpu.", "absent."])
        self.assertEqual(report["status"], "PLAY_STOP_AUTHORED_SCENE_RESTORATION_OBSERVED")
        with self.assertRaises(AssertionError):
            self.assert_prefix_probe(report, observed, True)

    def test_disk_history_ack_and_direct_routes_use_one_existing_canonical_store(self):
        self.assertIn("trace_history_db_path", inspect.signature(lifecycle.probe).parameters)
        for enabled in (False, True):
            with self.subTest(terminal_ack=enabled):
                report, observed = self._terminal_probe(enabled=enabled, trace=True, history=True)
                self.assertEqual(observed["trace_analysis_calls"], 1)
                analyzed = report["terminal_ack"]["trace_report"] if enabled else report["trace_report"]
                declared = report["trace_history_storage_requested"]
                self.assertEqual(declared, dict(path=str(observed["history_path"]), max_records=10000,
                                                max_bytes=2 * 1024 * 1024))
                storage = analyzed["history_storage"]
                self.assertEqual(storage["path"], declared["path"])
                self.assertTrue(storage["closed"])
                self.assertFalse(storage["failed"])
                self.assertEqual(analyzed["validation_limits"], dict(history=200000, active=8192, names=4096))
                self.assertEqual(observed["trace_analysis_kwargs"]["history_db_path"], observed["history_path"])
                self.assertEqual(observed["trace_analysis_kwargs"]["disk_history_max_records"], 10000)
                self.assertEqual(observed["trace_analysis_kwargs"]["disk_history_max_bytes"], 2 * 1024 * 1024)
                self.assertNotIn(observed["trace_path"], observed["hashed_paths"])
                if enabled:
                    self.assertEqual(observed["observer_kwargs"]["trace_history_db_path"], observed["history_path"])
                    self.assertEqual(observed["observer_kwargs"]["trace_disk_history_max_records"], 10000)
                    self.assertEqual(observed["observer_kwargs"]["trace_disk_history_max_bytes"], 2 * 1024 * 1024)
                self.assertNotIn("JCE_TRACE_HISTORY_DB", observed["environment"])

    def test_disk_history_composes_with_scope_shader_controls_and_failed_run_metadata(self):
        self.assertIn("trace_history_db_path", inspect.signature(lifecycle.probe).parameters)
        for enabled in (False, True):
            report, observed = self._terminal_probe(enabled=enabled, trace=True, history=True,
                scope_witnesses=True, shader_provenance=True, filesystem_open_failures=True)
            analyzed = report["terminal_ack"]["trace_report"] if enabled else report["trace_report"]
            self.assertEqual(observed["trace_analysis_calls"], 1)
            self.assertEqual(analyzed["shader_provenance"]["consumed_create_receipt_count"], 2)
            self.assertTrue(analyzed["history_storage"]["closed"])
            self.assertIn("required_scope_witnesses", analyzed)
        report, observed = self._terminal_probe(trace=True, history=True, resource_trace=True, mode="no_resources")
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["trace_history_storage_requested"]["max_records"], 10000)
        self.assertEqual(observed["trace_analysis_calls"], 1)
        self.assertTrue(observed["trace_analysis_kwargs"]["resource_observations"])

    def test_disk_history_defaults_leave_old_ack_and_direct_behavior(self):
        report, observed = self._terminal_probe(enabled=False, trace=True)
        self.assertNotIn("trace_history_storage_requested", report)
        self.assertNotIn("trace_report", report)
        self.assertEqual(observed["trace_analysis_calls"], 0)
        report, observed = self._terminal_probe(trace=True)
        self.assertNotIn("trace_history_storage_requested", report)
        self.assertNotIn("history_storage", report["terminal_ack"]["trace_report"])
        for key in ("trace_history_db_path", "trace_disk_history_max_records", "trace_disk_history_max_bytes"):
            self.assertNotIn(key, observed["observer_kwargs"])

    def test_invalid_disk_history_tuple_refuses_before_io_environment_or_child(self):
        self.assertIn("trace_history_db_path", inspect.signature(lifecycle.probe).parameters)
        path = Path("build/fresh-history.sqlite").absolute()
        valid = dict(trace=True, trace_history_db_path=path, disk_history_max_records=10000,
                     disk_history_max_bytes=2097152)
        invalid = [{key: value for key, value in valid.items() if key != missing}
                   for missing in ("trace_history_db_path", "disk_history_max_records", "disk_history_max_bytes")]
        invalid += [dict(valid, trace=False), dict(valid, trace_history_db_path=Path("relative.sqlite"))]
        invalid += [dict(valid, **{key: value}) for key in ("disk_history_max_records", "disk_history_max_bytes")
                    for value in (True, 0, -1, 1.5, "2")]
        for options in invalid:
            with self.subTest(options=options), patch.object(Path, "resolve") as resolve, \
                 patch.object(Path, "mkdir") as mkdir, patch.object(Path, "open") as opened, \
                 patch.object(Path, "exists") as exists, patch.object(lifecycle, "probe_environment") as environment, \
                 patch.object(lifecycle, "run_process") as run:
                with self.assertRaises(ValueError):
                    lifecycle.probe("absent.exe", "absent.scene", "unused", **options)
                for operation in (resolve, mkdir, opened, exists, environment, run):
                    operation.assert_not_called()

    def test_existing_history_database_or_sidecar_refuses_before_launch_and_is_preserved(self):
        self.assertIn("trace_history_db_path", inspect.signature(lifecycle.probe).parameters)
        for suffix in ("", "-journal", "-wal", "-shm"):
            with self.subTest(suffix=suffix), tempfile.TemporaryDirectory(prefix="jce-history-freshness-") as temporary:
                path = Path(temporary) / "history.sqlite"
                retained = Path(str(path) + suffix)
                retained.write_bytes(b"existing owner must survive")
                with patch.object(Path, "resolve") as resolve, patch.object(Path, "mkdir") as mkdir, \
                     patch.object(lifecycle, "run_process") as run:
                    with self.assertRaisesRegex(ValueError, "already exists"):
                        lifecycle.probe("absent.exe", "absent.scene", "unused", trace=True,
                            trace_history_db_path=path, disk_history_max_records=10000,
                            disk_history_max_bytes=2097152)
                for operation in (resolve, mkdir, run):
                    operation.assert_not_called()
                self.assertEqual(retained.read_bytes(), b"existing owner must survive")

    def test_history_missing_parent_refuses_before_output_creation_or_child_launch(self):
        with tempfile.TemporaryDirectory(prefix="jce-history-parent-") as temporary:
            path = Path(temporary) / "absent" / "history.sqlite"
            with patch.object(Path, "resolve") as resolve, patch.object(Path, "mkdir") as mkdir, \
                 patch.object(lifecycle, "run_process") as run:
                with self.assertRaisesRegex(ValueError, "parent directory must exist"):
                    lifecycle.probe("absent.exe", "absent.scene", "unused", trace=True,
                        trace_history_db_path=path, disk_history_max_records=10000,
                        disk_history_max_bytes=2097152)
            for operation in (resolve, mkdir, run):
                operation.assert_not_called()

    def test_history_cli_forwards_explicit_tuple_and_rejects_invalid_plans_before_probe(self):
        path = Path("build/cli-history.sqlite").absolute()
        base = ["editor_lifecycle_probe.py", "--exe", "absent.exe", "--scene", "absent.scene", "--out", "unused"]
        flags = ["--trace", "--trace-history-db", str(path), "--disk-history-max-records", "10000",
                 "--disk-history-max-bytes", "2097152"]
        with patch.object(sys, "argv", base + flags), patch.object(lifecycle, "probe", return_value="result.json") as run, \
             patch.object(sys, "stdout", io.StringIO()), patch.object(sys, "stderr", io.StringIO()):
            try: lifecycle.main()
            except SystemExit as error: self.fail("supported disk history CLI rejected: %s" % error.code)
        self.assertEqual(run.call_args.kwargs["trace_history_db_path"], path)
        self.assertEqual(run.call_args.kwargs["disk_history_max_records"], 10000)
        self.assertEqual(run.call_args.kwargs["disk_history_max_bytes"], 2097152)
        invalid = [flags[1:], ["--trace", "--trace-history-db", str(path)],
                   ["--trace", "--disk-history-max-records", "10000"],
                   ["--trace", "--trace-history-db", "relative.sqlite", *flags[3:]],
                   [*flags[:-1], "0"], [*flags[:-1], "1.5"]]
        for extra in invalid:
            with self.subTest(extra=extra), patch.object(sys, "argv", base + extra), \
                 patch.object(sys, "stderr", io.StringIO()), patch.object(lifecycle, "probe") as run:
                with self.assertRaises(SystemExit) as error:
                    lifecycle.main()
                self.assertEqual(error.exception.code, 2)
                run.assert_not_called()

    def test_scope_witnesses_ack_and_noack_share_one_actual_reader(self):
        for enabled in (False, True):
            with self.subTest(terminal_ack=enabled):
                report, observed = self._terminal_probe(enabled=enabled, trace=True, scope_witnesses=True)
                self.assertEqual(observed["trace_analysis_calls"], 1)
                analyzed = report["terminal_ack"]["trace_report"] if enabled else report["trace_report"]
                self.assertIn("required_scope_witnesses", analyzed)
                self.assertTrue(report["scope_witnesses_requested"])
                rows = {row["name"]: row for row in analyzed["required_scope_witnesses"]["required"]}
                self.assertEqual(set(rows), {"editor.play.begin", "editor.play.stop", "engine.log_quiesce"})
                self.assertEqual((rows["editor.play.begin"]["first"]["begin_timestamp_us"], rows["editor.play.stop"]["last"]["end_timestamp_us"]), (400, 505))
                self.assertEqual(observed["frame_reader_calls"], 1)
                self.assertIs(observed["trace_analysis_kwargs"]["scope_witnesses"], True)
                self.assertNotIn(observed["trace_path"], observed["hashed_paths"])
                if enabled:
                    self.assertEqual(observed["observer_kwargs"]["trace_analysis_options"],
                        dict(scope_witnesses=True, required=["editor.play.begin", "editor.play.stop"]))

    def test_scope_shader_filesystem_composition_keeps_one_ack_and_noack_reader(self):
        for enabled in (False, True):
            with self.subTest(terminal_ack=enabled):
                report, observed = self._terminal_probe(enabled=enabled, trace=True,
                    scope_witnesses=True, shader_provenance=True, filesystem_open_failures=True)
                analyzed = report["terminal_ack"]["trace_report"] if enabled else report["trace_report"]
                self.assertIn("required_scope_witnesses", analyzed)
                self.assertEqual(analyzed["shader_provenance"]["consumed_create_receipt_count"], 2)
                self.assertEqual(analyzed["filesystem_read_details"]["complete_packets_count"], 1)
                self.assertEqual(analyzed["nonrequired_task_outcomes"]["failed"], 1)
                rows = {row["name"]: row for row in analyzed["required_scope_witnesses"]["required"]}
                self.assertEqual(set(rows), {"editor.play.begin", "editor.play.stop", "engine.log_quiesce"})
                self.assertEqual(rows["editor.play.begin"]["first"]["begin_timestamp_us"], 400)
                self.assertEqual(rows["editor.play.stop"]["last"]["end_timestamp_us"], 505)
                self.assertEqual(observed["trace_analysis_calls"], 1)
                self.assertEqual(observed["frame_reader_calls"], 1)
                self.assertNotIn(observed["trace_path"], observed["hashed_paths"])
                for option in ("scope_witnesses", "shader_provenance", "filesystem_read_details"):
                    self.assertIs(observed["trace_analysis_kwargs"][option], True)
                if enabled:
                    self.assertEqual(observed["observer_kwargs"]["trace_analysis_options"],
                        dict(scope_witnesses=True, shader_provenance=True, filesystem_read_details=True,
                             required=["editor.play.begin", "editor.play.stop"]))
                    self.assertIn("cleanup_ownership", analyzed)
                    self.assertEqual(report["terminal_ack"]["trace_binding"], "validated")

    def test_scope_witnesses_validation_precedes_io_and_adds_no_producer_flag(self):
        self.assertIn("scope_witnesses", inspect.signature(lifecycle.probe).parameters)
        self.assertIn("scope_witnesses", inspect.signature(lifecycle_environment).parameters)
        for value in (1, None, "true"):
            with self.subTest(value=value), patch.object(Path, "resolve") as resolve:
                with self.assertRaisesRegex(ValueError, "scope_witnesses must be boolean"):
                    lifecycle.probe("absent.exe", "absent.scene", "unused", trace=True, scope_witnesses=value)
                resolve.assert_not_called()
        with patch.object(Path, "resolve") as resolve:
            with self.assertRaisesRegex(ValueError, "requires --trace"):
                lifecycle.probe("absent.exe", "absent.scene", "unused", scope_witnesses=True)
            resolve.assert_not_called()
        plain = lifecycle_environment(Path("scene.json"), Path("root"), trace=True)
        requested = lifecycle_environment(Path("scene.json"), Path("root"), trace=True, scope_witnesses=True)
        self.assertEqual(requested, plain)

    def test_scope_witness_cli_opt_in_and_default_forward_to_same_probe(self):
        for expected, extra in ((False, []), (True, ["--trace", "--require-mutation", "--scope-witnesses"])):
            argv = ["editor_lifecycle_probe.py", "--exe", "x", "--scene", "s", "--out", "o"] + extra
            with patch.object(sys, "argv", argv), patch.object(lifecycle, "probe", return_value=Path("result.json")) as run, patch("sys.stdout", new=io.StringIO()):
                try: lifecycle.main()
                except SystemExit as error: self.fail("scope witness CLI rejected opt-in: %s" % error.code)
            self.assertIs(run.call_args.kwargs.get("scope_witnesses", False), expected)

    def test_scope_witness_cli_without_trace_refuses_before_probe(self):
        argv = ["editor_lifecycle_probe.py", "--exe", "x", "--scene", "s", "--out", "o", "--scope-witnesses"]
        with patch.object(sys, "argv", argv), patch.object(lifecycle, "probe") as run:
            with self.assertRaises(SystemExit) as caught: lifecycle.main()
            self.assertEqual(caught.exception.code, 2)
            run.assert_not_called()

    def test_shader_provenance_environment_only_adds_supported_post_filter_flag(self):
        root, scene = Path("build/shader-control"), Path("scene.jbundle")
        with patch.dict("os.environ", {"JCE_RENDER_RESOURCE_DIAG": "1", "JCE_OTHER": "inherited"}):
            default = lifecycle_environment(scene, root)
            trace = lifecycle_environment(scene, root, trace=True)
            shader = lifecycle_environment(scene, root, trace=True, shader_provenance=True)
        self.assertNotIn("JCE_RENDER_RESOURCE_DIAG", default)
        self.assertNotIn("JCE_RENDER_RESOURCE_DIAG", trace)
        self.assertNotIn("JCE_OTHER", shader)
        self.assertEqual(shader.pop("JCE_RENDER_RESOURCE_DIAG"), "1")
        self.assertEqual(shader, trace)

    def test_shader_provenance_requires_trace_before_environment_or_io(self):
        with patch.object(lifecycle, "probe_environment") as environment:
            with self.assertRaisesRegex(ValueError, "--shader-provenance requires --trace"):
                lifecycle_environment(Path("absent.scene"), Path("unused"), shader_provenance=True)
        environment.assert_not_called()
        with patch.object(Path, "resolve") as resolve, patch.object(lifecycle, "run_process") as run:
            with self.assertRaisesRegex(ValueError, "--shader-provenance requires --trace"):
                lifecycle.probe("absent.exe", "absent.scene", "unused", shader_provenance=True)
        resolve.assert_not_called()
        run.assert_not_called()

    def test_shader_provenance_strict_bool_before_environment_or_io(self):
        for value in (None, 0, 1, "true", []):
            with self.subTest(value=value), patch.object(lifecycle, "probe_environment") as environment, \
                 patch.object(Path, "resolve") as resolve, patch.object(lifecycle, "run_process") as run:
                with self.assertRaisesRegex(ValueError, "shader_provenance must be boolean"):
                    lifecycle_environment(Path("absent.scene"), Path("unused"), trace=True, shader_provenance=value)
                with self.assertRaisesRegex(ValueError, "shader_provenance must be boolean"):
                    lifecycle.probe("absent.exe", "absent.scene", "unused", trace=True, shader_provenance=value)
                environment.assert_not_called()
                resolve.assert_not_called()
                run.assert_not_called()

    def test_cli_shader_provenance_reaches_same_probe_and_defaults_off(self):
        for flags, expected in (([], False), (["--trace", "--shader-provenance"], True)):
            argv = ["editor_lifecycle_probe.py", "--exe", "application.exe", "--scene", "scene.jbundle", "--out", "unused", *flags]
            with patch.object(sys, "argv", argv), patch.object(lifecycle, "probe", return_value="result.json") as run, \
                 patch.object(sys, "stderr", io.StringIO()), patch("builtins.print"):
                try:
                    lifecycle.main()
                except SystemExit as error:
                    self.fail("supported shader-provenance command rejected: " + str(error))
            self.assertIs(run.call_args.kwargs.get("shader_provenance", False), expected)
            self.assertEqual(run.call_count, 1)

    def test_cli_shader_provenance_without_trace_refuses_before_probe(self):
        argv = ["editor_lifecycle_probe.py", "--exe", "absent.exe", "--scene", "absent.scene", "--out", "unused", "--shader-provenance"]
        stderr = io.StringIO()
        with patch.object(sys, "argv", argv), patch.object(sys, "stderr", stderr), patch.object(lifecycle, "probe") as run:
            with self.assertRaises(SystemExit) as error:
                lifecycle.main()
        self.assertEqual(error.exception.code, 2)
        self.assertIn("--shader-provenance requires --trace", stderr.getvalue())
        run.assert_not_called()

    def test_cli_shader_provenance_does_not_accept_string_boolean(self):
        argv = ["editor_lifecycle_probe.py", "--exe", "absent.exe", "--scene", "absent.scene", "--out", "unused", "--trace", "--shader-provenance=false"]
        with patch.object(sys, "argv", argv), patch.object(sys, "stderr", io.StringIO()), patch.object(lifecycle, "probe") as run:
            with self.assertRaises(SystemExit) as error:
                lifecycle.main()
        self.assertEqual(error.exception.code, 2)
        run.assert_not_called()

    def test_shader_provenance_both_host_routes_use_one_real_canonical_trace_reader(self):
        for enabled in (False, True):
            with self.subTest(terminal_ack=enabled):
                report, observed = self._terminal_probe(enabled=enabled, trace=True, shader_provenance=True)
                self.assertTrue(report["shader_provenance_requested"])
                self.assertEqual(observed["environment"].get("JCE_RENDER_RESOURCE_DIAG"), "1")
                analyzed = report["terminal_ack"]["trace_report"] if enabled else report["trace_report"]
                self.assertIn("shader_provenance", analyzed)
                self.assertEqual(analyzed["shader_provenance"]["complete_create_link_creator_release_count"], 1)
                self.assertEqual(analyzed["shader_provenance"]["consumed_create_receipt_count"], 2)
                self.assertEqual(observed["trace_analysis_calls"], 1)
                self.assertIs(observed["trace_analysis_kwargs"]["shader_provenance"], True)
                self.assertNotIn(observed["trace_path"], observed["hashed_paths"])
                self.assertEqual(observed["frame_reader_calls"], 1)
                if enabled:
                    self.assertEqual(observed["observer_kwargs"]["trace_analysis_options"], {"shader_provenance": True})
                else:
                    self.assertNotIn("terminal_ack", report)
                    self.assertNotIn("terminal_ack_requested", report)
                    self.assertEqual(report["artifacts"]["lifecycle-trace.json"], analyzed["sha256"])

    def test_filesystem_open_failures_environment_only_adds_explicit_post_filter_flag(self):
        root, scene = Path("build/fs-control"), Path("scene.jbundle")
        with patch.dict("os.environ", {"JCE_FS_OPEN_FAILURE_DETAIL": "1", "JCE_OTHER": "inherited"}):
            default = lifecycle_environment(scene, root)
            trace = lifecycle_environment(scene, root, trace=True)
            requested = lifecycle_environment(scene, root, trace=True, filesystem_open_failures=True)
        self.assertNotIn("JCE_FS_OPEN_FAILURE_DETAIL", default)
        self.assertNotIn("JCE_FS_OPEN_FAILURE_DETAIL", trace)
        self.assertNotIn("JCE_OTHER", requested)
        self.assertEqual(requested.pop("JCE_FS_OPEN_FAILURE_DETAIL"), "1")
        self.assertEqual(requested, trace)

    def test_filesystem_open_failures_requires_trace_before_environment_or_io(self):
        with patch.object(lifecycle, "probe_environment") as environment:
            with self.assertRaisesRegex(ValueError, "--filesystem-open-failures requires --trace"):
                lifecycle_environment(Path("absent.scene"), Path("unused"), filesystem_open_failures=True)
        environment.assert_not_called()
        with patch.object(Path, "resolve") as resolve, patch.object(lifecycle, "run_process") as run:
            with self.assertRaisesRegex(ValueError, "--filesystem-open-failures requires --trace"):
                lifecycle.probe("absent.exe", "absent.scene", "unused", filesystem_open_failures=True)
        resolve.assert_not_called()
        run.assert_not_called()

    def test_filesystem_open_failures_strict_bool_before_environment_or_io(self):
        for value in (None, 0, 1, "true", []):
            with self.subTest(value=value), patch.object(lifecycle, "probe_environment") as environment, \
                 patch.object(Path, "resolve") as resolve, patch.object(lifecycle, "run_process") as run:
                with self.assertRaisesRegex(ValueError, "filesystem_open_failures must be boolean"):
                    lifecycle_environment(Path("absent.scene"), Path("unused"), trace=True, filesystem_open_failures=value)
                with self.assertRaisesRegex(ValueError, "filesystem_open_failures must be boolean"):
                    lifecycle.probe("absent.exe", "absent.scene", "unused", trace=True, filesystem_open_failures=value)
                environment.assert_not_called()
                resolve.assert_not_called()
                run.assert_not_called()

    def test_cli_filesystem_open_failures_reaches_same_probe_and_defaults_off(self):
        for flags, expected in (([], False), (["--trace", "--filesystem-open-failures"], True)):
            argv = ["editor_lifecycle_probe.py", "--exe", "application.exe", "--scene", "scene.jbundle", "--out", "unused", *flags]
            with patch.object(sys, "argv", argv), patch.object(lifecycle, "probe", return_value="result.json") as run, \
                 patch.object(sys, "stderr", io.StringIO()), patch("builtins.print"):
                try:
                    lifecycle.main()
                except SystemExit as error:
                    self.fail("supported filesystem-open-failures command rejected: " + str(error))
            self.assertIs(run.call_args.kwargs.get("filesystem_open_failures", False), expected)
            self.assertEqual(run.call_count, 1)

    def test_cli_filesystem_open_failures_without_trace_refuses_before_probe(self):
        argv = ["editor_lifecycle_probe.py", "--exe", "absent.exe", "--scene", "absent.scene", "--out", "unused", "--filesystem-open-failures"]
        stderr = io.StringIO()
        with patch.object(sys, "argv", argv), patch.object(sys, "stderr", stderr), patch.object(lifecycle, "probe") as run:
            with self.assertRaises(SystemExit) as error:
                lifecycle.main()
        self.assertEqual(error.exception.code, 2)
        self.assertIn("--filesystem-open-failures requires --trace", stderr.getvalue())
        run.assert_not_called()

    def test_cli_filesystem_open_failures_does_not_accept_string_boolean(self):
        argv = ["editor_lifecycle_probe.py", "--exe", "absent.exe", "--scene", "absent.scene", "--out", "unused", "--trace", "--filesystem-open-failures=false"]
        with patch.object(sys, "argv", argv), patch.object(sys, "stderr", io.StringIO()), patch.object(lifecycle, "probe") as run:
            with self.assertRaises(SystemExit) as error:
                lifecycle.main()
        self.assertEqual(error.exception.code, 2)
        run.assert_not_called()

    def test_filesystem_open_failures_both_routes_and_shader_combination_use_one_reader(self):
        for enabled in (False, True):
            for shader in (False, True):
                with self.subTest(terminal_ack=enabled, shader=shader):
                    report, observed = self._terminal_probe(enabled=enabled, trace=True,
                        shader_provenance=shader, filesystem_open_failures=True)
                    self.assertTrue(report["filesystem_open_failures_requested"])
                    self.assertEqual(observed["environment"].get("JCE_FS_OPEN_FAILURE_DETAIL"), "1")
                    if not enabled:
                        self.assertIn("trace_report", report)
                    analyzed = report["terminal_ack"]["trace_report"] if enabled else report["trace_report"]
                    self.assertIn("filesystem_read_details", analyzed)
                    self.assertEqual(analyzed["filesystem_read_details"]["complete_packets_count"], 1)
                    self.assertEqual(observed["trace_analysis_calls"], 1)
                    expected = {"filesystem_read_details": True}
                    if shader:
                        expected["shader_provenance"] = True
                        self.assertIn("shader_provenance", analyzed)
                    for key, value in expected.items():
                        self.assertIs(observed["trace_analysis_kwargs"][key], value)
                    if enabled:
                        self.assertEqual(observed["observer_kwargs"]["trace_analysis_options"], expected)
                    self.assertNotIn(observed["trace_path"], observed["hashed_paths"])
                    self.assertEqual(observed["frame_reader_calls"], 1)
                    self.assertEqual(report["artifacts"]["lifecycle-trace.json"], analyzed["sha256"])

    def test_terminal_ack_default_off_preserves_shape_and_filters_inherited_token(self):
        report, observed = self._terminal_probe(enabled=False)
        self.assertNotIn("terminal_ack", report)
        self.assertNotIn("terminal_ack_requested", report)
        self.assertNotIn("shader_provenance_requested", report)
        self.assertNotIn("filesystem_open_failures_requested", report)
        self.assertNotIn("trace_report", report)
        self.assertEqual(observed["trace_analysis_calls"], 0)
        self.assertNotIn("JCE_TERMINAL_ACK_TOKEN", observed["environment"])
        self.assertEqual(observed["observer_calls"], 0)
        self.assertEqual(observed["frame_reader_calls"], 1)
        self.assertEqual(report["status"], "PLAY_STOP_AUTHORED_SCENE_RESTORATION_OBSERVED")

    def test_terminal_ack_opt_in_uses_fresh_post_filter_token_and_owned_stderr(self):
        report, observed = self._terminal_probe()
        self.assertTrue(report["terminal_ack_requested"])
        self.assertEqual(observed["environment"]["JCE_TERMINAL_ACK_TOKEN"], TOKEN)
        self.assertEqual(report["terminal_ack"]["status"], "HOST_TERMINAL_ACK_OBSERVED")
        self.assertEqual(report["terminal_ack"]["process_identity"]["pid"], 123)
        self.assertEqual(observed["observer_calls"], 1)
        self.assertEqual(observed["frame_reader_calls"], 1)
        self.assertNotIn(observed["stderr"], observed["hashed_paths"])
        self.assertEqual(report["artifacts"]["process/stderr.log"], report["terminal_ack"]["stderr"]["sha256"])
        self.assertTrue(report["scene_control"]["restored"])

    def test_terminal_ack_requires_actual_owned_packet_despite_successful_exit(self):
        for mode in ("missing", "bad_token", "duplicate", "foreign_stderr"):
            with self.subTest(mode=mode):
                report, observed = self._terminal_probe(mode=mode)
                self.assertEqual(report["status"], "FAIL")
                self.assertIn("error", report)
                self.assertEqual(observed["observer_calls"], 1)
                self.assertEqual(observed["frame_reader_calls"], 0)

    def test_terminal_ack_trace_uses_real_complete_reader_and_reuses_hash(self):
        report, observed = self._terminal_probe(trace=True)
        ack = report["terminal_ack"]
        self.assertEqual(ack["trace_binding"], "validated")
        self.assertEqual(ack["trace"]["cleanup_scope"], "10")
        self.assertEqual(ack["trace_report"]["status"], "REQUIRED_CPU_LIFECYCLE_SCOPES_OBSERVED")
        self.assertEqual(report["artifacts"]["lifecycle-trace.json"], ack["trace"]["sha256"])
        self.assertNotIn(observed["trace_path"], observed["hashed_paths"])
        self.assertEqual(observed["frame_reader_calls"], 1)

    def test_terminal_ack_trace_refuses_unavailable_or_wrong_session(self):
        for mode in ("trace_unavailable", "trace_mismatch"):
            with self.subTest(mode=mode):
                report, _ = self._terminal_probe(trace=True, mode=mode)
                self.assertEqual(report["status"], "FAIL")

    def test_terminal_ack_resource_trace_uses_existing_required_observer(self):
        report, observed = self._terminal_probe(trace=True, resource_trace=True, mode="no_resources")
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(observed["environment"]["JCE_RESOURCE_TRACE"], "1")
        self.assertEqual(observed["observer_kwargs"]["trace_analysis_options"], {"resource_observations": True})
        self.assertIn("resource samples", report["error"])

    def test_terminal_ack_non_boolean_refuses_before_any_io_or_launch(self):
        for value in (None, 0, 1, "true", []):
            with self.subTest(value=value), patch.object(Path, "resolve") as resolve, \
                 patch.object(lifecycle, "run_process") as run:
                with self.assertRaisesRegex(ValueError, "require_terminal_ack must be boolean"):
                    lifecycle.probe("absent.exe", "absent.scene", "unused", require_terminal_ack=value)
                resolve.assert_not_called()
                run.assert_not_called()

    def test_cli_terminal_ack_opt_in_reaches_probe_and_default_stays_false(self):
        for flag, expected in (([], False), (["--require-terminal-ack"], True)):
            argv = ["editor_lifecycle_probe.py", "--exe", "application.exe", "--scene", "scene.json", "--out", "unused", *flag]
            with patch.object(sys, "argv", argv), patch.object(lifecycle, "probe", return_value="result.json") as run, patch("builtins.print"):
                lifecycle.main()
            self.assertIs(run.call_args.kwargs["require_terminal_ack"], expected)

    def test_cli_terminal_ack_does_not_accept_string_boolean_value(self):
        argv = ["editor_lifecycle_probe.py", "--exe", "absent.exe", "--scene", "absent.scene", "--out", "unused", "--require-terminal-ack=false"]
        with patch.object(sys, "argv", argv), patch.object(sys, "stderr", io.StringIO()), patch.object(lifecycle, "probe") as run:
            with self.assertRaises(SystemExit) as error:
                lifecycle.main()
        self.assertEqual(error.exception.code, 2)
        run.assert_not_called()

if __name__ == "__main__":
    unittest.main()
