"""Lifecycle evidence must reject missing phases, broken restoration and incomplete runs."""
import copy
import math
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from editor_lifecycle_probe import canonical_snapshot, lifecycle_environment, validate_errors, validate_phases, validate_scenes
from editor_portability_probe import validate_process


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
    def test_explicit_environment_retains_stop_playing_tick_semantics(self):
        root = Path("build/lifecycle-control").resolve()
        env = lifecycle_environment(Path("scene.jbundle").resolve(),root)
        self.assertEqual(env["JCE_MAX_FRAMES"],"240")
        self.assertEqual(env["JCE_DBG_SCENE_OBSERVE_AT"],"10,80,220")
        self.assertEqual(env["JCE_DBG_PLAY_STOP_AT"],"160")
        self.assertEqual(env["JCE_WINDOW_HIDDEN"],"1")
        self.assertEqual(env["JCE_FRAME_DT_FIXED"],"0.016666")
if __name__ == "__main__":
    unittest.main()
