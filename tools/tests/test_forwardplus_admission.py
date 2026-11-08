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


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def assert_foliage_bind_chain(test, environment, draw, special, model):
    """Check the actual source owners through each per-band submit."""
    colour = function(draw, "void sr_draw_entities(")
    arm = colour.index("jce_model_set_pre_submit_cb(sr_model_presubmit_cb, sr);")
    submit = colour.index("sr_colour_special_submit_opaque(")
    retire = colour.index("jce_model_set_pre_submit_cb(NULL, NULL);", submit)
    test.assertLess(arm, submit)
    test.assertLess(submit, retire)
    dispatcher = function(special, "static void sr_colour_special_submit_opaque(")
    test.assertIn("sr_draw_foliage(sr, scene, list, e, view_id, camera);", dispatcher)
    foliage = function(environment, "void sr_draw_foliage(")
    test.assertIn("sr_foliage_gpu_lod_cull_draw(sr, slot, mc->model", foliage)
    adapter = function(environment, "static bool sr_foliage_gpu_lod_cull_draw(")
    test.assertIn("jce_model_draw_foliage_lod_indirect(model, sr->renderer, view_id,", adapter)

    setter = function(model, "void jce_model_set_pre_submit_cb(")
    test.assertIn("s_pre_submit_cb   = cb;", setter)
    test.assertIn("s_pre_submit_user = user;", setter)
    owner = function(model, "void jce_model_draw_foliage_lod_indirect(")
    band = function(owner, "for (uint32_t b = 0; b < band_count; ++b)")
    material = band.index("jce_pbr_material_bind(mat, r, view_id);")
    callback = band.index("s_pre_submit_cb(s_pre_submit_user, view_id);")
    indirect = band.index("bgfx_submit_indirect(view_id, prog, indir, (uint16_t)b")
    test.assertLess(material, callback)
    test.assertLess(callback, indirect)
    test.assertIn("BGFX_DISCARD_ALL", band[indirect:])

    callback = function(draw, "static void sr_model_presubmit_cb(")
    test.assertIn("sr_inline_bind_pbr_global(sr, NULL, view_id, NULL, NULL,", callback)
    test.assertRegex(callback, r"if \(sr->fp_active_frame\)\s+"
                     r"jce_forwardplus_bind\(sr->fp_storage.module\);")
    test.assertLess(callback.index("sr_inline_bind_pbr_global("),
                    callback.index("jce_forwardplus_bind("))


def foliage_bind_sources():
    scene = ROOT / "engine/src/middleware/scene"
    return (
        (scene / "jce_sr_environment.c").read_text(),
        DRAW.read_text(),
        (scene / "jce_sr_colour_special_internal.h").read_text(),
        (ROOT / "engine/src/renderer/jce_model.c").read_text(),
    )


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
        self.assertIn("jce_model_draw_foliage_lod_indirect", function)
        assert_foliage_bind_chain(self, *foliage_bind_sources())

    def test_missing_shared_foliage_callback_bind_fails_same_chain_checker(self):
        environment, draw, special, model = foliage_bind_sources()
        callback = function(draw, "static void sr_model_presubmit_cb(")
        wire = "jce_forwardplus_bind(sr->fp_storage.module);"
        self.assertEqual(callback.count(wire), 1)
        mutant = callback.replace(wire, "(void)0;", 1)
        self.assertEqual(draw.count(callback), 1)
        assert_foliage_bind_chain(self, environment, draw, special, model)
        with self.assertRaisesRegex(AssertionError, "jce_forwardplus_bind"):
            assert_foliage_bind_chain(self, environment,
                draw.replace(callback, mutant, 1), special, model)

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
