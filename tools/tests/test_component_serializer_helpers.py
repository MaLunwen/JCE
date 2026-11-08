"""Serializer-scoped, actually called private helpers retain field coverage."""
import importlib.util
from pathlib import Path
import re
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("serializer_gate",
    ROOT / "tools/lint/check_component_serializer_roundtrip.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)
HELPER = ROOT / "engine/src/middleware/scene/jce_scene_line_colors_internal.h"
RENDER = ROOT / "engine/src/middleware/scene/jce_scene_components_render.c"
COMPONENT = "JceLineRendererComponent"


class SerializerHelperControls(unittest.TestCase):
    def findings(self, changes=None):
        original = Path.read_text

        def read(path, *args, **kwargs):
            text = original(path, *args, **kwargs)
            return changes[path](text) if changes and path in changes else text

        with mock.patch.object(Path, "read_text", read):
            return {(comp, field) for comp, field, _reason in gate.one_way_fields()}

    def omit(self, text, field):
        # Remove only the actual complete assignment. The remaining numeric
        # read still compiles, but does not restore the component field.
        pattern = r"line->" + field + r"\[i\] = \(float\)(jce_json_get_number\([^;]+);"
        changed, count = re.subn(pattern, r"(void)\1;", text)
        self.assertEqual(count, 1, "actual colour assignment changed")
        return changed

    def test_actual_called_helper_restores_both_color_arrays(self):
        findings = self.findings()
        self.assertNotIn((COMPONENT, "color_start"), findings)
        self.assertNotIn((COMPONENT, "color_end"), findings)

    def test_missing_actual_assignment_is_reported(self):
        for field in ("color_start", "color_end"):
            with self.subTest(field=field):
                findings = self.findings({HELPER: lambda text: self.omit(text, field)})
                self.assertIn((COMPONENT, field), findings)

    def test_present_but_uncalled_helper_is_not_persistence(self):
        def remove_call(text):
            needle = "    scene_line_parse_colors(&l, c, false);"
            self.assertEqual(text.count(needle), 1)
            return text.replace(needle, "    /* Deliberately omit colour restoration. */")

        findings = self.findings({RENDER: remove_call})
        self.assertIn((COMPONENT, "color_start"), findings)
        self.assertIn((COMPONENT, "color_end"), findings)

    def test_partial_defaults_cannot_hide_missing_serializer_writer(self):
        def remove_writer(text):
            start = text.index("static void ser_line_renderer(")
            end = text.index("\n}\n", start) + 3
            body, count = re.subn(
                r'^\s*cJSON_AddNumberToObject\(o, "colorEnd[RGBA]", c->color_end\[\d\]\);\n',
                "", text[start:end], flags=re.M)
            self.assertEqual(count, 4, "actual end-colour writer changed")
            return text[:start] + body + text[end:]

        self.assertIn((COMPONENT, "color_end"), self.findings({RENDER: remove_writer}))

    def test_uncalled_typed_accessor_cannot_hide_missing_read(self):
        def change(text):
            text = self.omit(text, "color_end")
            return text + "\nstatic void parse_unused_line_colors(JceLineRendererComponent *line)\n" \
                "{ line->color_end[0] = 1.0f; }\n"

        self.assertIn((COMPONENT, "color_end"), self.findings({HELPER: change}))

    def test_missing_included_helper_fails_closed(self):
        def missing(_text):
            raise FileNotFoundError("required serializer helper missing")

        with self.assertRaisesRegex(FileNotFoundError, "required serializer helper"):
            self.findings({HELPER: missing})


if __name__ == "__main__":
    unittest.main()
