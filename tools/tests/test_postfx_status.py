"""Exercise the production TAA status logging with separate pipeline and submission controls."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]

class PostFXStatusTests(unittest.TestCase):
    def test_two_pipelines_and_request_without_submission(self):
        spec = importlib.util.spec_from_file_location("postfx_format_driver", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        source = (ROOT / "engine/src/renderer/jce_postfx.c").read_text(encoding="utf-8")
        reset = source[source.index("static void reset_output_state("):source.index("/* ── Default parameters") ]
        composite_begin = source.index("            const bool composite_submitted =")
        composite_end = source.index("\n", source.index("pipeline->dof_active = composite_submitted", composite_begin))
        composite = source[composite_begin:composite_end]
        selector = source[source.index("static void postfx_log_taa_state("):source.index("void jce_postfx_apply(")]
        submit = source[source.index("static bool submit_postfx("):source.index("/* AUTO EXPOSURE: targets")]
        call = "postfx_log_taa_state(p, submitted)" if "bool submitted" in selector else "postfx_log_taa_state(p)"
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
typedef struct {uint16_t idx;} bgfx_program_handle_t;
typedef bgfx_program_handle_t bgfx_vertex_buffer_handle_t;
typedef bgfx_program_handle_t bgfx_index_buffer_handle_t;
typedef bgfx_program_handle_t bgfx_texture_handle_t;
typedef bgfx_program_handle_t bgfx_frame_buffer_handle_t;
typedef struct {bool taa_enabled,taa_log_reported,taa_log_submitted;uint16_t view_base;
 bgfx_vertex_buffer_handle_t quad_vb;bgfx_index_buffer_handle_t quad_ib;
 bgfx_texture_handle_t output_tex;bgfx_frame_buffer_handle_t output_fb;
 int output_ping;bool mb_active,dof_active,mb_enabled,dof_enabled;
 bool limited_composite_sampling,composite_limit_reported;
 bgfx_program_handle_t prog_composite;} JcePostFXPipeline;
#define BGFX_DISCARD_ALL 255
#define BGFX_STATE_WRITE_RGB 1
#define BGFX_STATE_WRITE_A 2
static unsigned submits,discards,vertex_sets;
static uint16_t resolved=42;
static uint16_t jce_shader_deferred_resolve(uint16_t value){(void)value;return resolved;}
static void bgfx_submit(uint16_t view,bgfx_program_handle_t p,uint32_t depth,uint8_t flags)
{(void)depth;(void)flags;assert(view==34 && p.idx==42);submits++;}
static void bgfx_discard(uint8_t flags){(void)flags;discards++;}
static void bgfx_set_vertex_buffer(uint8_t stream,bgfx_vertex_buffer_handle_t p,uint32_t start,uint32_t count)
{(void)stream;(void)p;(void)start;(void)count;vertex_sets++;}
static void bgfx_set_index_buffer(bgfx_index_buffer_handle_t p,uint32_t start,uint32_t count)
{(void)p;(void)start;(void)count;}
static void bgfx_set_state(uint64_t state,uint32_t color){(void)state;(void)color;}
static unsigned records;
static char messages[16][128];
static void record(const char *tag,const char *format,...) {
 (void)tag;assert(records<16);va_list args;va_start(args,format);
 vsnprintf(messages[records++],128,format,args);va_end(args);
}
#define LOG_TAG "test"
#define LOG_INFO record
#define LOG_WARN record
''' + reset + submit + selector + "\nstatic void status(JcePostFXPipeline *p,bool submitted) {" + call + ";}\n" + "\nstatic void composite_status(JcePostFXPipeline *pipeline) {const uint16_t view_id=34;const bool c_mblur=true,c_dof=true;\n" + composite + "\n}\n" + r'''
int main(int argc,char **argv)
{
 assert(argc==2);JcePostFXPipeline scene={0},game={0};
 scene.taa_enabled=true;scene.view_base=33;game.view_base=150;
 if(!strcmp(argv[1],"interleave")) {
  for(int i=0;i<4;i++){status(&scene,true);status(&game,false);}
  assert(records==2);
  status(&scene,false);assert(records==3);
  status(&scene,false);assert(records==3);
 } else if(!strcmp(argv[1],"submission")) {
  bgfx_program_handle_t program={10};scene.quad_vb.idx=2;scene.quad_ib.idx=3;
  assert(draw_fullscreen(&scene,34,program));assert(submits==1 && discards==0);
  resolved=UINT16_MAX;assert(!draw_fullscreen(&scene,34,program));
  assert(submits==1 && discards==1);
  scene.quad_vb.idx=UINT16_MAX;unsigned previous=vertex_sets;
  assert(!draw_fullscreen(&scene,34,program));assert(vertex_sets==previous);
  assert(submits==1 && discards==2);
 } else if(!strcmp(argv[1],"compat-composite")) {
  scene.limited_composite_sampling=true;
  assert(!postfx_composite_multisampling(&scene) && records==0);
  scene.dof_enabled=true;
  assert(!postfx_composite_multisampling(&scene) && records==1);
  scene.mb_enabled=true;
  assert(!postfx_composite_multisampling(&scene) && records==1);
  assert(scene.dof_enabled && scene.mb_enabled);
  assert(strstr(messages[0],"GL 2.1 stock Composite"));
  game.dof_enabled=game.mb_enabled=true;
  assert(postfx_composite_multisampling(&game) && records==1);
 } else if(!strcmp(argv[1],"output")) {
  scene.mb_active=scene.dof_active=true;scene.output_tex.idx=9;scene.output_fb.idx=10;scene.output_ping=1;
  reset_output_state(&scene);assert(!scene.mb_active && !scene.dof_active);
  assert(scene.output_tex.idx==UINT16_MAX && scene.output_fb.idx==UINT16_MAX && scene.output_ping==-1);
  scene.quad_vb.idx=2;scene.quad_ib.idx=3;
  composite_status(&scene);assert(scene.mb_active && scene.dof_active);
  resolved=UINT16_MAX;composite_status(&scene);assert(!scene.mb_active && !scene.dof_active);
 } else {
  status(&scene,false);assert(records==1);
  assert(strstr(messages[0],"taa resolve: off"));
 }
 puts("PostFX status PASS");return 0;
}
'''
        base = ROOT / "build/tests/postfx-status-tests"
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
            for control in ("interleave", "unsubmitted", "submission", "output", "compat-composite"):
                result = subprocess.run([str(executable),control], cwd=directory, capture_output=True, text=True)
                with self.subTest(control=control):
                    self.assertEqual(result.returncode, 0, control + ": " + result.stdout + result.stderr)
                    self.assertIn("PostFX status PASS", result.stdout)

if __name__ == "__main__":
    unittest.main()
