import unittest
from editor_hierarchy_suite import validate_observations, scene, operation_phases, validate_rename_history, validate_display_name, validate_capture, hierarchy_cache_environment, validate_cache_observations, compare_cache_controls

class HierarchyTests(unittest.TestCase):
    def test_real_count_and_consumption(self):
        text = "hierarchy-population: loaded=10000 roots=10000 rows=10000 capacity=16384 frame=50\ninspector-request: generation=1 entity=2 frame=60\ninspector-complete: generation=1 entity=2 frame=61 status=drawn latency_ms=0.5"
        self.assertEqual(validate_observations(text, 10000)["inspector"][0]["frame"], 61)
        for broken in (text.replace("rows=10000", "rows=3276"), text.replace("entity=2 frame=61", "entity=3 frame=61"), text.replace("status=drawn", "status=invalidated"), text + "\ninspector-complete: generation=1 entity=2 frame=61 status=drawn latency_ms=0.5", text + "\nhierarchy-population: loaded=10000 roots=10000 rows=10000 capacity=16384 frame=50"):
            with self.assertRaises(ValueError): validate_observations(broken, 10000)
    def test_actual_phases_reject_missing_and_quantized_zero(self):
        names = ("ed_hier_roots", "ed_hier_flat", "ed_select_entity", "ed_rename_entity",
                 "ed_reparent_entity", "ed_hierarchy_reveal_scroll", "ed_inspector_response", "ed_inspector_draw")
        samples = {"frame_indices": [0, 1, 2], "phase_ms": {name: [float("nan"), 0, .5] for name in names}}
        result = operation_phases(samples)
        self.assertEqual(result["ed_select_entity"]["positive_observations"], [{"frame": 2, "ms": .5}])
        self.assertEqual(result["ed_select_entity"]["absent_count"], 1)
        self.assertEqual(result["ed_select_entity"]["zero_count"], 1)
        samples["phase_ms"]["ed_select_entity"] = [0, 0, 0]
        unresolved = operation_phases(samples)["ed_select_entity"]
        self.assertEqual(unresolved["status"], "UNAVAILABLE_BELOW_CSV_RESOLUTION")
        self.assertEqual(unresolved["zero_count"], 3)
        self.assertEqual(unresolved["csv_resolution_ms"], 0.0001)
        self.assertIsNone(unresolved["median_ms"])
        samples["phase_ms"]["ed_select_entity"] = [float("nan")] * 3
        with self.assertRaises(ValueError): operation_phases(samples)
        samples["phase_ms"]["ed_select_entity"] = [0, 0]
        with self.assertRaises(ValueError): operation_phases(samples)
        del samples["phase_ms"]["ed_select_entity"]
        with self.assertRaises(ValueError): operation_phases(samples)

    def test_wide_fixture_public_parent_ids_and_expanded_population(self):
        nodes = scene(100000, "wide")["entities"]
        self.assertNotIn("parentId", nodes[0])
        self.assertTrue(all(node["parentId"] == 1 for node in nodes[1:]))
        self.assertEqual(len(nodes), 100000)
        text = "hierarchy-population: loaded=100000 roots=1 rows=100000 capacity=131072 frame=50\ninspector-request: generation=1 entity=2 frame=30\ninspector-complete: generation=1 entity=2 frame=31 status=drawn latency_ms=0.5"
        self.assertEqual(validate_observations(text, 100000, "wide")["roots"], 1)
        for broken in (text.replace("rows=100000", "rows=32768"), text.replace("roots=1 ", "roots=2 ")):
            with self.assertRaises(ValueError): validate_observations(broken, 100000, "wide")
        with self.assertRaises(ValueError): validate_observations(text, 100000)

    def test_actual_rename_history_markers_require_stable_identity(self):
        text = "rename-history: frame=91 phase=undo entity=3 correct=1 name=Node2\nrename-history: frame=111 phase=redo entity=3 correct=1 name=Renamed"
        validate_rename_history(text)
        for broken in (text.replace("entity=3 correct=1 name=Renamed", "entity=4 correct=1 name=Renamed"), text.replace("correct=1", "correct=0"), text.split("\n")[0], text + "\n" + text.split("\n")[1]):
            with self.assertRaises(ValueError): validate_rename_history(broken)

    def test_display_consumption_rejects_stale_or_pre_operation_buffer(self):
        text = "inspector-complete: generation=2 entity=3 frame=80 status=drawn latency_ms=0.5 display_name=Renamed"
        validate_display_name(text, "Renamed", 80)
        for broken in (text.replace("display_name=Renamed", "display_name=Node0"), text.replace("frame=80", "frame=60"), text.replace("status=drawn", "status=invalidated")):
            with self.assertRaises(ValueError): validate_display_name(broken, "Renamed", 80)

    def test_fixture_real_nodes(self):
        value = scene(10000)["entities"]
        self.assertEqual(len(value), 10000)
        self.assertEqual(len({x["id"] for x in value}), 10000)

    def test_capture_requires_complete_actual_png(self):
        from pathlib import Path
        from PIL import Image
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"editor.png"
            with self.assertRaises(ValueError): validate_capture(path)
            Image.new("RGB", (16, 8)).save(path)
            self.assertEqual(validate_capture(path), (16, 8))
            path.write_bytes(path.read_bytes()[:40])
            with self.assertRaises(Exception): validate_capture(path)

    def test_negative_or_infinite_phase_is_not_absent_data(self):
        names = ("ed_hier_roots", "ed_hier_flat", "ed_select_entity", "ed_rename_entity",
                 "ed_reparent_entity", "ed_hierarchy_reveal_scroll", "ed_inspector_response", "ed_inspector_draw")
        for value in (-1, float("inf"), -float("inf")):
            samples = {"frame_indices": [0, 1], "phase_ms": {name: [.5, .5] for name in names}}
            samples["phase_ms"]["ed_rename_entity"][1] = value
            with self.assertRaises(ValueError): operation_phases(samples)
    def test_explicit_cache_control_and_actual_trace_guards(self):
        self.assertEqual(hierarchy_cache_environment(False), {})
        self.assertEqual(hierarchy_cache_environment(True), {"JCE_DISABLE_HIERARCHY_CACHE":"1"})
        text="hierarchy-flat-cache: frame=1 eligible=1 reused=0 rows=10000 roots=10000\nhierarchy-flat-cache: frame=2 eligible=1 reused=1 rows=10000 roots=10000"
        self.assertEqual(validate_cache_observations(text,False)["reuse_count"],1)
        with self.assertRaises(ValueError):validate_cache_observations(text,True)
        disabled=text.replace("eligible=1 reused=1","eligible=0 reused=0").replace("eligible=1 reused=0","eligible=0 reused=0")
        self.assertEqual(validate_cache_observations(disabled,True)["rebuild_count"],2)
        for bad in ("",text+"\n"+text.splitlines()[0],text.replace("eligible=1 reused=1","eligible=0 reused=1")):
            with self.assertRaises(ValueError):validate_cache_observations(bad,False)
    def test_cache_ab_rehashes_and_rejects_changed_actual_rows(self):
        from pathlib import Path
        import tempfile, json, copy
        from editor_hierarchy_suite import sha256_file
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); raw=root/"editor.log";raw.write_text("actual")
            result=dict(executable_sha256="exe",scene_sha256="scene",backend_actual="d3d11",raw_sha256={"editor.log":sha256_file(raw)},
                hierarchy_cache=dict(disabled_requested=False,observations=[dict(frame=1,rows=10,roots=10)]),
                observations=dict(loaded=10,roots=10,rows=10,capacity=16,inspector=[dict(generation=1,entity=2,frame=60)]))
            previous=root/"result.json";previous.write_text(json.dumps(result))
            current=copy.deepcopy(result);current["hierarchy_cache"]["disabled_requested"]=True
            compare_cache_controls(current,previous)
            changed=copy.deepcopy(current);changed["hierarchy_cache"]["observations"][0]["rows"]=9
            with self.assertRaises(ValueError):compare_cache_controls(changed,previous)
            changed=copy.deepcopy(current);changed["observations"]["inspector"][0]["entity"]=3
            with self.assertRaises(ValueError):compare_cache_controls(changed,previous)
            raw.write_text("changed")
            with self.assertRaises(ValueError):compare_cache_controls(current,previous)
if __name__ == "__main__": unittest.main()
