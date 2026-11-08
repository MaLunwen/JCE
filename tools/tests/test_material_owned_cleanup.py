"""Run production renderer/material/cache owners against counted CPU ports.

These controls prove ownership, admission and teardown; they do not prove GPU
transfer, compiled shader validity or host device behavior.
"""
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


def execute(test, code, expected_runtime_failure=None):
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
            command = ["cl", "/nologo", "/TC", "/std:c11", "/W3",
                       "/I" + str(ROOT / "engine/include"), "/I" + str(ROOT / "engine/src"),
                       str(path), "/Fe:probe.exe", "/Fo:probe.obj"]
            env = driver.msvc_env(driver.resolve_target(None))
            executable = directory / "probe.exe"
        else:
            compiler = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
            if not compiler:
                test.skipTest("no C compiler for production ownership probe")
            executable = directory / "probe"
            command = [compiler, "-std=c99", "-I" + str(ROOT / "engine/include"),
                       "-I" + str(ROOT / "engine/src"), str(path), "-o", str(executable)]
            env = None
        result = subprocess.run(command, cwd=directory, env=env, capture_output=True, text=True)
        test.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True)
        if expected_runtime_failure is None:
            test.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            # The compiler must succeed above. Only the actual runtime assertion
            # with this witness is accepted; a compile error cannot prove it.
            test.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            test.assertIn(expected_runtime_failure, result.stdout + result.stderr)


