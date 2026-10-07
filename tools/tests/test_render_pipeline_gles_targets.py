"""Exercise the actual GLES target policy without substituting a backend label for capability."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]

class GLESTargetPolicyTests(unittest.TestCase):
    def test_hdr_and_taa_capability_constraints(self):
        spec = importlib.util.spec_from_file_location("postfx_format_driver", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        source = (ROOT / "engine/src/renderer/jce_render_pipeline.c").read_text(encoding="utf-8")
        policy_start = source.index("    /* WebGL2 / OpenGL ES clamp")
        policy_end = source.index("    /* LOW-tier floor", policy_start)
        selector = "static void apply_policy(void) {" + source[policy_start:policy_end] + "}"
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define JCE_CAP_TEXTURE_FLOAT (1u << 3)
#define JCE_CAP_FRAMEBUFFER_FLOAT (1u << 4)
#define JCE_RP_QUALITY_LOW 0
#define JCE_BACKEND_OPENGLES 1
#define JCE_BACKEND_NATIVE 2
typedef int JceRendererBackend;
typedef struct {
 bool hdr_color,enable_taa,enable_ssr,enable_ssgi,enable_motion_blur,depth_prepass;
 int csm_cascade_count,post_quality;
} JceRenderPipelineDesc;
static JceRenderPipelineDesc s_active;
static uint32_t caps;
static int backend=JCE_BACKEND_OPENGLES;
static JceRendererBackend jce_renderer_get_active_backend(void) { return backend; }
static uint32_t jce_renderer_get_caps(void) { return caps; }
''' + selector + r'''
int main(void)
{
 caps=JCE_CAP_TEXTURE_FLOAT|JCE_CAP_FRAMEBUFFER_FLOAT;
 s_active=(JceRenderPipelineDesc){0};s_active.hdr_color=true;s_active.enable_taa=true;
 apply_policy();assert(s_active.hdr_color && s_active.enable_taa);
 s_active.hdr_color=false;s_active.enable_taa=true;apply_policy();assert(!s_active.enable_taa);
 s_active.hdr_color=true;s_active.enable_taa=true;caps=JCE_CAP_TEXTURE_FLOAT;
 apply_policy();assert(!s_active.hdr_color && !s_active.enable_taa);
 s_active.hdr_color=true;s_active.enable_taa=true;caps=JCE_CAP_FRAMEBUFFER_FLOAT;
 apply_policy();assert(!s_active.hdr_color && !s_active.enable_taa);
 caps=JCE_CAP_TEXTURE_FLOAT|JCE_CAP_FRAMEBUFFER_FLOAT;
 s_active.hdr_color=true;s_active.enable_taa=true;s_active.enable_ssr=true;
 s_active.post_quality=2;s_active.csm_cascade_count=4;
 apply_policy();assert(s_active.hdr_color && s_active.enable_taa);
 assert(!s_active.enable_ssr && s_active.post_quality==0 && s_active.csm_cascade_count==1);
 backend=JCE_BACKEND_NATIVE;caps=0;s_active.hdr_color=true;s_active.enable_taa=true;
 apply_policy();assert(s_active.hdr_color && s_active.enable_taa);
 puts("GLES target policy PASS");return 0;
}
'''
        base = ROOT / "build/gles-target-tests"
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
                    self.skipTest("no C compiler available for the production selector probe")
                executable = directory / "probe"
                command = [compiler, "-std=c99", str(file), "-o", str(executable)]
                env = None
            result = subprocess.run(command, cwd=directory, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("GLES target policy PASS", result.stdout)

if __name__ == "__main__":
    unittest.main()
