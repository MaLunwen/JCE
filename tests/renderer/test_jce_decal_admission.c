/* Complete actual pool create/destroy and scene drive/reset CPU owners. */
#include "unity.h"
#include <jce/renderer/jce_decals.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/os/core/jce_log.h>
#include "renderer/jce_renderer_shader_profile_internal.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static struct {
    JceRendererApiInfo api;
    bgfx_renderer_type_t backend;
    unsigned attempts, api_queries, backend_queries, suffix_queries, loads;
    unsigned allocations, frees, live, fail_allocation, fail_shader;
    unsigned links, samplers, shader_destroys, program_destroys, sampler_destroys;
    unsigned clears, walks, updates, renders, warnings, errors, resets;
    bool fail_program, fail_sampler;
    void *memory[4];
    JceDecalPool *active;
} port;
static JceRendererApiInfo observed_api(void) { ++port.api_queries; return port.api; }
static bgfx_renderer_type_t observed_backend(void) { ++port.backend_queries; return port.backend; }
static const char *backend_suffix(void) { ++port.suffix_queries; return "glsl"; }
static void *counted_calloc(size_t count, size_t size)
{
    const unsigned call = ++port.allocations;
    if (call == port.fail_allocation) return NULL;
    void *p = calloc(count, size); TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_LESS_THAN_UINT(4, port.live); port.memory[port.live++] = p;
    return p;
}
static void counted_free(void *p)
{
    if (!p) return;
    unsigned i = 0; while (i < port.live && port.memory[i] != p) ++i;
    TEST_ASSERT_LESS_THAN_UINT(port.live, i);
    port.memory[i] = port.memory[--port.live]; ++port.frees; free(p);
}
static void counted_log(JceLogLevel level, const char *tag, const char *file,
    int line, const char *format, ...)
{
    (void)tag; (void)file; (void)line; (void)format;
    if (level == JCE_LOG_LEVEL_WARN) ++port.warnings;
    if (level == JCE_LOG_LEVEL_ERROR) ++port.errors;
}
static void layout_begin(bgfx_vertex_layout_t *layout, bgfx_renderer_type_t backend)
{ (void)layout; TEST_ASSERT_EQUAL_INT(BGFX_RENDERER_TYPE_NOOP, backend); }
static void layout_add(bgfx_vertex_layout_t *layout, bgfx_attrib_t attr,
    uint8_t num, bgfx_attrib_type_t type, bool normalized, bool integer)
{ (void)layout; (void)attr; (void)num; (void)type; (void)normalized; (void)integer; }
static void layout_end(bgfx_vertex_layout_t *layout) { (void)layout; }
static bgfx_shader_handle_t load_shader(const JcePakArchive *pak,
    const char *name, const char *suffix, const char *tag)
{
    const unsigned i = ++port.loads;
    TEST_ASSERT_NOT_NULL(pak); TEST_ASSERT_EQUAL_STRING("glsl", suffix);
    TEST_ASSERT_EQUAL_STRING("decals", tag);
    TEST_ASSERT_EQUAL_STRING((i & 1u) ? "vs_decal" : "fs_decal", name);
    bgfx_shader_handle_t handle = {(uint16_t)((i - 1u) & 1u)};
    if (i == port.fail_shader) handle.idx = UINT16_MAX;
    return handle;
}
static bgfx_program_handle_t link_program(bgfx_shader_handle_t vs,
    bgfx_shader_handle_t fs, bool destroy_shaders)
{
    ++port.links; TEST_ASSERT_EQUAL_UINT(0, vs.idx); TEST_ASSERT_EQUAL_UINT(1, fs.idx);
    TEST_ASSERT_TRUE(destroy_shaders);
    bgfx_program_handle_t handle = {port.fail_program ? UINT16_MAX : 0}; return handle;
}
static bgfx_uniform_handle_t create_sampler(const char *name,
    bgfx_uniform_type_t type, uint16_t count)
{
    ++port.samplers; TEST_ASSERT_EQUAL_STRING("s_decal", name);
    TEST_ASSERT_EQUAL_INT(BGFX_UNIFORM_TYPE_SAMPLER, type); TEST_ASSERT_EQUAL_UINT(1, count);
    bgfx_uniform_handle_t handle = {port.fail_sampler ? UINT16_MAX : 0}; return handle;
}
static void destroy_shader(bgfx_shader_handle_t handle)
{ TEST_ASSERT_TRUE(BGFX_HANDLE_IS_VALID(handle)); ++port.shader_destroys; }
static void destroy_program(bgfx_program_handle_t handle)
{ TEST_ASSERT_TRUE(BGFX_HANDLE_IS_VALID(handle)); ++port.program_destroys; }
static void destroy_sampler(bgfx_uniform_handle_t handle)
{ TEST_ASSERT_TRUE(BGFX_HANDLE_IS_VALID(handle)); ++port.sampler_destroys; }
#define jce_renderer_get_api_info observed_api
#define bgfx_get_renderer_type observed_backend
#define jce_shader_backend_suffix backend_suffix
#define jce_shader_load_from_pak load_shader
#define bgfx_vertex_layout_begin layout_begin
#define bgfx_vertex_layout_add layout_add
#define bgfx_vertex_layout_end layout_end
#define bgfx_create_program link_program
#define bgfx_create_uniform create_sampler
#define bgfx_destroy_shader destroy_shader
#define bgfx_destroy_program destroy_program
#define bgfx_destroy_uniform destroy_sampler
#define jce_log_write counted_log
#define JCE_CALLOC counted_calloc
#define JCE_FREE counted_free
#define LOG_TAG "decals"
#define jce_decals_create actual_decal_pool_create
#include JCE_TEST_DECAL_POOL_OWNER
#undef jce_decals_create
static JceDecalPool *scene_create(const JceDecalPoolDesc *desc)
{
    ++port.attempts; JceDecalPool *pool = actual_decal_pool_create(desc);
    if (pool) port.active = pool; return pool;
}
static void release_active(void)
{ if (port.active) { JceDecalPool *pool=port.active; port.active=NULL; jce_decals_destroy(pool); } }