class MaterialCleanupTests(unittest.TestCase):
    def test_shared_light_bindings_device_recreation(self):
        text = source("engine/src/renderer/jce_lighting_system.c")
        declarations = text[text.index("static struct {"):text.index("} s_lu;") + len("} s_lu;")]
        signature = "void jce_lighting_system_shutdown(void)"
        shutdown = function(text, signature) if signature in text else signature + "{}"
        cookie = source("engine/src/renderer/jce_light_cookie_internal.h")
        cookie_types = cookie[cookie.index("typedef enum JceLightCookieReason {"):
                              cookie.index("static inline JceLightCookieTexture jce_light_cookie_texture_create(")]
        cookie_destroy = function(cookie, "static inline void jce_light_cookie_texture_destroy(")
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
''' + cookie_types + cookie_destroy + declarations + shutdown + r'''
static void initialized_fixture(bool valid_textures){
    bgfx_uniform_handle_t *u[]={&s_lu.ambient_color,&s_lu.dir_lights,&s_lu.point_lights,
      &s_lu.spot_lights,&s_lu.light_counts,&s_lu.area_lights,&s_lu.area_params,
      &s_lu.camera_pos,&s_lu.ies_params,&s_lu.cookie_dir_params,&s_lu.cookie_dir_vp,
      &s_lu.s_cookie,&s_lu.s_ies_lut};
    for(int i=0;i<13;i++){u[i]->idx=(uint16_t)(i+1);live[i+1]=true;}
    s_lu.white_1x1.idx=valid_textures?14:UINT16_MAX;
    s_lu.cookie_texture.texture.idx=valid_textures?15:UINT16_MAX;
    s_lu.cookie_texture.atlas_enabled=valid_textures;
    s_lu.cookie_texture.reason=JCE_LIGHT_COOKIE_READY;
    if(valid_textures)live[14]=live[15]=true;
    s_lu.init=true;
}
int main(void){
    jce_lighting_system_shutdown();assert(uniforms==0&&textures==0);
    for(int cycle=0;cycle<2;cycle++){
        initialized_fixture(true);jce_lighting_system_shutdown();
        assert(uniforms==13*(cycle+1)&&textures==2*(cycle+1));
        assert(!s_lu.init&&!s_lu.cookie_texture.atlas_enabled);
        assert(s_lu.cookie_texture.texture.idx==UINT16_MAX);
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
        cache = text[text.index("#define PBR_PROG_CACHE_MAX"):text.index("/* Load + link a custom shader program")]
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
''' + declarations + '\n' + cache + ensure + shutdown + r'''
int main(void){
    const unsigned uniforms_per_device=15; /* includes u_toonAtlas */
    jce_pbr_material_shutdown();assert(uniform_deletes==0&&texture_deletes==0);
    ensure_uniforms();ensure_uniforms();assert(uniform_creates==uniforms_per_device&&texture_creates==2);
    s_prog_cache.count=1;s_prog_cache.entries[0].used=true;s_prog_cache.entries[0].program=700;
    s_view_mode=2;s_uv_override[0]=3;
    jce_pbr_material_shutdown();assert(uniform_deletes==uniforms_per_device&&texture_deletes==2&&programs==1);
    assert(!s_uniforms_init&&s_view_mode==0&&s_uv_override[0]==0);
    jce_pbr_material_shutdown();assert(uniform_deletes==uniforms_per_device&&texture_deletes==2&&programs==1);
    ensure_uniforms();assert(uniform_creates==2*uniforms_per_device&&texture_creates==4);
    jce_pbr_material_shutdown();assert(uniform_deletes==2*uniforms_per_device&&texture_deletes==4);
    fail_texture=true;ensure_uniforms();jce_pbr_material_shutdown();
    assert(uniform_deletes==3*uniforms_per_device&&texture_deletes==4);
    for(unsigned i=1;i<next_handle;i++)assert(!live[i]);return 0;
}
''')

    def test_renderer_reload_admission_swap_and_partial_failure(self):
        text = source("engine/src/renderer/jce_renderer.c")
        install = function(text, "void jce_renderer_set_shaders(")
        snapshot = function(text, "static JceShaderSet renderer_owned_shader_set(")
        reload = function(text, "bool jce_renderer_reload_shaders_fs(")
        execute(self, r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "renderer/jce_shader_set_owned_internal.h"
/* Use the production owner field list and JceShaderSet alias release. Backend
 * handles are counted CPU ports, not a substitute for renderer/GPU execution. */
typedef struct {uint16_t idx;} bgfx_program_handle_t;
typedef struct JceRenderer {
    bool is_fallback, headless;
#define OWNER_FIELD(member_, renderer_) bgfx_program_handle_t renderer_;
    JCE_SHADER_SET_OWNED_FIELDS(OWNER_FIELD)
#undef OWNER_FIELD
    JceShaderHandle program_variant[JCE_SHADER_VARIANT_COUNT][JCE_SHADER_KEY_COUNT];
} JceRenderer;
#define LOG_ERROR(...) ((void)0)
#define LOG_INFO(...) ((void)0)
static JceShaderSet next;
static JceRenderer *active;
static unsigned loads, destroys[64];
static bool live[64], require_swap;
JceShaderSet jce_shaders_load_all_fs(const char *dir,const JcePakArchive *pak){
    (void)dir;assert(pak);loads++;return next;
}
void jce_shader_program_destroy(JceShaderHandle h){
    assert(h.idx<64&&live[h.idx]);
    if(require_swap&&(h.idx==5||h.idx==7))assert(active->program.idx==20);
    live[h.idx]=false;destroys[h.idx]++;
}
''' + install + snapshot + reload + r'''
int main(void){
    JceRenderer r;memset(&r,0xff,sizeof r);r.is_fallback=false;r.headless=true;
    const JcePakArchive *pak=(const JcePakArchive *)&r;active=&r;
    memset(&next,0xff,sizeof next);next.color.idx=20;live[20]=true;
    assert(!jce_renderer_reload_shaders_fs(&r,"overlay",pak));
    assert(!jce_renderer_reload_shaders_fs(&r,"overlay",pak));
    assert(loads==0&&r.program.idx==UINT16_MAX&&live[20]);
    r.headless=false;r.is_fallback=true;
    assert(!jce_renderer_reload_shaders_fs(&r,"overlay",pak));
    r.is_fallback=false;
    assert(!jce_renderer_reload_shaders_fs(&r,"overlay",NULL));
    assert(!jce_renderer_reload_shaders_fs(NULL,"overlay",pak));assert(loads==0);
    r.program.idx=5;r.program_text_sdf.idx=7;r.program_variant[0][0].idx=7;
    live[5]=live[7]=true;next.variant[0][0].idx=20;require_swap=true;
    assert(jce_renderer_reload_shaders_fs(&r,"overlay",pak));
    assert(loads==1&&r.program.idx==20&&r.program_variant[0][0].idx==20);
    assert(destroys[5]==1&&destroys[7]==1&&destroys[20]==0&&live[20]);
    memset(&next,0xff,sizeof next);next.text_sdf.idx=30;next.variant[0][0].idx=30;live[30]=true;
    assert(!jce_renderer_reload_shaders_fs(&r,"broken",pak));
    assert(loads==2&&r.program.idx==20&&live[20]&&destroys[30]==1);
    const JceShaderSet current=renderer_owned_shader_set(&r);
    require_swap=false;jce_shader_set_release_owned(&current);
    assert(destroys[20]==1);
    for(unsigned i=0;i<64;i++)assert(!live[i]);return 0;
}
''')

    def test_custom_program_cache_capacity_hit_failure_and_session_reset(self):
        self._custom_program_cache_probe(False)

    def test_custom_program_cache_old_warning_reset_cannot_pass(self):
        self._custom_program_cache_probe(True)

    def _custom_program_cache_probe(self, remove_warning_reset):
        text = source("engine/src/renderer/jce_pbr_material.c")
        trace = source("engine/src/renderer/jce_pbr_custom_trace_internal.h")
        scope_header = source("engine/src/os/core/jce_trace_scope_internal.h")
        trace_header = source("engine/include/jce/os/core/jce_trace.h")
        def declaration(owner, begin, end):
            first = owner.index(begin)
            return owner[first:owner.index(end, first) + len(end)]
        # Actual private diagnostic types, isolated typed ports: this source
        # fixture owns cache admission/cleanup, not Native trace publication.
        trace_contract = "\n".join((
            declaration(scope_header, "typedef struct JceTraceScope {", "} JceTraceScope;"),
            declaration(trace_header, "typedef enum JceTraceTaskState {", "} JceTraceTaskState;"),
            declaration(trace, "enum { PBR_CUSTOM_ROUTE_NONE", "} JcePbrCustomBlobTrace;"),
            r'''
static JceTraceScope jce_trace_scope_begin(const char *name){(void)name;return (JceTraceScope){0};}
static void jce_trace_scope_end(JceTraceScope scope,JceTraceTaskState state){(void)scope;(void)state;}
static void pbr_custom_request_trace(JceTraceScope scope,const char *mat,const char *vs,const char *fs){
    (void)scope;(void)mat;(void)vs;(void)fs;
}
static void pbr_custom_blob_trace_publish(JceTraceScope scope,const JcePbrCustomBlobTrace *detail,
    const char *requested,const char *resolved){(void)scope;(void)detail;(void)requested;(void)resolved;}
static void pbr_custom_program_trace_publish(JceTraceScope scope,bool attempted,bool valid,bool admitted,int reason){
    (void)scope;(void)attempted;(void)valid;(void)admitted;(void)reason;
}
'''))
        declarations = text[text.index("static struct {"):text.index("/* Global view mode")]
        declarations += "static float s_view_mode;"
        cache = text[text.index("#define PBR_PROG_CACHE_MAX"):text.index("/* Load + link a custom shader program")]
        load = function(text, "static uint16_t load_custom_program(")
        shutdown = function(text, "void jce_pbr_material_shutdown(void)")
        witness = "PBR cache warning epoch reset missing"
        if remove_warning_reset:
            marker = "s_prog_cache.full_warned = false;"
            self.assertEqual(shutdown.count(marker), 1)
            shutdown = shutdown.replace(marker, "(void)0;", 1)
        execute(self, r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
''' + trace_contract + r'''
typedef struct {uint16_t idx;} bgfx_uniform_handle_t;
typedef bgfx_uniform_handle_t bgfx_texture_handle_t;
typedef bgfx_uniform_handle_t JceShaderHandle;
typedef struct JceFileSystem JceFileSystem;
#define BGFX_HANDLE_IS_VALID(h) ((h).idx!=UINT16_MAX)
#define JCE_FREE free
#define LOG_DEBUG(...) ((void)0)
#define LOG_WARN(...) (++warnings)
static unsigned reads, links, destroys, warnings, next_handle=1;
static bool live[192], fail_link;
static void safe_copy(char *dst,size_t size,const char *src){snprintf(dst,size,"%s",src);}
static void custom_blob_path(const JceFileSystem *fs,const char *mat,const char *rel,char *out,size_t size){
    (void)fs;(void)mat;safe_copy(out,size,rel);
}
static void *custom_blob_read(const JceFileSystem *fs,const char *path,uint64_t *size,JcePbrCustomBlobTrace *detail){
    (void)fs;(void)detail;assert(path&&*path);reads++;*size=4;void *p=malloc(4);assert(p);return p;
}
static JceShaderHandle jce_renderer_create_program_from_blobs(const void *vs,size_t vn,const void *fs,size_t fn){
    assert(vs&&fs&&vn==4&&fn==4);links++;JceShaderHandle h={UINT16_MAX};
    if(!fail_link){h.idx=(uint16_t)next_handle++;assert(h.idx<192);live[h.idx]=true;}return h;
}
static void jce_renderer_destroy_program(JceShaderHandle h){
    assert(h.idx<192&&live[h.idx]);live[h.idx]=false;destroys++;
}
static void bgfx_destroy_uniform(bgfx_uniform_handle_t h){(void)h;assert(!"uninitialized binder has no uniform owner");}
static void bgfx_destroy_texture(bgfx_texture_handle_t h){(void)h;assert(!"uninitialized binder has no texture owner");}
''' + declarations + '\n' + cache + load + shutdown + r'''
int main(void){
    char path[64];uint16_t first=UINT16_MAX;
    for(int i=0;i<64;i++){
        snprintf(path,sizeof path,"materials/%d.mat.json",i);
        uint16_t h=load_custom_program(NULL,path,"vs.bin","fs.bin");
        assert(h!=UINT16_MAX);if(i==0)first=h;
    }
    assert(s_prog_cache.count==64&&links==64&&reads==128);
    for(int i=0;i<3;i++){
        snprintf(path,sizeof path,"materials/overflow-%d.mat.json",i);
        assert(load_custom_program(NULL,path,"vs.bin","fs.bin")==UINT16_MAX);
    }
    assert(links==64&&reads==128&&warnings==1);
    assert(load_custom_program(NULL,"materials/0.mat.json","vs.bin","fs.bin")==first);
    assert(links==64&&reads==128&&warnings==1);
    jce_pbr_material_shutdown();assert(destroys==64&&s_prog_cache.count==0);
    assert(!s_prog_cache.full_warned && "PBR cache warning epoch reset missing");
    jce_pbr_material_shutdown();assert(destroys==64);
    /* Fresh cache lifetime: refill and emit exactly one further budget warning
     * before retiring every new program once. */
    for(int i=0;i<64;i++){
        snprintf(path,sizeof path,"materials/new-session-%d.mat.json",i);
        uint16_t h=load_custom_program(NULL,path,"vs.bin","fs.bin");
        assert(h!=UINT16_MAX);
    }
    assert(links==128&&reads==256&&warnings==1);
    assert(load_custom_program(NULL,"materials/new-overflow.mat.json","vs.bin","fs.bin")==UINT16_MAX);
    assert(load_custom_program(NULL,"materials/new-overflow2.mat.json","vs.bin","fs.bin")==UINT16_MAX);
    assert(links==128&&reads==256&&warnings==2);
    jce_pbr_material_shutdown();assert(destroys==128&&s_prog_cache.count==0);
    assert(!s_prog_cache.full_warned && "PBR cache warning epoch reset missing");
    fail_link=true;
    assert(load_custom_program(NULL,"materials/fail.mat.json","vs.bin","fs.bin")==UINT16_MAX);
    assert(links==129&&reads==258&&s_prog_cache.count==1);
    assert(load_custom_program(NULL,"materials/fail.mat.json","vs.bin","fs.bin")==UINT16_MAX);
    assert(links==129&&reads==258);jce_pbr_material_shutdown();assert(destroys==128);
    jce_pbr_material_shutdown();assert(destroys==128);
    for(unsigned i=0;i<192;i++)assert(!live[i]);return 0;
}
''', expected_runtime_failure=witness if remove_warning_reset else None)

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
