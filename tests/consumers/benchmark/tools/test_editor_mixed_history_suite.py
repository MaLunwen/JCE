import unittest
from editor_mixed_history_suite import validate, validate_process, validate_budget_refusal, validate_full_first, validate_play_stop, validate_duplicate_state
from pathlib import Path
from unittest.mock import patch
import tempfile
from PIL import Image
class MixedHistoryTests(unittest.TestCase):
    def test_actual_tokens_rebind_while_numeric_handles_change(self):
        text = "history-scene-restore: reason=undo focused=200 token=7 name=Renamed parent_name=ROOT population=10000\nhistory-entity-restore: token=7 source_id=456 current_id=200 reason=undo name=Node0 parent=0\nhistory-entity-restore: token=7 source_id=200 current_id=200 reason=redo name=Renamed parent=0\nhistory-scene-restore: reason=redo focused=300 token=7 name=Renamed parent_name=Node1 population=10000"
        validate(text, "reparent", 10000)
        for broken in (text.replace("token=7 name=Renamed parent_name=Node1", "token=8 name=Renamed parent_name=Node1"), text.replace("reason=undo name=Node0", "reason=undo name=Other"), text.replace("parent_name=Node1", "parent_name=ROOT"), text+"\n"+text.split("\n")[1]):
            with self.assertRaises(ValueError): validate(broken, "reparent", 10000)
    def test_process_and_complete_capture_guards(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stdout = root / "stdout.txt"; stdout.write_text("")
            capture = root / "editor.png"
            process = {"process": {"returncode": 0, "timed_out": False, "stdout_path": str(stdout)}, "backend_actual": "d3d11"}
            with patch("editor_mixed_history_suite.assert_backend", return_value="d3d11"):
                with self.assertRaises(ValueError): validate_process(process, "", "d3d11", capture)
                Image.new("RGB", (8, 8)).save(capture)
                self.assertEqual(validate_process(process, "", "d3d11", capture), "d3d11")
                for key, value in (("returncode", 1), ("timed_out", True)):
                    process["process"][key] = value
                    with self.assertRaises(ValueError): validate_process(process, "", "d3d11", capture)
                    process["process"][key] = 0 if key == "returncode" else False
                process["backend_actual"] = "vulkan"
                with self.assertRaises(ValueError): validate_process(process, "", "d3d11", capture)
                process["backend_actual"] = "d3d11"
                capture.write_bytes(capture.read_bytes()[:40])
                with self.assertRaises(Exception): validate_process(process, "", "d3d11", capture)
    def test_budget_refusal_requires_actual_unchanged_state_and_exact_errors(self):
        row = "history-budget-refusal: unchanged=1 selected_unchanged=1 focused_unchanged=1 population=10000 before_hash=123 after_hash=123"
        error = "[MAIN] ERROR - history identity preparation refused before scene clear"
        text = "\n".join([row, error, row, error, "editor-command: operation=reparent entity=456 frame=100 correct=1 parent=457 name=Renamed", "inspector-content-observation: frame=190 entity=456 valid=1 display_name=Renamed"])
        validate_budget_refusal(text, 10000)
        for bad in (text.replace("after_hash=123", "after_hash=124"), text.replace("unchanged=1", "unchanged=0"), text+"\n"+row, text+"\n[MAIN] ERROR - undo: rollback failed after restore error", text.replace("display_name=Renamed", "display_name=Node0"), text.replace("valid=1", "valid=0"), text.replace("frame=190 entity=456", "frame=139 entity=456"), text.replace("frame=190 entity=456", "frame=190 entity=457")):
            with self.assertRaises(ValueError): validate_budget_refusal(bad, 10000)
    def test_full_first_inverse_reintroduces_new_scoped_identity(self):
        text = "\n".join(["history-entity-restore: token=7 source_id=456 current_id=456 reason=undo name=Node0 parent=457", "history-scene-restore: reason=undo focused=200 token=0 name=Node0 parent_name=ROOT population=10000", "history-scene-restore: reason=redo focused=300 token=7 name=Node0 parent_name=Node1 population=10000", "history-entity-restore: token=7 source_id=456 current_id=300 reason=redo name=Renamed parent=301", "inspector-complete: generation=6 entity=300 frame=190 status=drawn latency_ms=1.0 display_name=Renamed"])
        validate_full_first(text, 10000)
        for broken in (text.replace("focused=300 token=7", "focused=300 token=8"), text.replace("focused=200 token=0", "focused=200 token=7"), text.replace("parent_name=Node1", "parent_name=ROOT")):
            with self.assertRaises(ValueError): validate_full_first(broken, 10000)
    def test_play_stop_requires_actual_lifecycle_and_postrestore_identity(self):
        text = "\n".join(["autoplay: entered Play at frame 20", "play mode stopped", "history-scene-restore: reason=play-stop-restore focused=200 token=0 name=Node0 parent_name=ROOT population=10000", "history-entity-restore: token=7 source_id=200 current_id=200 reason=undo name=Node0 parent=0", "history-entity-restore: token=7 source_id=200 current_id=200 reason=redo name=Renamed parent=0", "inspector-content-observation: frame=190 entity=200 valid=1 display_name=Renamed"])
        validate_play_stop(text, 10000)
        for bad in (text.replace("play mode stopped", ""), text.replace("reason=redo name=Renamed", "reason=redo name=Node0"), text.replace("focused=200", "focused=201"), text.replace("valid=1", "valid=0")):
            with self.assertRaises(ValueError): validate_play_stop(bad, 10000)
    def test_duplicate_state_uses_actual_parent_and_transform(self):
        import copy
        document = {"entities": [{"id": 1, "name": "Parent"}, {"id": 2, "name": "Duplicate", "parentId": 1, "components": [{"type": "Transform", "posX": 17}]}, {"id": 3, "name": "Duplicate", "components": [{"type": "Transform", "posX": 29}]}]}
        validate_duplicate_state(document, copy.deepcopy(document))
        for key, value in (("parentId", 0), ("name", "Other")):
            bad = copy.deepcopy(document); bad["entities"][1][key] = value
            with self.assertRaises(ValueError): validate_duplicate_state(document, bad)
        bad = copy.deepcopy(document); bad["entities"][1]["components"][0]["posX"] = 29
        with self.assertRaises(ValueError): validate_duplicate_state(document, bad)
if __name__ == "__main__": unittest.main()
