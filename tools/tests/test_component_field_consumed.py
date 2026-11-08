"""The real field gate sees new consumers, never ignored source copies."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "component_field_gate", Path(__file__).parents[1] /
    "lint/check_component_field_consumed.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)

FOCUS = "JceControl39Component"
FOCUS_READER = "void consume_new(%s *p)\n{\n use(p->value);\n}\n" % FOCUS


class ComponentFieldWorkingTreeControls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        subprocess.run(["git", "init", "-q", str(self.root)],
                       check=True, capture_output=True)
        self.write(".gitignore", "**/build/\ndist/\nengine/src/ignored_reader.c\n")
        header = "".join(
            "typedef struct {\n float value;\n} JceControl%dComponent;\n" % index
            for index in range(40))
        impl = "".join("JCE_COMP_IMPL(JceControl%dComponent, control%d)\n" %
                       (index, index) for index in range(40))
        self.write("engine/include/jce/middleware/scene/jce_scene.h", header)
        self.write("engine/src/middleware/scene/jce_scene.c", impl)
        # The extractor requires a real retired registry row and ten actual
        # authoring surfaces; do not patch its fail-closed guards or readers.
        self.write("engine/src/middleware/scene/jce_scene_components_json.c",
                   'REG("Retired", key, 0, 0, 0, 0, 0, parse, NULL, 0, 0, 0)\n')
        self.old_reader = "".join(
            "void consume%d(JceControl%dComponent *p)\n{\n use(p->value);\n}\n" %
            (index, index) for index in range(39))
        self.write("engine/src/runtime/old_owner.c", self.old_reader)
        panels = "".join(
            "void author%d(JceControl%dComponent *p)\n{\n p->value = 1;\n}\n" %
            (index, index) for index in range(40))
        self.write("editor/src/panels/controls.cpp", panels)
        for index in range(9):
            self.write("editor/src/panels/empty%d.cpp" % index, "/* panel */\n")
        self.baseline = self.write("baseline.json", json.dumps({
            "fields": [], "unauthorable": [], "reasons": {}}))
        self.context = contextlib.ExitStack()
        self.addCleanup(self.context.close)
        replacements = {
            "REPO_ROOT": self.root,
            "HEADER": self.root / "engine/include/jce/middleware/scene/jce_scene.h",
            "IMPL": self.root / "engine/src/middleware/scene/jce_scene.c",
            "REGISTRY": self.root / "engine/src/middleware/scene/jce_scene_components_json.c",
            "BASELINE": self.baseline,
        }
        for name, value in replacements.items():
            self.context.enter_context(patch.object(gate, name, value))
        self.context.enter_context(patch.object(gate._lgf, "REPO_ROOT", self.root))
        self.context.enter_context(patch.object(gate.sys, "argv", ["field_gate"]))

    def write(self, relative, content):
        target = self.root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
        return target

    def run_gate(self):
        stdout, stderr = io.StringIO(), io.StringIO()
        before = self.baseline.read_bytes()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = gate.main()
        self.assertEqual(self.baseline.read_bytes(), before)
        # No fixture source is staged: the real Git index remains empty.
        self.assertEqual(gate._lgf.tracked(), [])
        return code, stdout.getvalue(), stderr.getvalue()

    def test_untracked_production_consumer_closes_gate_without_staging(self):
        relative = "engine/src/runtime/new_owner.cpp"
        self.write(relative, FOCUS_READER)
        self.assertIn(relative, gate._lgf.working_tree())
        self.assertIn(relative, gate.engine_texts())
        code, stdout, stderr = self.run_gate()
        self.assertEqual(code, 0, stderr)
        self.assertIn("all baselined", stdout)

    def test_reader_move_into_new_translation_unit_keeps_gate_clean(self):
        self.write("engine/src/runtime/old_owner.c", self.old_reader + FOCUS_READER)
        self.assertEqual(self.run_gate()[0], 0)
        self.write("engine/src/runtime/old_owner.c", self.old_reader)
        self.write("engine/src/runtime/moved_owner.c", FOCUS_READER)
        code, _, stderr = self.run_gate()
        self.assertEqual(code, 0, stderr)

    def test_ignored_copies_comments_serializers_and_other_domains_do_not_read(self):
        excluded = [
            "engine/src/runtime/build/frozen-sources/reader.cpp",
            "engine/src/ignored_reader.c",
            "dist/engine/src/runtime/reader.c",
        ]
        for relative in excluded:
            self.write(relative, FOCUS_READER)
        self.write("engine/src/middleware/scene/jce_scene_components_control.c",
                   FOCUS_READER)
        self.write("engine/src/runtime/comment_only.c", "/* %s */\n" % FOCUS_READER)
        self.write("tests/consumers/reader.c", FOCUS_READER)
        self.write("engine/src_backup/reader.c", FOCUS_READER)
        listing = gate._lgf.working_tree()
        for relative in excluded:
            self.assertNotIn(relative, listing)
        self.assertEqual(gate.unread_fields(), [(FOCUS, "value")])
        code, _, stderr = self.run_gate()
        self.assertEqual(code, 1)
        self.assertIn(FOCUS + ".value", stderr)
        self.assertIn("1 newly unread, 0 newly unauthorable", stderr)

    def test_deleted_source_in_listing_does_not_hide_live_consumption(self):
        self.write("engine/src/runtime/new_owner.c", FOCUS_READER)
        listing = gate._lgf.working_tree() + ["engine/src/runtime/deleted_owner.c"]
        with patch.object(gate._lgf, "working_tree", return_value=listing):
            code, _, stderr = self.run_gate()
        self.assertEqual(code, 0, stderr)


if __name__ == "__main__":
    unittest.main()
