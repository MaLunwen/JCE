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
        base = ROOT / "build/tests/gles-target-tests"
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


    def test_current_pre_jitter_taa_admission_and_old_wires(self):
        spec = importlib.util.spec_from_file_location("taa_admission_driver", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        scene = (ROOT / "engine/src/middleware/scene/jce_scene_renderer.c").read_text(encoding="utf-8")
        postfx = (ROOT / "engine/src/renderer/jce_postfx.c").read_text(encoding="utf-8")
        def actual(text, signature):
            start = text.index(signature)
            return text[start:text.index("\n}", start) + 2]
        format_owner = actual(postfx, "static bgfx_texture_format_t postfx_color_format(void)")
        supported = actual(postfx, "bool jce_postfx_taa_supported(void)")
        requested = actual(scene, "bool jce_scene_renderer_taa_requested(JceSceneRenderer *sr)")
        begin = actual(scene, "bool jce_scene_renderer_taa_begin_frame(JceSceneRenderer *sr,")
        standalone = (ROOT / "engine/include/jce/application/jce_default_main.inc.h").read_text(encoding="utf-8")
        bridge = actual(standalone, "static bool s_default_main_taa_requested(void)")
        game = (ROOT / "editor/src/scene/jce_editor_game_render.cpp").read_text(encoding="utf-8")
        self.assertIn("jce_scene_renderer_taa_requested(engine_sr)", game)
        self.assertIn("jce_postfx_taa_supported();", game)
        prefix = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int bgfx_texture_format_t;
#define BGFX_TEXTURE_FORMAT_RGBA8 0
#define BGFX_TEXTURE_FORMAT_RGBA16F 1
#define BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER 1
#define JCE_CVAR_FLAG_NONE 0
typedef struct { unsigned formats[2]; } bgfx_caps_t;
static bgfx_caps_t caps; static bool have_caps, hdr;
static const bgfx_caps_t *bgfx_get_caps(void) { return have_caps ? &caps : NULL; }
static bool jce_render_pipeline_is_feature_enabled(const char *name) { return hdr; }
typedef struct { bool enable_taa; } JceRenderPipelineDesc;
static JceRenderPipelineDesc policy;
static void jce_render_pipeline_get(JceRenderPipelineDesc *out) { *out=policy; }
typedef struct { bool value; float scalar; } JceCvar;
static void jce_cvar_set_bool(JceCvar *cv,bool value) { cv->value=value; }
static bool jce_cvar_get_bool(const JceCvar *cv) { return cv->value; }
static float jce_cvar_get_float(const JceCvar *cv) { return cv->scalar; }
static JceCvar tuning;
static JceCvar *jce_cvar_register_float(const char*a,float value,int f,const char*b) { tuning.scalar=value;return &tuning; }
typedef struct { float raw[4][4]; } jce_mat4;
typedef struct { bool prev_valid; jce_mat4 prev_proj,prev_view; unsigned frame_index; float current_jitter[2]; } JceTaaState;
typedef struct { bool enabled; unsigned matrices; } JcePostFXPipeline;
typedef struct { JceCvar *cv_taa; bool rp_taa_seen,rp_last_taa; JcePostFXPipeline *postfx_pipeline; JceTaaState taa_state; } JceSceneRenderer;
static JceSceneRenderer *s_scene_renderer;
static jce_mat4 jce_m4_multiply(const jce_mat4 *a,const jce_mat4*b) { return *a; }
static jce_mat4 jce_m4_inverse(const jce_mat4 *a) { return *a; }
static void jce_postfx_set_taa_matrices(JcePostFXPipeline *p,const jce_mat4*a,const jce_mat4*b) { ++p->matrices; }
static void jce_taa_advance(JceTaaState *s,unsigned w,unsigned h) { ++s->frame_index;s->current_jitter[0]=.125f; }
static void jce_taa_apply_jitter(jce_mat4*m,const float*j) { m->raw[0][0]+=j[0]; }
static void jce_postfx_set_taa(JcePostFXPipeline*p,bool e,float f,float l,float m) { p->enabled=e; }
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"CHECK FAILED line %d: %s\n",__LINE__,#c);return 1; } } while(0)
"""
        checks = r"""
int main(void) {
 JceCvar cv={true,0}; JcePostFXPipeline fx={0}; JceSceneRenderer sr={0};
 sr.cv_taa=&cv;sr.postfx_pipeline=&fx;
 jce_mat4 clean={0},out={0};clean.raw[0][0]=1;
 have_caps=true;caps.formats[1]=1;hdr=true;policy.enable_taa=false;
 s_scene_renderer=&sr;
 CHECK(!s_default_main_taa_requested());CHECK(!cv.value);
 sr.rp_taa_seen=false;cv.value=true; /* independently verify begin-frame wire */
 CHECK(!jce_scene_renderer_taa_begin_frame(&sr,32,32,&clean,&clean,&out));
 CHECK(!cv.value && sr.rp_taa_seen && sr.taa_state.frame_index==0);
 CHECK(memcmp(&clean,&out,sizeof(clean))==0 && fx.matrices==1);
 policy.enable_taa=true;
 CHECK(jce_scene_renderer_taa_begin_frame(&sr,32,32,&clean,&clean,&out));
 CHECK(cv.value && sr.taa_state.frame_index==1 && fx.enabled);
 cv.value=false;CHECK(!jce_scene_renderer_taa_requested(&sr));
 CHECK(sr.rp_last_taa); /* unchanged policy preserves explicit override */
 cv.value=true;hdr=false;unsigned count=sr.taa_state.frame_index;
 CHECK(!jce_scene_renderer_taa_begin_frame(&sr,32,32,&clean,&clean,&out));
 CHECK(sr.taa_state.frame_index==count && memcmp(&clean,&out,sizeof(clean))==0);
 hdr=true;caps.formats[1]=0;
 CHECK(!jce_postfx_taa_supported());
 CHECK(!jce_scene_renderer_taa_begin_frame(&sr,32,32,&clean,&clean,&out));
 CHECK(sr.taa_state.frame_index==count);
 have_caps=false;CHECK(!jce_postfx_taa_supported());
 have_caps=true;caps.formats[1]=1;
 CHECK(!jce_scene_renderer_taa_begin_frame(&sr,0,32,&clean,&clean,&out));
 CHECK(!jce_scene_renderer_taa_begin_frame(&sr,32,0,&clean,&clean,&out));
 CHECK(sr.taa_state.frame_index==count);
 policy.enable_taa=false;CHECK(!jce_scene_renderer_taa_requested(&sr));
 CHECK(!cv.value);CHECK(!jce_scene_renderer_taa_requested(NULL));
 CHECK(!jce_scene_renderer_taa_begin_frame(NULL,32,32,&clean,&clean,&out));
 CHECK(memcmp(&clean,&out,sizeof(clean))==0);
 puts("actual pre-jitter request/format admission PASS");return 0;
}
"""
        old_request = begin.replace("!jce_scene_renderer_taa_requested(sr)", "!(sr->cv_taa ? jce_cvar_get_bool(sr->cv_taa) : false)")
        old_format = begin.replace("        !jce_postfx_taa_supported() || ", "        ")
        self.assertNotEqual(begin, old_request)
        self.assertNotEqual(begin, old_format)
        old_bridge = bridge.replace("jce_scene_renderer_taa_requested(s_scene_renderer) &&\n           jce_postfx_taa_supported()", "jce_cvar_get_bool(s_scene_renderer->cv_taa)")
        self.assertNotEqual(bridge, old_bridge)
        base = ROOT / "build/tests/taa-admission-tests"
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            directory = Path(temporary)
            for name, wire, route, expected in (("current", begin, bridge, 0), ("old_request", old_request, bridge, 1), ("old_format", old_format, bridge, 1), ("old_bridge", begin, old_bridge, 1)):
                file = directory / (name + ".c")
                file.write_text(prefix + format_owner + supported + requested + route + wire + checks, encoding="utf-8")
                if driver.HOST == "windows":
                    command = ["cl", "/nologo", "/TC", "/std:c11", "/W3", str(file), "/Fe:"+name+".exe", "/Fo:"+name+".obj"]
                    env = driver.msvc_env(driver.resolve_target(None))
                    executable = directory / (name + ".exe")
                else:
                    compiler = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
                    if not compiler: self.skipTest("no C compiler available for actual pre-jitter owner")
                    executable = directory / name
                    command = [compiler, "-std=c99", str(file), "-o", str(executable)]
                    env = None
                result = subprocess.run(command, cwd=directory, env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True)
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                self.assertIn("admission PASS" if expected==0 else "CHECK FAILED", result.stdout + result.stderr)

if __name__ == "__main__":
    unittest.main()
