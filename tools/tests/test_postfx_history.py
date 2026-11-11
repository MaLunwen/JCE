"""Compile actual TAA uniform/history policy and copy submission with failure controls."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]

class PostFXHistoryTests(unittest.TestCase):
    def test_first_resume_and_copy_submission(self):
        spec = importlib.util.spec_from_file_location("postfx_format_driver", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        source = (ROOT / "engine/src/renderer/jce_postfx.c").read_text(encoding="utf-8")
        begin = source.index("void jce_postfx_apply(")
        if "static bool postfx_take_taa_history(" in source:
            policy = source[source.index("static bool postfx_take_taa_history("):source.index("/* Report CPU resolve submissions")]
        else:
            # Compile the original uniform binding rather than recreating its policy.
            binding = source[source.index("        bgfx_set_uniform(pipeline->u_taaParams,"):].split("\n", 1)[0]
            policy = "static bool postfx_take_taa_history(JcePostFXPipeline *p){return p->history_valid;}\n" + \
                     "static void postfx_bind_taa_params(JcePostFXPipeline *pipeline,bool history_available,bool motion_available){" + binding + "}\n"
        if "postfx_take_frame_inputs" not in policy:
            policy += "typedef struct {bool have_matrices;bgfx_texture_handle_t ext_motion;} PostFXFrameInputs;\nstatic PostFXFrameInputs postfx_take_frame_inputs(JcePostFXPipeline *p){PostFXFrameInputs i={p->taa_have_matrices,p->taa_ext_motion_tex};return i;}\n"
        # Consume the live device/HDR selector and support query used by the
        # real setter. Counted external ports below provide changing inputs;
        # they do not replace production capability policy with a true stub.
        selector_start = source.index("static bgfx_texture_format_t postfx_color_format(")
        selector_end = source.index("\n}", selector_start) + 2
        selector = source[selector_start:selector_end]
        supported = source[source.index("bool jce_postfx_taa_supported("):
                           source.index("void jce_postfx_set_taa(")]
        setter = source[source.index("void jce_postfx_set_taa("):source.index("void jce_postfx_set_taa_matrices(")]
        support_guard = "if (enabled && !jce_postfx_taa_supported())"
        self.assertEqual(setter.count(support_guard), 1)
        copy = source[source.index("        /* (c) copy resolve",begin):source.index("    /* ── 1. Bloom",begin)].rstrip()
        assert copy.endswith("}")
        copy = copy[:-1]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {uint16_t idx;} bgfx_uniform_handle_t;
typedef bgfx_uniform_handle_t bgfx_program_handle_t;
typedef bgfx_uniform_handle_t bgfx_texture_handle_t;
typedef struct {bool history_valid,taa_fbos_valid,taa_enabled,taa_have_matrices;float taa_params[4];
 bgfx_uniform_handle_t u_taaParams,u_texColor;bgfx_program_handle_t prog_present;
 bgfx_uniform_handle_t taa_history_fb;bgfx_texture_handle_t taa_ext_motion_tex;} JcePostFXPipeline;
typedef enum { BGFX_TEXTURE_FORMAT_RGBA8, BGFX_TEXTURE_FORMAT_RGBA16F } bgfx_texture_format_t;
typedef struct { uint32_t formats[2]; } bgfx_caps_t;
#define BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER 0x1000u
#define LOG_TAG "history-test"
#define LOG_WARN(...) ((void)0)
static bgfx_caps_t device;
static bool initialized=true,hdr=true;
static const bgfx_caps_t *bgfx_get_caps(void){return initialized ? &device : NULL;}
static bool jce_render_pipeline_is_feature_enabled(const char *name)
{assert(strcmp(name,"hdr_color")==0);return hdr;}
/* New unsupported-policy witnesses fail normally in the old-wire program,
   without relying on a crash or a disabled assertion build. */
#define CHECK_POLICY(expr) do { if (!(expr)) return 73; } while (0)
static float recorded[4];
static bool copy_ready=true,copy_submit=true;
static void bgfx_set_uniform(bgfx_uniform_handle_t handle,const void *values,uint16_t count)
{(void)handle;assert(count==1);memcpy(recorded,values,sizeof recorded);}
static bool postfx_program_ready(bgfx_program_handle_t handle){(void)handle;return copy_ready;}
static bool draw_fullscreen(JcePostFXPipeline *p,uint16_t view,bgfx_program_handle_t handle)
{(void)p;(void)view;(void)handle;return copy_submit;}
#define POSTFX_SETUP_VIEW(view,fb) ((void)0)
#define bgfx_set_view_name(view,name,length) ((void)0)
#define bgfx_set_texture(stage,sampler,texture,flags) ((void)0)
''' + selector + supported + setter + policy + "\nstatic void copy_history(JcePostFXPipeline *pipeline) {\n" + r'''
 const uint16_t v_copy=22;bgfx_texture_handle_t resolved={4},current_tex={2};
 int current_fb_index=-1,ping=0;
''' + copy + r'''
}
int main(void)
{
 JcePostFXPipeline p={0};
 device.formats[BGFX_TEXTURE_FORMAT_RGBA16F]=BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER;
 CHECK_POLICY(jce_postfx_taa_supported());
 jce_postfx_set_taa(&p,true,0.95f,1.2f,1.3f);CHECK_POLICY(p.taa_enabled);
 device.formats[BGFX_TEXTURE_FORMAT_RGBA16F]=0;
 CHECK_POLICY(!jce_postfx_taa_supported());
 jce_postfx_set_taa(&p,true,0.95f,1.2f,1.3f);
 CHECK_POLICY(!p.taa_enabled && p.taa_params[0]==0.95f);
 device.formats[BGFX_TEXTURE_FORMAT_RGBA16F]=BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER;
 hdr=false;CHECK_POLICY(!jce_postfx_taa_supported());
 jce_postfx_set_taa(&p,true,0.95f,1.2f,1.3f);CHECK_POLICY(!p.taa_enabled);
 hdr=true;initialized=false;CHECK_POLICY(!jce_postfx_taa_supported());
 jce_postfx_set_taa(&p,true,0.95f,1.2f,1.3f);CHECK_POLICY(!p.taa_enabled);
 initialized=true;CHECK_POLICY(jce_postfx_taa_supported());
 jce_postfx_set_taa(&p,true,0.95f,1.2f,1.3f);CHECK_POLICY(p.taa_enabled);
 jce_postfx_set_taa(NULL,true,0.95f,1.2f,1.3f);
 p.taa_have_matrices=true;p.taa_ext_motion_tex.idx=7;
 PostFXFrameInputs inputs=postfx_take_frame_inputs(&p);
 assert(inputs.have_matrices && inputs.ext_motion.idx==7);
 assert(!p.taa_have_matrices && p.taa_ext_motion_tex.idx==UINT16_MAX);
 inputs=postfx_take_frame_inputs(&p);assert(!inputs.have_matrices && inputs.ext_motion.idx==UINT16_MAX);
 p.taa_params[0]=0.95f;p.taa_params[1]=1.2f;p.taa_params[2]=1.3f;
 postfx_bind_taa_params(&p,false,true);assert(recorded[0]==0.0f);
 assert(recorded[1]==1.2f && recorded[2]==1.3f && p.taa_params[0]==0.95f);
 postfx_bind_taa_params(&p,true,false);assert(recorded[0]==0.0f);
 postfx_bind_taa_params(&p,true,true);assert(recorded[0]==0.95f);
 p.history_valid=true;p.taa_fbos_valid=true;
 assert(postfx_take_taa_history(&p));assert(!p.history_valid);
 /* A skipped frame cannot leave history eligible for the next frame. */
 assert(!postfx_take_taa_history(&p));
 p.history_valid=true;p.taa_fbos_valid=false;assert(!postfx_take_taa_history(&p));assert(!p.history_valid);
 p.taa_fbos_valid=true;p.history_valid=true;jce_postfx_set_taa(&p,false,0.95f,1.2f,1.3f);
 assert(p.history_valid && !p.taa_enabled);
 /* Renderer end-frame temporarily disarms TAA; next begin-frame can reuse
    the immediately preceding submitted history if no apply was skipped. */
 jce_postfx_set_taa(&p,true,0.95f,1.2f,1.3f);assert(p.taa_enabled && p.history_valid);
 assert(postfx_take_taa_history(&p));assert(!p.history_valid);
 /* An actually disabled/skipped apply consumes eligibility without copying. */
 jce_postfx_set_taa(&p,false,0.95f,1.2f,1.3f);postfx_take_taa_history(&p);
 jce_postfx_set_taa(&p,true,0.95f,1.2f,1.3f);assert(!p.history_valid);
 copy_history(&p);assert(p.history_valid);
 p.history_valid=false;copy_submit=false;copy_history(&p);assert(!p.history_valid);
 copy_ready=false;copy_submit=true;copy_history(&p);assert(!p.history_valid);
 puts("PostFX history PASS");return 0;
}
'''
        base = ROOT / "build/tests/postfx-history-tests"
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
            for old_guard in (False, True):
                with self.subTest(old_guard=old_guard):
                    program = harness
                    if old_guard:
                        self.assertEqual(program.count(support_guard), 1)
                        program = program.replace(support_guard, "if (false)", 1)
                    file.write_text(program, encoding="utf-8")
                    result = subprocess.run(command, cwd=directory, env=env, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    result = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 73 if old_guard else 0,
                                     result.stdout + result.stderr)
                    if old_guard:
                        self.assertNotIn("PostFX history PASS", result.stdout)
                    else:
                        self.assertIn("PostFX history PASS", result.stdout)

if __name__ == "__main__":
    unittest.main()
