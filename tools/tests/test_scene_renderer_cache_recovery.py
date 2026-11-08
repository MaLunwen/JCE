"""Actual model/grass cache admission and reset bodies with counted CPU ports.

The standalone native diagnostics fixture separately consumes the actual core
logger and trace. These exact-body controls prove production wires/retry policy,
not glTF decoding, compiled shader pixels or GPU completion.
"""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


def load(filename, name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools/tests" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


function = load("test_material_owned_cleanup.py", "cache_recovery_extract").function
execute = load("test_host_font_selection.py", "cache_recovery_compile").execute_stock

COMMON = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define LOG_TAG "controlled-cache-owner"
static unsigned warnings, failures, successes, counters;
static char last_warning[1536];
static bool capture=true;
static uint32_t capture_generation=1;
static void diagnostic(const char *tag,const char *format,...) {
    (void)tag; va_list args; va_start(args,format);
    vsnprintf(last_warning,sizeof last_warning,format,args); va_end(args); ++warnings;
}
#define LOG_WARN diagnostic
#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define JCE_TRACE_CATEGORY_RENDER 5
#define JCE_TRACE_TASK_FAILED 1
#define JCE_TRACE_TASK_SUCCEEDED 0
struct JceTraceScope {uint64_t id;};
static bool jce_trace_enabled(void) {return capture;}
static uint32_t jce_trace_capture_generation(void) {return capture_generation;}
static JceTraceScope jce_trace_scope_begin(const char *name) {(void)name;return {capture?1u:0u};}
static void jce_trace_scope_end(JceTraceScope scope,unsigned state) {
    if(scope.id) {if(state==JCE_TRACE_TASK_FAILED) ++failures;else ++successes;}
}
static void jce_trace_counter_scoped_internal(unsigned category,const char *name,
    int64_t value,uint64_t id,uint64_t parent) {
    (void)category;(void)name;(void)value;if(id&&id==parent)++counters;
}
/* Typed observation only; the dedicated native fixture owns real-ring evidence. */
static void jce_trace_counter_ring_internal(unsigned category,const char *name,
    int64_t value,uint64_t id,uint64_t parent) {
    jce_trace_counter_scoped_internal(category,name,value,id,parent);
}
static uint32_t jce_fnv1a32_str(const char *text) {
    uint32_t hash=2166136261u;while(*text)hash=(hash^(unsigned char)*text++)*16777619u;return hash;
}
#define REQUIRE(value_) do {if(!(value_)){fprintf(stderr,"cache recovery failed %d: %s\n",__LINE__,#value_);return 4;}}while(0)
"""

MODEL_PORTS = r"""
struct JceModel {int unused;};struct JceModelCpu {int unused;};
struct JcePakArchive {int unused;};struct JceRenderer {int unused;};
struct JceStaticBatchMember {int unused;};
struct JceAsyncTask {bool terminal;unsigned state;};
static JceModel model;static JceModelCpu cpu;static JceAsyncTask task;
struct JceSceneRenderer;
#include "__DIAGNOSTIC_HEADER__"
#include "__MODEL_DEPS_HEADER__"
__CACHE_LAYOUT__
__CACHE_CONSTANTS__
struct JceSceneRenderer {
    __CACHE_STATE__
    bool has_cbs;const JcePakArchive *pak;JceRenderer *renderer;uint64_t model_frame;
    struct {JceModel *(*load_model)(const char *,void *);void *userdata;} cbs;
};
/* Bounds/animation are independent owners. This cache fixture keeps the
 * feature disabled; the typed ports reject accidental admission. */
static void sr_model_bounds_note(JceSceneRenderer *sr,const char *path,JceModel *ready) {
    (void)path;(void)ready;if(sr->model_bounds_enabled) abort();
}
static void sr_model_bounds_reset(JceSceneRenderer *sr) {if(sr->model_bounds_enabled) abort();}
static void sr_anim_instances_destroy(JceSceneRenderer *sr) {if(sr->model_bounds_enabled) abort();}
static unsigned callback_loads,allocations,job_frees,submits,releases,model_destroys;
static unsigned current_frame=1;static bool restore_missing,fail_allocation,fail_submit,fail_upload;
static JceModel *load_model(const char *path,void *user) {
    (void)user;++callback_loads;
    return strstr(path,"missing")&&!restore_missing?NULL:&model;
}
static void *allocate(size_t count) {++allocations;return fail_allocation?NULL:malloc(count);}
static void release(void *pointer) {if(pointer){++job_frees;free(pointer);}}
#define JCE_MALLOC allocate
#define JCE_CALLOC calloc
#define JCE_FREE release
static void jce_model_gltf_cpu_free(JceModelCpu *value) {(void)value;}
static void jce_model_destroy(JceModel *value) {(void)value;++model_destroys;}
static uint64_t jce_model_gpu_bytes(JceModel *value) {(void)value;return 42;}
static JceModel *jce_model_upload_gltf_cpu(JceModelCpu *value) {(void)value;return fail_upload?NULL:&model;}
static uint32_t jce_renderer_get_frame_index(JceRenderer *renderer) {(void)renderer;return current_frame;}
static bool jce_async_task_is_terminal(JceAsyncTask *value) {return value->terminal;}
static unsigned jce_async_task_state(JceAsyncTask *value) {return value->state;}
static void jce_async_task_release(JceAsyncTask *value) {(void)value;++releases;}
static void sr_async_task_cancel_and_release(JceAsyncTask **value) {if(*value){++releases;*value=NULL;}}
#define JCE_ASYNC_STATE_SUCCEEDED 1
#define JCE_ASYNC_PRIORITY_NORMAL 1
static int sr_model_worker(void *,void *) {return 0;}
struct JceAsyncTaskDesc {int (*work)(void *,void *);void *user_data;const char *debug_name;unsigned priority;};
static void jce_async_task_desc_init(JceAsyncTaskDesc *desc) {memset(desc,0,sizeof *desc);}
static void *jce_async_default_executor(void) {return NULL;}
static JceAsyncTask *jce_async_submit(void *,const JceAsyncTaskDesc *) {++submits;return fail_submit?NULL:&task;}
static void sr_probe_batch_members(JceSceneRenderer *,SrModelCache *entry,const char *) {entry->batch_probed=true;}
"""

MODEL_CONTROL = r"""
int main(void) {
    JceSceneRenderer sr={};sr.has_cbs=true;sr.cbs.load_model=load_model;
    char path[256],occupancy[96],counts[96];
    const unsigned loaded_count=128;
    SrModelCache *first=NULL;
    for(unsigned i=0;i<SR_MODEL_CACHE_MAX;++i) {
        snprintf(path,sizeof path,"%s-%u.glb",i<loaded_count?"loaded":"missing",i);
        SrModelCache *entry=sr_get_model(&sr,path,i);
        REQUIRE((i<loaded_count)==(entry!=NULL));
        if(i==0) {
            first=entry;
            REQUIRE(sr.model_cache_capacity==SR_MODEL_CACHE_INITIAL);
        }
    }
    REQUIRE(sr.model_cache_capacity==SR_MODEL_CACHE_MAX&&sr.model_cache_used==SR_MODEL_CACHE_MAX);
    REQUIRE(callback_loads==SR_MODEL_CACHE_MAX&&warnings==1&&
        jce_scene_renderer_model_cache_count(&sr)==loaded_count);
    unsigned before=callback_loads;
    REQUIRE(sr_get_model(&sr,"loaded-0.glb",5)==first&&callback_loads==before);
    REQUIRE(sr_get_model(&sr,"new-model.glb",500)==NULL);
    snprintf(occupancy,sizeof occupancy,"occupied=%u limit=%u",SR_MODEL_CACHE_MAX,SR_MODEL_CACHE_MAX);
    snprintf(counts,sizeof counts,"loaded=%u pending=0 failed=%u",
        loaded_count,SR_MODEL_CACHE_MAX-loaded_count);
    REQUIRE(warnings==2&&strstr(last_warning,"first_reason=entry_budget")&&
        strstr(last_warning,occupancy)&&strstr(last_warning,counts));
    REQUIRE(sr_get_model(&sr,"new-model.glb",501)==NULL&&warnings==2&&callback_loads==before);
    REQUIRE(sr_get_model(&sr,"loaded-0.glb",5)!=NULL&&callback_loads==before);
    jce_scene_renderer_invalidate_model_cache(&sr);
    REQUIRE(sr.model_cache_generation==1&&!sr.model_lookup_hint&&!sr.model_upload_frame_valid&&
        !sr.model_cache&&!sr.model_cache_capacity&&!sr.model_cache_used&&
        jce_scene_renderer_model_cache_count(&sr)==0);
    restore_missing=true;
    REQUIRE(sr_get_model(&sr,"missing-128.glb",42)!=NULL&&callback_loads==before+1);
    REQUIRE(sr_get_model(&sr,"missing-128.glb",43)!=NULL&&callback_loads==before+1);
    restore_missing=false;
    REQUIRE(sr_get_model(&sr,"missing-again.glb",44)==NULL&&warnings==3);
    REQUIRE(strstr(last_warning,"generation=1"));
    jce_scene_renderer_invalidate_model_cache(&sr);
    unsigned old_warnings=warnings;
    sr.has_cbs=false;sr.pak=NULL;
    REQUIRE(sr_get_model(&sr,"archive-model.glb",60)==NULL&&warnings==old_warnings+1);
    REQUIRE(sr_get_model(&sr,"archive-model.glb",61)==NULL&&warnings==old_warnings+1);
    JceSceneRenderer other={};
    REQUIRE(sr_get_model(&other,"archive-model.glb",62)==NULL&&warnings==old_warnings+2);
    sr.pak=(JcePakArchive *)&sr;
    memset(path,'x',sizeof(path)-1);path[sizeof(path)-1]=0;
    char too_long[257];memset(too_long,'x',sizeof(too_long)-1);too_long[sizeof(too_long)-1]=0;
    before=submits;
    REQUIRE(sr_get_model(&sr,too_long,99)==NULL&&submits==before&&strstr(last_warning,"path_length"));
    fail_allocation=true;
    REQUIRE(sr_get_model(&sr,"runtime.glb",70)==NULL&&strstr(last_warning,"job_allocation"));
    fail_allocation=false;fail_submit=true;
    REQUIRE(sr_get_model(&sr,"runtime.glb",71)==NULL&&strstr(last_warning,"task_admission"));
    fail_submit=false;
    REQUIRE(sr_get_model(&sr,"runtime.glb",72)==NULL&&sr.model_inflight==1);
    SrModelCache *pending=sr_get_model(&sr,"runtime.glb",73);
    REQUIRE(pending&&pending->pending&&!pending->model&&pending->request_entity==72);
    task.terminal=true;task.state=0;
    sr_model_poll(&sr);
    REQUIRE(sr.model_inflight==0&&pending->failed&&!pending->pending&&
        pending->failure_reason==SR_MODEL_FAILURE_DECODE&&strstr(last_warning,"decode_failed"));
    before=submits;
    REQUIRE(sr_get_model(&sr,"runtime.glb",74)==NULL&&submits==before);
    jce_scene_renderer_invalidate_model_cache(&sr);++current_frame;
    REQUIRE(sr_get_model(&sr,"runtime.glb",75)==NULL&&sr.model_inflight==1);
    pending=sr_get_model(&sr,"runtime.glb",76);pending->job->cpu=&cpu;
    task.state=JCE_ASYNC_STATE_SUCCEEDED;fail_upload=true;
    sr_model_poll(&sr);
    REQUIRE(pending->failed&&pending->failure_reason==SR_MODEL_FAILURE_UPLOAD&&strstr(last_warning,"upload_failed"));
    jce_scene_renderer_invalidate_model_cache(&sr);++current_frame;fail_upload=false;
    REQUIRE(sr_get_model(&sr,"runtime.glb",77)==NULL);
    pending=sr_get_model(&sr,"runtime.glb",78);pending->job->cpu=&cpu;
    sr_model_poll(&sr);
    REQUIRE(!pending->failed&&pending->model==&model&&pending->vram_bytes==42);
    REQUIRE(jce_scene_renderer_model_cache_count(&sr)==1);
    jce_scene_renderer_invalidate_model_cache(&sr);
    REQUIRE(sr.model_inflight==0&&failures>0&&counters>0);
    puts("Model cache production wire PASS");return 0;
}
"""


def model_probe(mutation=None):
    main = (ROOT / "engine/src/middleware/scene/jce_scene_renderer.c").read_text(encoding="utf-8")
    types = (ROOT / "engine/src/middleware/scene/jce_sr_types.h").read_text(encoding="utf-8")
    state = (ROOT / "engine/src/middleware/scene/jce_sr_internal.h").read_text(encoding="utf-8")
    diagnostic = (ROOT / "engine/src/middleware/scene/jce_sr_model_diagnostics.c").read_text(encoding="utf-8")
    cache_end = types.index("} SrModelCache;") + len("} SrModelCache;")
    cache = types[types.rfind("typedef struct", 0, cache_end):cache_end]
    constants = "\n".join(line for line in types.splitlines() if line.startswith((
        "#define SR_MODEL_CACHE_INITIAL", "#define SR_MODEL_CACHE_MAX",
        "#define SR_MODEL_MAX_INFLIGHT", "#define SR_MODEL_UPLOADS_PER_FRAME")))
    start = state.index("    SrModelCache           **model_cache;")
    end = state.index("    SrAnimStorage", start)
    ports = MODEL_PORTS.replace("__DIAGNOSTIC_HEADER__", (ROOT /
        "engine/src/middleware/scene/jce_sr_model_diagnostics.h").as_posix())
    ports = ports.replace("__MODEL_DEPS_HEADER__", (ROOT /
        "engine/src/middleware/scene/jce_sr_model_deps.h").as_posix())
    ports = ports.replace("__CACHE_LAYOUT__", cache).replace("__CACHE_CONSTANTS__", constants)
    ports = ports.replace("__CACHE_STATE__", state[start:end])
    # Keep the complete diagnostic algorithm and its macros/helpers. Only its
    # includes are supplied by the real private declaration and typed CPU ports.
    owners = diagnostic
    for header in (
        "jce_sr_internal.h",
        "jce_sr_model_diagnostics.h",
        "os/core/jce_trace_scope_internal.h",
        "os/core/jce_trace_capture_internal.h",
    ):
        include = f'#include "{header}"'
        if owners.count(include) != 1:
            raise AssertionError("Expected one actual model diagnostic dependency")
        owners = owners.replace(include, "", 1)
    table = (ROOT / "engine/src/middleware/scene/jce_sr_model_table.h").read_text(encoding="utf-8")
    # Use the real growth/rehash algorithm: a table allocation moves pointers,
    # while every returned entry remains at its own stable heap address.
    owners += "\n" + function(table, "static bool sr_model_table_prepare_new_key(")
    owners += "\n" + function(main, "struct SrModelJob {") + ";\n"
    for signature in ("static void sr_model_job_destroy(", "static void sr_model_poll(",
        "static uint32_t sr_path_hash(", "SrModelCache *sr_get_model(",
        "uint32_t jce_scene_renderer_model_cache_count(", "void jce_scene_renderer_invalidate_model_cache("):
        owners += "\n" + function(main, signature)
    if mutation == "full":
        # Full tables refuse in prepare_new_key before the defensive free-slot
        # fallback. Remove only the diagnostic on that actual admission path.
        wire = "sr_model_cache_failure_hash(sr, h, entity_id, reason);"
    elif mutation == "reset":
        wire = "sr_model_cache_diagnostics_reset(sr);"
    else:
        wire = None
    if wire:
        if owners.count(wire) != 1:
            raise AssertionError("Expected one actual model diagnostic wire")
        owners = owners.replace(wire, "(void)0;", 1)
    return COMMON + ports + owners + MODEL_CONTROL

GRASS_PORTS = r"""
#include <jce/os/core/jce_math.h>
struct Handle {uint16_t idx;};
typedef Handle bgfx_program_handle_t;typedef Handle bgfx_uniform_handle_t;
typedef Handle JceShaderHandle;typedef uint64_t JceEntity;
struct JceMesh {int unused;};struct JcePakArchive {int unused;};
struct JceScene;
#define BGFX_HANDLE_IS_VALID(handle_) ((handle_).idx!=UINT16_MAX)
#define BGFX_UNIFORM_TYPE_VEC4 1
#define JCE_CSM_MAX_CASCADES 4
#define SR_WATER_SLOT_MAX 16
#define JCE_FREE free
struct Slot {bool used;};
struct Cluster {bool used;float *inst;JceMesh *expanded_mesh;};
struct Meshlet {bool used;Handle indirect,shadow_indirect[JCE_CSM_MAX_CASCADES];};
struct World {int unused;};
#include "__PREPASS_HEADER__"
#include "__SSGI_HEADER__"
struct JceSsgi;
__PREV_LAYOUT__
struct JceSceneRenderer {
    __SSGI_STATE__
    __PREV_STATE__
    const JcePakArchive *pak;
    JceMesh *grass_blade;Handle prog_grass,prog_grass_shadow;
    bool grass_prog_tried,grass_shadow_prog_tried;
    bool decals_authored_tried; /* Matches the actual reset owner's private latch. */
    Handle u_grass_time,u_grass_wind,u_grass_fade,u_grass_camera,u_grass_color;
    uint32_t grass_gate_reported_reasons;uint64_t grass_resource_generation;
    Slot foliage_cache[16],grass_cache[16],water_cache[16];
    Cluster fcluster_cache[48];Meshlet meshlet_cache[16];
    bool resource_prepared_valid; /* Real reset owner invalidates its preparation key. */
    bool model_bounds_enabled; /* Bounds stay outside the grass resource fixture. */
    bool ecull_frz_valid,shadow_space_valid,shadow_cache_valid[JCE_CSM_MAX_CASCADES];
    uint32_t eidx_list_gen,wcache_cap,wcache_count,shadow_cache_cascades;World *wcache;
};
static unsigned colour_loads,depth_loads,program_destroys,blade_creates,uniform_creates;
static unsigned prepass_releases;static bool invalid_prepass_reason;
static bool colour_present,depth_present,deferred_fail[32];
static JceMesh blade;static JcePakArchive grass_pak;
static unsigned unexpected_shader_requests;
/* Count only exact typed requests made by the actual colour/depth owners. */
static JceShaderHandle jce_renderer_shader_load_named_internal(
    const JcePakArchive *pak,const char *vs,const char *fs) {
    if(pak!=&grass_pak||!vs||strcmp(vs,"grass")||!fs) {
        ++unexpected_shader_requests;return {UINT16_MAX};
    }
    if(strcmp(fs,"grass")==0) {
        ++colour_loads;return {uint16_t(colour_present?0:UINT16_MAX)};
    }
    if(strcmp(fs,"grass_shadow")==0) {
        ++depth_loads;return {uint16_t(depth_present?1:UINT16_MAX)};
    }
    ++unexpected_shader_requests;return {UINT16_MAX};
}
static uint16_t jce_shader_deferred_resolve(uint16_t handle) {
    return handle==UINT16_MAX||deferred_fail[handle]?UINT16_MAX:handle;
}
static void jce_shader_program_destroy(JceShaderHandle handle) {
    if(handle.idx<32) {deferred_fail[handle.idx]=false;++program_destroys;}
}
static Handle bgfx_create_uniform(const char *,unsigned,unsigned) {++uniform_creates;return {2};}
static JceMesh *sr_grass_build_blade(float,float,int) {++blade_creates;return &blade;}
static bool sr_grass_expanded_mode(void) { return false; }
/* Typed counted dependency: the separate native prepass-owner fixture tests
 * its real allocations and teardown. Keep the full actual grass/reset body. */
static void sr_release_prepass_targets(JceSceneRenderer *,SrPrepassTargetReleaseReason reason) {
    ++prepass_releases;if(reason!=SR_PREPASS_TARGET_RESET)invalid_prepass_reason=true;
}
static void sr_fullscreen_effect_reset_all(JceSceneRenderer *) {}
static void sr_model_bounds_reset(JceSceneRenderer *sr) {if(sr->model_bounds_enabled) abort();}
static void sr_bounds_cache_reset(JceSceneRenderer *) {}
static void sr_foliage_slot_free(JceSceneRenderer *sr,int slot) {sr->foliage_cache[slot].used=false;}
static void sr_grass_slot_free(JceSceneRenderer *sr,int slot) {sr->grass_cache[slot].used=false;}
static void sr_water_slot_free(JceSceneRenderer *sr,int slot) {sr->water_cache[slot].used=false;}
static JceMesh expanded_leaf;
static unsigned expanded_mesh_releases;
static bool invalid_expanded_mesh_release;
static void jce_mesh_destroy(JceMesh *mesh) {
    if(mesh==&expanded_leaf) ++expanded_mesh_releases;
    else invalid_expanded_mesh_release=true;
}
static void bgfx_destroy_indirect_buffer(Handle) {}
static void cold(JceSceneRenderer *sr) {
    memset(sr,0,sizeof *sr);sr->pak=&grass_pak;
    sr->prog_grass.idx=sr->prog_grass_shadow.idx=UINT16_MAX;
    sr->u_grass_time.idx=sr->u_grass_wind.idx=sr->u_grass_fade.idx=UINT16_MAX;
    sr->u_grass_camera.idx=sr->u_grass_color.idx=UINT16_MAX;
}
"""

GRASS_CONTROL = r"""
int main(void) {
    JceSceneRenderer sr; cold(&sr);
    int ssgi_token=0;
    JceSsgi *retained_ssgi=(JceSsgi *)&ssgi_token;
    sr.ssgi=retained_ssgi;sr.ssgi_active_frame=true;sr.ssgi_result_idx=0;
    sr.ssgi_idle={91u,119u,true,true};
    SrPrevXform history[2]={};
    history[0].entity=71;history[0].used=history[0].touched=history[0].prev_valid=true;
    history[0].current_world.raw[3][0]=4;history[1].entity=89;history[1].used=true;
    sr.prev_xform=history;sr.prev_xform_cap=2;sr.prev_xform_count=2;
    sr.prev_xform_scene=(const JceScene *)&sr;sr.prev_xform_gen_valid=true;
    sr.decals_authored_tried=true;
    sr.fcluster_cache[0].used=true;
    sr.fcluster_cache[0].expanded_mesh=&expanded_leaf;
    REQUIRE(!sr_grass_program_ready(&sr,900,true)&&colour_loads==1&&depth_loads==0&&warnings==1);
    REQUIRE(!sr_grass_program_ready(&sr,901,true)&&colour_loads==1&&warnings==1);
    REQUIRE(strstr(last_warning,"entity=900 generation=0"));
    jce_scene_renderer_reset_entity_caches(&sr);
    REQUIRE(sr.grass_resource_generation==1&&!sr.grass_prog_tried&&!sr.decals_authored_tried);
    REQUIRE(sr.ssgi==retained_ssgi&&!sr.ssgi_active_frame&&sr.ssgi_result_idx==UINT16_MAX&&
        !sr.ssgi_idle.valid&&!sr.ssgi_idle.wanted&&
        sr.ssgi_idle.frame==0&&sr.ssgi_idle.frames==0);
    REQUIRE(sr.prev_xform==history&&sr.prev_xform_cap==2&&sr.prev_xform_count==0&&
        !sr.prev_xform_scene&&!sr.prev_xform_gen_valid);
    unsigned char cleared_history[sizeof history]={};
    REQUIRE(memcmp(history,cleared_history,sizeof history)==0);
    REQUIRE(!sr_grass_program_ready(&sr,902,true)&&colour_loads==2&&warnings==2);
    REQUIRE(!sr_grass_program_ready(&sr,903,true)&&colour_loads==2&&warnings==2);
    colour_present=true;
    REQUIRE(!sr_grass_program_ready(&sr,904,true)&&colour_loads==2);
    jce_scene_renderer_reset_entity_caches(&sr);
    REQUIRE(!sr_grass_program_ready(&sr,905,true)&&colour_loads==3&&depth_loads==1&&warnings==3);
    REQUIRE(sr_grass_program_ready(&sr,906,false)&&colour_loads==3&&depth_loads==1);
    REQUIRE(!sr_grass_program_ready(&sr,907,true)&&depth_loads==1&&warnings==3);
    REQUIRE(strstr(last_warning,"entity=905 generation=2"));
    depth_present=true;
    REQUIRE(!sr_grass_program_ready(&sr,908,true)&&depth_loads==1);
    jce_scene_renderer_reset_entity_caches(&sr);
    REQUIRE(sr_grass_program_ready(&sr,909,true)&&colour_loads==3&&depth_loads==2);
    REQUIRE(sr.prog_grass.idx==0&&sr.prog_grass_shadow.idx==1&&program_destroys==0);
    jce_scene_renderer_reset_entity_caches(&sr);
    REQUIRE(sr_grass_program_ready(&sr,910,true)&&colour_loads==3&&depth_loads==2&&program_destroys==0);
    sr.prog_grass.idx=16;deferred_fail[16]=true;
    REQUIRE(!sr_grass_program_ready(&sr,911,true)&&colour_loads==3);
    jce_scene_renderer_reset_entity_caches(&sr);
    REQUIRE(program_destroys==1&&!sr.grass_prog_tried&&sr.grass_shadow_prog_tried);
    REQUIRE(sr_grass_program_ready(&sr,912,true)&&colour_loads==4&&depth_loads==2);
    sr.prog_grass_shadow.idx=17;deferred_fail[17]=true;
    REQUIRE(!sr_grass_program_ready(&sr,913,true));
    jce_scene_renderer_reset_entity_caches(&sr);
    REQUIRE(program_destroys==2&&sr.grass_prog_tried&&!sr.grass_shadow_prog_tried);
    REQUIRE(sr_grass_program_ready(&sr,914,true)&&colour_loads==4&&depth_loads==3);
    unsigned prior=warnings;colour_present=false;
    JceSceneRenderer other;cold(&other);
    REQUIRE(!sr_grass_program_ready(&other,915,true)&&warnings==prior+1&&other.grass_resource_generation==0);
    REQUIRE(sr.grass_resource_generation==6&&blade_creates==2&&uniform_creates==10);
    REQUIRE(prepass_releases==6&&!invalid_prepass_reason);
    REQUIRE(expanded_mesh_releases==1&&!invalid_expanded_mesh_release);
    REQUIRE(!sr.fcluster_cache[0].used&&sr.fcluster_cache[0].expanded_mesh==nullptr);
    REQUIRE(sr.ssgi==retained_ssgi&&!sr.ssgi_active_frame&&sr.ssgi_result_idx==UINT16_MAX);
    REQUIRE(unexpected_shader_requests==0);
    REQUIRE(failures>0&&successes>0&&counters>0);
    puts("Grass program production reset PASS");return 0;
}
"""


def grass_probe(mutation=None):
    environment = (ROOT / "engine/src/middleware/scene/jce_sr_environment.c").read_text(encoding="utf-8")
    prepare = (ROOT / "engine/src/middleware/scene/jce_sr_grass_prepare.c").read_text(encoding="utf-8")
    history = (ROOT / "engine/src/middleware/scene/jce_sr_velocity_world.c").read_text(encoding="utf-8")
    ssgi = (ROOT / "engine/src/middleware/scene/jce_sr_ssgi_lifetime.c").read_text(encoding="utf-8")
    types = (ROOT / "engine/src/middleware/scene/jce_sr_types.h").read_text(encoding="utf-8")
    state = (ROOT / "engine/src/middleware/scene/jce_sr_internal.h").read_text(encoding="utf-8")
    history_end = types.index("} SrPrevXform;") + len("} SrPrevXform;")
    layout = types[types.rfind("typedef struct", 0, history_end):history_end]
    state_start = state.index("    SrPrevXform               *prev_xform;")
    state_end = state.index("    bool                       kindcache_on;", state_start)
    ssgi_start = state.index("    JceSsgi                   *ssgi;")
    ssgi_end = state.index("    bool                       ssr_active_frame;", ssgi_start)
    # Keep both dependencies' complete actual reset bodies and typed state.
    # Their native owner fixtures retain frame/retirement and world-history scope.
    owners = function(ssgi, "void sr_ssgi_reset(")
    # The shared native probe is C++20; adapt only this C99 aggregate-literal
    # spelling, retaining the complete reset body and actual private type.
    literal = "(SrSsgiIdle){0}"
    if owners.count(literal) != 1:
        raise AssertionError("Expected one actual SSGI zero-state assignment")
    owners = owners.replace(literal, "SrSsgiIdle{0}", 1)
    owners += "\n" + function(history, "void sr_prev_xform_reset(")
    owners += "\n" + function(environment, "void sr_grass_lazy_init(")
    for signature in ("void sr_grass_reset_failed_programs(", "static bool sr_grass_program_result(",
        "static bool sr_grass_program_ready("):
        owners += "\n" + function(prepare, signature)
    owners += "\n" + function(environment, "void jce_scene_renderer_reset_entity_caches(")
    if mutation == "prepass":
        wire = "sr_release_prepass_targets(sr, SR_PREPASS_TARGET_RESET);"
    elif mutation == "ssgi_reset":
        wire = "sr_ssgi_reset(sr);"
    elif mutation == "reset":
        wire = "sr_grass_reset_failed_programs(sr);"
    elif mutation == "colour":
        wire = "bool *tried[2] = {&sr->grass_prog_tried, &sr->grass_shadow_prog_tried};"
    elif mutation == "depth":
        wire = "bool *tried[2] = {&sr->grass_prog_tried, &sr->grass_shadow_prog_tried};"
    else:
        wire = None
    if wire:
        if owners.count(wire) != 1:
            raise AssertionError("Expected one actual grass retry wire")
        replacement = "(void)0;" if mutation in ("reset", "prepass", "ssgi_reset") else (
            "bool dummy = true; bool *tried[2] = {&dummy, &sr->grass_shadow_prog_tried};" if mutation == "colour"
            else "bool dummy = true; bool *tried[2] = {&sr->grass_prog_tried, &dummy};")
        owners = owners.replace(wire, replacement, 1)
    ports = GRASS_PORTS.replace("__PREPASS_HEADER__", (ROOT /
        "engine/src/middleware/scene/jce_sr_prepass_targets.h").as_posix())
    ports = ports.replace("__SSGI_HEADER__", (ROOT /
        "engine/src/middleware/scene/jce_sr_ssgi_lifetime.h").as_posix())
    ports = ports.replace("__SSGI_STATE__", state[ssgi_start:ssgi_end])
    ports = ports.replace("__PREV_LAYOUT__", layout).replace("__PREV_STATE__", state[state_start:state_end])
    return COMMON + ports + owners + GRASS_CONTROL


class SceneRendererCacheRecoveryTests(unittest.TestCase):
    def test_model_cache_full_hits_negative_entries_async_failures_and_reset(self):
        self.assertIn("Model cache production wire PASS", execute(self, model_probe()))

    def test_model_missing_capacity_diagnostic_wire_fails_normally(self):
        execute(self, model_probe("full"), negative=True, negative_exit=4,
            negative_marker="cache recovery failed ")

    def test_model_missing_reset_wire_fails_normally(self):
        execute(self, model_probe("reset"), negative=True, negative_exit=4,
            negative_marker="cache recovery failed ")

    def test_grass_missing_to_present_colour_depth_and_retained_success(self):
        self.assertIn("Grass program production reset PASS", execute(self, grass_probe()))

    def test_grass_missing_actual_reset_wire_fails_normally(self):
        execute(self, grass_probe("reset"), negative=True, negative_exit=4,
            negative_marker="cache recovery failed ")

    def test_grass_reset_retains_actual_prepass_owner_wire(self):
        execute(self, grass_probe("prepass"), negative=True, negative_exit=4,
            negative_marker="cache recovery failed ")

    def test_grass_reset_retains_actual_ssgi_state_reset_wire(self):
        execute(self, grass_probe("ssgi_reset"), negative=True, negative_exit=4,
            negative_marker="cache recovery failed ")

    def test_grass_missing_colour_rearm_fails_normally(self):
        execute(self, grass_probe("colour"), negative=True, negative_exit=4,
            negative_marker="cache recovery failed ")

    def test_grass_missing_depth_rearm_fails_normally(self):
        execute(self, grass_probe("depth"), negative=True, negative_exit=4,
            negative_marker="cache recovery failed ")


if __name__ == "__main__":
    unittest.main()
