"""Shared vertex consumers require the same complete bgfx IO interface."""
import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.lint import check_shader_varying_pairs as gate


class SharedShaderInterface(unittest.TestCase):
    def test_shared_vertex_mismatch_fails_then_matching_declaration_passes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            shaders = root / "engine/shaders/pbr"
            shaders.mkdir(parents=True)
            source = root / "engine/src"
            source.mkdir()
            (shaders / "vs_skin.sc").write_text(
                "$output v_normal, v_curClip, v_prevClip\n", encoding="utf-8")
            (shaders / "fs_skin.sc").write_text(
                "$input v_normal, v_curClip, v_prevClip\n", encoding="utf-8")
            normal = shaders / "fs_normal.sc"
            normal.write_text("$input v_normal\n", encoding="utf-8")
            (source / "normal.c").write_text(
                'shader_load_program_named(pak, "skin", "normal");\n',
                encoding="utf-8")
            with patch.object(gate, "ROOT", root), patch.object(
                    gate, "SHADERS", root / "engine/shaders"):
                diagnostics = io.StringIO()
                with contextlib.redirect_stderr(diagnostics):
                    self.assertEqual(gate.main(), 1)
                self.assertIn("fs_normal.sc does not declare", diagnostics.getvalue())
                normal.write_text(
                    "$input v_normal, v_curClip, v_prevClip\n", encoding="utf-8")
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(gate.main(), 0)


if __name__ == "__main__":
    unittest.main()
