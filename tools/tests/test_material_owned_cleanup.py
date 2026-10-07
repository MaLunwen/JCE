"""Run production material and scene cache teardown against GPU ownership spies."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(relative):
    control = os.environ.get("JCE_CLEANUP_CONTROL_ROOT")
    path = Path(control) / Path(relative).name if control else ROOT / relative
    return path.read_text(encoding="utf-8")


def function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def execute(test, code):
    spec = importlib.util.spec_from_file_location("material_cleanup_driver", ROOT / "tools/build/jce.py")
    driver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(driver)
    base = ROOT / "build/material-cleanup-tests"
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=base) as temporary:
        directory = Path(temporary)
        path = directory / "probe.c"
        path.write_text(code, encoding="utf-8")
        if driver.HOST == "windows":
            command = ["cl", "/nologo", "/TC", "/std:c11", "/W3", str(path), "/Fe:probe.exe", "/Fo:probe.obj"]
            env = driver.msvc_env(driver.resolve_target(None))
            executable = directory / "probe.exe"
        else:
            compiler = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
            if not compiler:
                test.skipTest("no C compiler for production ownership probe")
            executable = directory / "probe"
            command = [compiler, "-std=c99", str(path), "-o", str(executable)]
            env = None
        result = subprocess.run(command, cwd=directory, env=env, capture_output=True, text=True)
        test.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True)
        test.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class MaterialCleanupTests(unittest.TestCase):
    def test_shared_light_bindings_device_recreation(self):
        text = source("engine/src/renderer/jce_lighting_system.c")
        declarations = text[text.index("static struct {"):text.index("} s_lu;") + len("} s_lu;")]
        signature = "void jce_lighting_system_shutdown(void)"
        shutdown = function(text, signature) if signature in text else signature + "{}"
        execute(self, r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
typedef struct {uint16_t idx;} bgfx_uniform_handle_t;
typedef bgfx_uniform_handle_t bgfx_texture_handle_t;
#define BGFX_HANDLE_IS_VALID(h) ((h).idx!=UINT16_MAX)
static unsigned uniforms,textures;
static bool live[64];
static void bgfx_destroy_uniform(bgfx_uniform_handle_t h){assert(live[h.idx]);live[h.idx]=false;uniforms++;}
static void bgfx_destroy_texture(bgfx_texture_handle_t h){assert(live[h.idx]);live[h.idx]=false;textures++;}
''' + declarations + shutdown + r'''
static void initialized_fixture(bool valid_textures){
    bgfx_uniform_handle_t *u[]={&s_lu.ambient_color,&s_lu.dir_lights,&s_lu.point_lights,
      &s_lu.spot_lights,&s_lu.light_counts,&s_lu.area_lights,&s_lu.area_params,
      &s_lu.camera_pos,&s_lu.ies_params,&s_lu.cookie_dir_params,&s_lu.cookie_dir_vp,
      &s_lu.s_cookie,&s_lu.s_ies_lut};
    for(int i=0;i<13;i++){u[i]->idx=(uint16_t)(i+1);live[i+1]=true;}
    s_lu.white_1x1.idx=valid_textures?14:UINT16_MAX;
    s_lu.cookie_atlas_array.idx=valid_textures?15:UINT16_MAX;
    if(valid_textures)live[14]=live[15]=true;
    s_lu.init=true;s_lu.cookie_array_ok=true;
}
int main(void){
    jce_lighting_system_shutdown();assert(uniforms==0&&textures==0);
    for(int cycle=0;cycle<2;cycle++){
        initialized_fixture(true);jce_lighting_system_shutdown();
        assert(uniforms==13*(cycle+1)&&textures==2*(cycle+1));
        assert(!s_lu.init&&!s_lu.cookie_array_ok);
        jce_lighting_system_shutdown();
    }
    initialized_fixture(false);jce_lighting_system_shutdown();assert(uniforms==39&&textures==4);
    for(int i=0;i<64;i++)assert(!live[i]);return 0;
}
''')

    def test_lazy_binder_device_recreation_and_partial_failure(self):
        text = source("engine/src/renderer/jce_pbr_material.c")
        declarations = text[text.index("static struct {"):text.index("/* Global view mode")]
        declarations += "static float s_view_mode;"
        ensure = function(text, "static void ensure_uniforms(void)\n{")
        shutdown = function(text, "void jce_pbr_material_shutdown(void)")
        execute(self, r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
typedef struct {uint16_t idx;} bgfx_uniform_handle_t;
typedef bgfx_uniform_handle_t bgfx_texture_handle_t;
typedef bgfx_uniform_handle_t JceShaderHandle;
typedef struct {int unused;} bgfx_memory_t;
#define BGFX_HANDLE_IS_VALID(h) ((h).idx != UINT16_MAX)
#define BGFX_UNIFORM_TYPE_VEC4 0
#define BGFX_UNIFORM_TYPE_SAMPLER 1
#define BGFX_TEXTURE_FORMAT_RGBA8 0
#define LOG_DEBUG(...) ((void)0)
static unsigned next_handle=1, uniform_creates, texture_creates, programs;
static unsigned uniform_deletes, texture_deletes;
static bool live[1024], fail_texture;
static bgfx_uniform_handle_t bgfx_create_uniform(const char *n,int t,int count){
    (void)n;(void)t;(void)count;bgfx_uniform_handle_t h={(uint16_t)next_handle++};
    live[h.idx]=true;uniform_creates++;return h;}
static const bgfx_memory_t *bgfx_copy(const void *p,uint32_t n){(void)p;(void)n;return NULL;}
static bgfx_texture_handle_t bgfx_create_texture_2d(int w,int h,bool m,int layers,int f,
    uint64_t flags,const bgfx_memory_t *data,int skip){
    (void)w;(void)h;(void)m;(void)layers;(void)f;(void)flags;(void)data;(void)skip;
    bgfx_texture_handle_t result={UINT16_MAX};if(fail_texture)return result;
    result.idx=(uint16_t)next_handle++;live[result.idx]=true;texture_creates++;return result;}
static void bgfx_destroy_uniform(bgfx_uniform_handle_t h){assert(live[h.idx]);live[h.idx]=false;uniform_deletes++;}
static void bgfx_destroy_texture(bgfx_texture_handle_t h){assert(live[h.idx]);live[h.idx]=false;texture_deletes++;}
static void jce_renderer_destroy_program(JceShaderHandle h){assert(h.idx==700);programs++;}
static struct {bool used;uint16_t program;} s_prog_cache[2];
static int s_prog_cache_count;
''' + declarations + ensure + shutdown + r'''
int main(void){
    jce_pbr_material_shutdown();assert(uniform_deletes==0&&texture_deletes==0);
    ensure_uniforms();ensure_uniforms();assert(uniform_creates==14&&texture_creates==2);
    s_prog_cache_count=1;s_prog_cache[0].used=true;s_prog_cache[0].program=700;
    s_view_mode=2;s_uv_override[0]=3;
    jce_pbr_material_shutdown();assert(uniform_deletes==14&&texture_deletes==2&&programs==1);
    assert(!s_uniforms_init&&s_view_mode==0&&s_uv_override[0]==0);
    jce_pbr_material_shutdown();assert(uniform_deletes==14&&texture_deletes==2&&programs==1);
    ensure_uniforms();assert(uniform_creates==28&&texture_creates==4);
    jce_pbr_material_shutdown();assert(uniform_deletes==28&&texture_deletes==4);
    fail_texture=true;ensure_uniforms();jce_pbr_material_shutdown();
    assert(uniform_deletes==42&&texture_deletes==4);
    for(unsigned i=1;i<next_handle;i++)assert(!live[i]);return 0;
}
''')

    def test_scene_uploaded_pending_failed_and_borrowed_textures(self):
        text = source("engine/src/middleware/scene/jce_scene_renderer.c")
        start = text.index("    for (int i = 0; i < sr->tex_cache_count;", text.index("void jce_scene_renderer_destroy("))
        end = text.index("    /* Shader Graph custom-program cache:", start)
        cleanup = text[start:end]
        execute(self, r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef struct {uint16_t idx;} JceTexture;
struct SrTexJob {int id;};
typedef struct {struct {JceTexture tex;bool used,pending;void *task;struct SrTexJob *job;} tex_cache[4];
    int tex_cache_count,tex_inflight;int tex_hash[8];} JceSceneRenderer;
static unsigned textures,cancels,jobs;
static bool jce_texture_valid(JceTexture h){return h.idx!=UINT16_MAX;}
static void jce_texture_destroy(JceTexture h){assert(h.idx==42);textures++;}
static void sr_async_task_cancel_and_release(void **t){assert(*t);*t=NULL;cancels++;}
static void sr_tex_job_destroy(struct SrTexJob *j){assert(j&&j->id==9);jobs++;}
static void cleanup(JceSceneRenderer *sr){
''' + cleanup + r'''
}
int main(void){JceSceneRenderer sr;memset(&sr,0,sizeof sr);struct SrTexJob job={9};
    sr.tex_cache_count=4;sr.tex_inflight=1;
    for(int i=0;i<4;i++){sr.tex_cache[i].tex.idx=UINT16_MAX;sr.tex_cache[i].used=true;}
    sr.tex_cache[0].tex.idx=42;
    sr.tex_cache[1].pending=true;sr.tex_cache[1].job=&job;sr.tex_cache[1].task=&job;
    /* Callback texture 99 is borrowed and never inserted in this owned cache. */
    sr.tex_cache[3].used=false;sr.tex_cache[3].tex.idx=99;sr.tex_hash[0]=1;
    cleanup(&sr);assert(textures==1&&cancels==1&&jobs==1);
    assert(sr.tex_cache_count==0&&sr.tex_inflight==0&&sr.tex_hash[0]==0);
    for(int i=0;i<4;i++){assert(!sr.tex_cache[i].used&&!sr.tex_cache[i].pending);
        assert(sr.tex_cache[i].tex.idx==UINT16_MAX&&sr.tex_cache[i].job==NULL&&sr.tex_cache[i].task==NULL);}
    cleanup(&sr);assert(textures==1&&cancels==1&&jobs==1);return 0;
}
''')


if __name__ == "__main__":
    unittest.main()
