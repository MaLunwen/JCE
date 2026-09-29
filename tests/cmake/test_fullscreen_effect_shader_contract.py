import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
VERTEX_SHADER = ROOT / "engine" / "shaders" / "postfx" / "vs_jce_fullscreen.sc"
SHADER_ABI = ROOT / "engine" / "shaders" / "include" / "jce_fullscreen_effect.sh"


class FullscreenEffectShaderContractTests(unittest.TestCase):
    def test_screen_uv_is_backend_independent(self):
        source = VERTEX_SHADER.read_text(encoding="utf-8")

        self.assertIn("v_texcoord0 = a_texcoord0;", source)
        self.assertNotIn("BGFX_SHADER_LANGUAGE", source)
        self.assertNotRegex(source, r"v_texcoord0\.y\s*=\s*1\.0\s*-\s*v_texcoord0\.y")

    def test_render_target_sampling_has_an_explicit_origin_helper(self):
        source = SHADER_ABI.read_text(encoding="utf-8")

        self.assertIn("jce_fullscreen_render_target_uv", source)
        self.assertIn("BGFX_SHADER_LANGUAGE_GLSL", source)


if __name__ == "__main__":
    unittest.main()