/* Typed syntax/dependency ports for unrelated entity caches and decal culling. */
#define JCE_CSM_MAX_CASCADES 4
#define SR_WATER_SLOT_MAX 1
#define SR_PREPASS_TARGET_RESET 1
struct CacheSlot {
    bool used; void *inst;
    JceMesh *expanded_mesh;
    bgfx_indirect_buffer_handle_t indirect, shadow_indirect[JCE_CSM_MAX_CASCADES];
};
struct CacheWorld { unsigned token; };
typedef struct JceScene { int authored; } JceScene;
typedef uint32_t JceEntity;
typedef struct JceSceneRenderer {
    const JcePakArchive *pak;
    JceDecalPool *decals, *decals_authored;
    bool decals_authored_tried, resource_prepared_valid, ecull_frz_valid;
    struct CacheSlot foliage_cache[1], grass_cache[1], water_cache[1], fcluster_cache[1], meshlet_cache[1];
    unsigned eidx_list_gen, wcache_cap, wcache_count;
    struct CacheWorld *wcache;
    bool shadow_cache_valid[JCE_CSM_MAX_CASCADES], shadow_space_valid;
    unsigned shadow_cache_cascades;
} JceSceneRenderer;
typedef struct {
    JceSceneRenderer *sr; JceScene *scene; uint32_t spawned, candidates;
} SrDecalEachCtx;
static int jce_scene_count_decals(JceScene *scene) { return scene->authored; }
static void sr_decal_each_entity(JceScene *scene, JceEntity entity, void *user)
{
    (void)scene; (void)entity; SrDecalEachCtx *ctx = user;
    ++ctx->candidates; ++ctx->spawned;
}
static void jce_scene_each_decal(JceScene *scene,
    void (*callback)(JceScene *, JceEntity, void *), void *user)
{ ++port.walks; for (int i=0;i<scene->authored;++i) callback(scene,(JceEntity)i,user); }
void jce_decals_clear(JceDecalPool *pool) { TEST_ASSERT_NOT_NULL(pool); ++port.clears; }
void jce_decals_update(JceDecalPool *pool, float dt)
{ TEST_ASSERT_NOT_NULL(pool); TEST_ASSERT_EQUAL_FLOAT(0.016f,dt); ++port.updates; }
void jce_decals_render(JceDecalPool *pool, uint16_t view)
{ TEST_ASSERT_NOT_NULL(pool); TEST_ASSERT_EQUAL_UINT(7,view); ++port.renders; }
static void sr_prev_xform_reset(JceSceneRenderer *sr) { (void)sr; ++port.resets; }
static void sr_ssgi_reset(JceSceneRenderer *sr) { (void)sr; }
static void sr_release_prepass_targets(JceSceneRenderer *sr,int mode) { (void)sr;(void)mode; }
static void sr_fullscreen_effect_reset_all(JceSceneRenderer *sr) { (void)sr; }
static void sr_grass_reset_failed_programs(JceSceneRenderer *sr) { (void)sr; }
static void sr_foliage_slot_free(JceSceneRenderer *sr,int i) { (void)sr;(void)i;TEST_FAIL_MESSAGE("unrelated foliage slot active"); }
static void sr_grass_slot_free(JceSceneRenderer *sr,int i) { (void)sr;(void)i;TEST_FAIL_MESSAGE("unrelated grass slot active"); }
static void sr_water_slot_free(JceSceneRenderer *sr,int i) { (void)sr;(void)i;TEST_FAIL_MESSAGE("unrelated water slot active"); }
static void destroy_indirect(bgfx_indirect_buffer_handle_t handle) { (void)handle; TEST_FAIL_MESSAGE("unrelated indirect active"); }
#define bgfx_destroy_indirect_buffer destroy_indirect
#define jce_decals_create scene_create
#include JCE_TEST_DECAL_SCENE_OWNER
#undef jce_decals_create
static void destroy_mesh(JceMesh *mesh)
{ (void)mesh; TEST_FAIL_MESSAGE("unrelated expanded mesh active"); }
#define jce_mesh_destroy destroy_mesh
#include JCE_TEST_DECAL_RESET_OWNER
#undef jce_mesh_destroy
static unsigned pak_token, runtime_token;
static JceSceneRenderer renderer(void)
{ JceSceneRenderer sr;memset(&sr,0,sizeof sr);sr.pak=(const JcePakArchive *)&pak_token;return sr; }
static void drive(JceSceneRenderer *sr,JceScene *scene) { sr_drive_decals(sr,scene,7,0.016f); }
void setUp(void)
{
    memset(&port,0,sizeof port);port.api.build_tier=JCE_GRAPHICS_API_TIER_STABLE;
    port.api.backend=JCE_BACKEND_OPENGL;port.backend=BGFX_RENDERER_TYPE_OPENGL;
}
void tearDown(void) { release_active(); TEST_ASSERT_EQUAL_UINT(0,port.live); }
static void empty_scene_never_acquires_but_runtime_stamps_keep_updating(void)
{
    JceSceneRenderer sr=renderer();JceScene scene={0};sr.decals=(JceDecalPool *)&runtime_token;
    for(int i=0;i<20;++i)drive(&sr,&scene);
    TEST_ASSERT_EQUAL_UINT(0,port.attempts+port.allocations+port.loads+port.api_queries);
    TEST_ASSERT_EQUAL_UINT(20,port.updates);TEST_ASSERT_EQUAL_UINT(20,port.renders);
    TEST_ASSERT_FALSE(sr.decals_authored_tried);
}
static void authored_modern_pool_is_acquired_once_and_cleared_when_removed(void)
{
    JceSceneRenderer sr=renderer();JceScene scene={2};drive(&sr,&scene);drive(&sr,&scene);
    TEST_ASSERT_EQUAL_UINT(1,port.attempts);TEST_ASSERT_EQUAL_UINT(2,port.allocations);
    TEST_ASSERT_EQUAL_UINT(2,port.loads);TEST_ASSERT_EQUAL_UINT(2,port.walks);
    scene.authored=0;drive(&sr,&scene);TEST_ASSERT_EQUAL_UINT(3,port.clears);
    TEST_ASSERT_EQUAL_UINT(2,port.renders);TEST_ASSERT_EQUAL_UINT(1,port.attempts);
}
static void unsupported_authored_attempt_is_latched_without_blocking_runtime(void)
{
    port.api.build_tier=JCE_GRAPHICS_API_TIER_COMPATIBILITY;
    JceSceneRenderer sr=renderer();JceScene scene={1};sr.decals=(JceDecalPool *)&runtime_token;
    for(int i=0;i<20;++i)drive(&sr,&scene);
    TEST_ASSERT_EQUAL_UINT(1,port.attempts);TEST_ASSERT_EQUAL_UINT(1,port.warnings);
    TEST_ASSERT_EQUAL_UINT(0,port.suffix_queries+port.allocations+port.loads+port.errors);
    TEST_ASSERT_EQUAL_UINT(20,port.updates);TEST_ASSERT_EQUAL_UINT(20,port.renders);
    TEST_ASSERT_NULL(sr.decals_authored);TEST_ASSERT_EQUAL_INT(1,scene.authored);
}
static void allocation_failure_is_latched_and_explicit_reset_allows_one_retry(void)
{
    port.fail_allocation=1;JceSceneRenderer sr=renderer();JceScene scene={1};
    sr.decals=(JceDecalPool *)&runtime_token;drive(&sr,&scene);drive(&sr,&scene);
    TEST_ASSERT_EQUAL_UINT(1,port.attempts);TEST_ASSERT_EQUAL_UINT(2,port.updates);
    TEST_ASSERT_EQUAL_UINT(1,port.errors);TEST_ASSERT_EQUAL_UINT(0,port.loads);
    jce_scene_renderer_reset_entity_caches(&sr);TEST_ASSERT_FALSE(sr.decals_authored_tried);
    drive(&sr,&scene);TEST_ASSERT_EQUAL_UINT(2,port.attempts);TEST_ASSERT_NOT_NULL(sr.decals_authored);
    TEST_ASSERT_EQUAL_UINT(3,port.updates);TEST_ASSERT_EQUAL_UINT(1,port.resets);
}
static void shader_failure_releases_partial_and_does_not_retry_each_frame(void)
{
    port.fail_shader=2;JceSceneRenderer sr=renderer();JceScene scene={1};
    drive(&sr,&scene);drive(&sr,&scene);
    TEST_ASSERT_EQUAL_UINT(1,port.attempts);TEST_ASSERT_EQUAL_UINT(1,port.shader_destroys);
    TEST_ASSERT_EQUAL_UINT(2,port.frees);TEST_ASSERT_NULL(sr.decals_authored);
}
static void slot_oom_has_no_shader_load_or_live_allocation(void)
{
    port.fail_allocation=2;JceDecalPoolDesc desc={16,(const JcePakArchive *)&pak_token};
    TEST_ASSERT_NULL(scene_create(&desc));TEST_ASSERT_EQUAL_UINT(1,port.frees);
    TEST_ASSERT_EQUAL_UINT(0,port.loads+port.live);TEST_ASSERT_EQUAL_UINT(1,port.errors);
}
static void link_and_sampler_failures_never_publish_invalid_pool(void)
{
    JceDecalPoolDesc desc={16,(const JcePakArchive *)&pak_token};port.fail_program=true;
    TEST_ASSERT_NULL(scene_create(&desc));TEST_ASSERT_EQUAL_UINT(0,port.samplers+port.shader_destroys);
    TEST_ASSERT_EQUAL_UINT(2,port.frees);TEST_ASSERT_EQUAL_UINT(1,port.errors);
    port.fail_program=false;port.fail_sampler=true;TEST_ASSERT_NULL(scene_create(&desc));
    TEST_ASSERT_EQUAL_UINT(1,port.program_destroys);TEST_ASSERT_EQUAL_UINT(4,port.frees);
}
static void valid_zero_program_and_sampler_are_destroyed_once(void)
{
    JceDecalPoolDesc desc={16,(const JcePakArchive *)&pak_token};TEST_ASSERT_NOT_NULL(scene_create(&desc));
    release_active();TEST_ASSERT_EQUAL_UINT(1,port.program_destroys);
    TEST_ASSERT_EQUAL_UINT(1,port.sampler_destroys);TEST_ASSERT_EQUAL_UINT(2,port.frees);
}
static void successful_pool_is_retained_across_explicit_scene_reset(void)
{
    JceSceneRenderer sr=renderer();JceScene scene={1};drive(&sr,&scene);
    JceDecalPool *pool=sr.decals_authored;jce_scene_renderer_reset_entity_caches(&sr);drive(&sr,&scene);
    TEST_ASSERT_EQUAL_PTR(pool,sr.decals_authored);TEST_ASSERT_EQUAL_UINT(1,port.attempts);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(empty_scene_never_acquires_but_runtime_stamps_keep_updating);
    RUN_TEST(authored_modern_pool_is_acquired_once_and_cleared_when_removed);
    RUN_TEST(unsupported_authored_attempt_is_latched_without_blocking_runtime);
    RUN_TEST(allocation_failure_is_latched_and_explicit_reset_allows_one_retry);
    RUN_TEST(shader_failure_releases_partial_and_does_not_retry_each_frame);
    RUN_TEST(slot_oom_has_no_shader_load_or_live_allocation);
    RUN_TEST(link_and_sampler_failures_never_publish_invalid_pool);
    RUN_TEST(valid_zero_program_and_sampler_are_destroyed_once);
    RUN_TEST(successful_pool_is_retained_across_explicit_scene_reset);
    return UNITY_END();
}
