"""Execute the production renderer shader/uniform teardown with ownership spies."""
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]

class RendererCleanupTests(unittest.TestCase):
    def test_sdf_and_invalid_handles(self):
        spec = importlib.util.spec_from_file_location("renderer_cleanup_driver", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        source = (ROOT / "engine/src/renderer/jce_renderer.c").read_text(encoding="utf-8")
        start = source.index("    s_dbg_text_enabled = false;", source.index("void jce_renderer_destroy("))
        end = source.index("    /* Subsystems and path-keyed", start)
        cleanup = source[start:end]
        required = {"program_text_sdf", "u_sdf_params", "u_sdf_outline", "u_sdf_shadow_offset", "u_sdf_shadow_color"}
        fields = sorted(set(re.findall(r"r->(\w+)", cleanup)) | required)
        harness = "\n".join([
            "#include <assert.h>", "#include <stdint.h>", "#include <stdbool.h>",
            "#include <stdio.h>", "#include <string.h>",
            "typedef struct {uint16_t idx;} JceShaderHandle;",
            "typedef JceShaderHandle bgfx_uniform_handle_t;",
            "typedef struct {" + "".join("JceShaderHandle " + f + ";" for f in fields) + "} JceRenderer;",
            "static bool s_dbg_text_enabled;static unsigned programs,uniforms;",
            "static void jce_shader_program_destroy(JceShaderHandle p){assert(p.idx==42);programs++;}",
            "static void bgfx_destroy_uniform(bgfx_uniform_handle_t u){assert(u.idx>=43&&u.idx<=46);uniforms++;}",
            "static void cleanup(JceRenderer *r){" + cleanup + "}",
            "int main(void){JceRenderer r;memset(&r,255,sizeof r);r.program_text_sdf.idx=42;",
            "r.u_sdf_params.idx=43;r.u_sdf_outline.idx=44;r.u_sdf_shadow_offset.idx=45;r.u_sdf_shadow_color.idx=46;",
            "cleanup(&r);assert(programs==1&&uniforms==4);memset(&r,255,sizeof r);",
            "cleanup(&r);assert(programs==1&&uniforms==4);puts(\"Renderer owned cleanup PASS\");return 0;}"
        ])
        base = ROOT / "build/renderer-cleanup-tests"
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            directory = Path(temporary)
            file = directory / "probe.c"
            file.write_text(harness, encoding="utf-8")
            if driver.HOST == "windows":
                command = ["cl", "/nologo", "/TC", "/std:c11", "/W3", str(file), "/Fe:probe.exe", "/Fo:probe.obj"]
                env = driver.msvc_env(driver.resolve_target(None))
                executable = directory / "probe.exe"
            else:
                compiler = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
                if not compiler:
                    self.skipTest("no C compiler available for production teardown probe")
                executable = directory / "probe"
                command = [compiler, "-std=c99", str(file), "-o", str(executable)]
                env = None
            result = subprocess.run(command, cwd=directory, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Renderer owned cleanup PASS", result.stdout)

if __name__ == "__main__":
    unittest.main()
