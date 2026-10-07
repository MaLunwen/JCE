"""Guard scene fast-path admission against the authoritative shader axes.

These compiler-free source/manifest contracts do not replace GPU pixel controls.
"""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
DRAW = ROOT / "engine/src/middleware/scene/jce_sr_draw.c"


def condition(source, variable):
    match = re.search(r"const bool " + variable + r"\s*=([^;]+);", source)
    if not match:
        raise AssertionError("missing admission condition: " + variable)
    return match.group(1)


def admits(expression, forwardplus, **options):
    # Restrict parsing to the existing pure boolean policy expression. Actual
    # scene/material/mesh fallback fidelity needs the native render regression.
    expression = re.sub(r"jce_renderer_get_program_\w+\(sr->renderer\)"
                        r"\.idx != UINT16_MAX", "program_valid", expression)
    expression = expression.replace("cfg->view_mode == JCE_SCENE_VIEW_SHADED", "shaded")
    expression = expression.replace("cfg->draw_opaque", "opaque")
    expression = expression.replace("sr->fp_active_frame", "forwardplus")
    expression = expression.replace("&&", " and ").replace("!", " not ")
    values = dict(pi_enabled=True, ti_enabled=True, pg_active=False,
                  shaded=True, opaque=True, program_valid=True,
                  forwardplus=forwardplus)
    values.update(options)
    expression = " ".join(expression.split())
    return bool(eval(expression, {"__builtins__": {}}, values))


class ForwardPlusAdmissionTests(unittest.TestCase):
    def test_unsupported_tint_and_array_families_keep_ordinary_pbr_fallback(self):
        source = DRAW.read_text()
        manifest = json.loads((ROOT / "contracts/shader-keywords.json").read_text())
        axis = next(axis for axis in manifest["axes"] if axis["name"] == "fwdplus")
        for variable, vertex in (("prim_inst_on", "pbr_inst_tint"),
                                 ("tex_inst_on", "pbr_inst_tex_array")):
            family = next(row["fs_family"] for row in manifest["vertex_variants"]
                          if row["name"] == vertex)
            self.assertNotIn(family, axis["families"])
            expression = condition(source, variable)
            self.assertTrue(admits(expression, False), variable)
            self.assertFalse(admits(expression, True), variable)
            self.assertFalse(admits(expression, False, program_valid=False))
            self.assertFalse(admits(expression, False, opaque=False))
            self.assertFalse(admits(expression, False, pg_active=True))

    def test_negative_control_without_frame_admission_reproduces_wrong_path(self):
        source = DRAW.read_text()
        for variable in ("prim_inst_on", "tex_inst_on"):
            expression = condition(source, variable)
            mutant = expression.replace("&& !sr->fp_active_frame", "")
            self.assertNotEqual(mutant, expression)
            self.assertTrue(admits(mutant, True))

    def test_foliage_unsupported_fade_keeps_existing_band_and_cluster_path(self):
        source = (ROOT / "engine/src/middleware/scene/jce_sr_environment.c").read_text()
        begin = source.index("static bool sr_foliage_gpu_lod_cull_draw(")
        end = source.index("void sr_draw_foliage(", begin)
        function = source[begin:end]
        self.assertIn("bool fade_prog = !sr->fp_active_frame &&", function)
        self.assertIn("if (!fade_prog)", function)
        self.assertIn("jce_renderer_get_program_pbr_inst(sr->renderer)", function)
        self.assertIn("const float fade_w = fade_prog ? s_fade : 0.0f", function)
        self.assertIn("s_step, bands, capb_real, fade_w", function)
        self.assertIn("jce_forwardplus_bind(sr->forwardplus)", function)
        self.assertIn("jce_model_draw_foliage_lod_indirect", function)

    def test_tinted_model_batch_rejects_before_mutating_counts_or_allocating(self):
        source = DRAW.read_text()
        start = source.index("static bool sr_inst_batch_add(")
        end = source.index("/* Sort by (model, lod)", start)
        function = source[start:end]
        guard = re.search(r"if \((sr->fp_active_frame && tint)\) return false;", function)
        self.assertIsNotNone(guard)
        self.assertLess(guard.start(), function.index("JCE_REALLOC"))
        self.assertLess(guard.start(), function.index("sr->inst_batch_count++"))
        # Fallback retains the one-instance tint operation. The model's native
        # count==1 path already binds a tinted copy of each effective material.
        solo = source[source.index("#define SR_DRAW_ONE()"):]
        self.assertIn("jce_model_draw_instanced_tinted", solo)
        self.assertIn("model, &t1, 1)", solo)
        self.assertIn("jce_model_draw(mc->model", solo)


if __name__ == "__main__":
    unittest.main()
