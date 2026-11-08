/* Complete shipping GPU-scene allocation/cleanup and velocity acquire owners.
 * Typed CPU ports create no renderer, resource, driver program or window. */
#include "unity.h"
#include <jce/renderer/jce_gpu_scene.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/os/core/jce_log.h>
#include <bgfx/c99/bgfx.h>
#include "renderer/jce_renderer_shader_profile_internal.h"
#include <stdlib.h>
#include <string.h>

static const char *const compute_names[] = {
    "cs_cull_frustum", "cs_hiz_build", "cs_hiz_reduce", "cs_cull_reset",
    "cs_cull_compact", "cs_build_indirect", "cs_meshlet_cull",
    "cs_foliage_cull", "cs_foliage_indirect"
};
static struct {
    JceRendererApiInfo api;
    bgfx_caps_t caps;
    unsigned api_queries, cap_queries, suffix_queries, allocations, frees;
    unsigned uniform_creates, uniform_destroys, program_loads, program_destroys;
    unsigned layout_begins, layout_ends, warnings, velocity_static, velocity_skin;
    unsigned destroyed_uniform[32], destroyed_program[16];
    bool live_uniform[32], live_program[16];
    bool fail_allocation, null_caps, fail_velocity_static;
    uint32_t fail_program_mask;
    void *owner;
} port;
static unsigned pak_token;

static JceRendererApiInfo observed_api(void)
{
    ++port.api_queries;
    return port.api;
}
static const bgfx_caps_t *observed_caps(void)
{
    ++port.cap_queries;
    return port.null_caps ? NULL : &port.caps;
}
static const char *suffix(void)
{
    ++port.suffix_queries;
    return port.caps.rendererType == BGFX_RENDERER_TYPE_OPENGL ? "glsl" :
        port.caps.rendererType == BGFX_RENDERER_TYPE_OPENGLES ? "essl" : "dx11";
}
static void *owner_alloc(size_t bytes, void *context)
{
    TEST_ASSERT_EQUAL_PTR(&port, context);
    ++port.allocations;
    if (port.fail_allocation) return NULL;
    TEST_ASSERT_NULL(port.owner);
    port.owner = malloc(bytes);
    TEST_ASSERT_NOT_NULL(port.owner);
    memset(port.owner, 0xa5, bytes);
    return port.owner;
}
static void owner_free(void *owner, void *context)
{
    TEST_ASSERT_EQUAL_PTR(&port, context);
    TEST_ASSERT_EQUAL_PTR(port.owner, owner);
    TEST_ASSERT_NOT_NULL(owner);
    free(owner);
    port.owner = NULL;
    ++port.frees;
}
static void storage_free(void *storage)
{
    TEST_ASSERT_NULL(storage); /* These creation controls never grow host batches. */
}
static bgfx_uniform_handle_t create_uniform(const char *name,
    bgfx_uniform_type_t type, uint16_t count)
{
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_TRUE(type == BGFX_UNIFORM_TYPE_VEC4 ||
        type == BGFX_UNIFORM_TYPE_MAT4 || type == BGFX_UNIFORM_TYPE_SAMPLER);
    TEST_ASSERT_TRUE(count > 0);
    TEST_ASSERT_TRUE(port.uniform_creates < 32);
    bgfx_uniform_handle_t handle = {(uint16_t)port.uniform_creates++};
    TEST_ASSERT_FALSE(port.live_uniform[handle.idx]);
    port.live_uniform[handle.idx] = true;
    return handle;
}
static void destroy_uniform(bgfx_uniform_handle_t handle)
{
    TEST_ASSERT_TRUE(handle.idx < 32);
    TEST_ASSERT_TRUE(port.live_uniform[handle.idx]);
    port.live_uniform[handle.idx] = false;
    ++port.destroyed_uniform[handle.idx];
    ++port.uniform_destroys;
}
static bgfx_program_handle_t load_compute(const JcePakArchive *pak,
    const char *name, const char *extension)
{
    TEST_ASSERT_EQUAL_PTR(&pak_token, pak);
    TEST_ASSERT_EQUAL_STRING(port.caps.rendererType == BGFX_RENDERER_TYPE_OPENGL ? "glsl" :
        port.caps.rendererType == BGFX_RENDERER_TYPE_OPENGLES ? "essl" : "dx11", extension);
    unsigned i;
    for (i = 0; i < sizeof compute_names / sizeof compute_names[0]; ++i)
        if (!strcmp(name, compute_names[i])) break;
    TEST_ASSERT_TRUE(i < sizeof compute_names / sizeof compute_names[0]);
    ++port.program_loads;
    bgfx_program_handle_t handle = {UINT16_MAX};
    if (!(port.fail_program_mask & (1u << i))) {
        TEST_ASSERT_FALSE(port.live_program[i]);
        handle.idx = (uint16_t)i;
        port.live_program[i] = true;
    }
    return handle;
}
static void destroy_program(bgfx_program_handle_t handle)
{
    TEST_ASSERT_TRUE(handle.idx < 16);
    TEST_ASSERT_TRUE(port.live_program[handle.idx]);
    port.live_program[handle.idx] = false;
    ++port.destroyed_program[handle.idx];
    ++port.program_destroys;
}
static bgfx_vertex_layout_t *layout_begin(bgfx_vertex_layout_t *layout,
    bgfx_renderer_type_t backend)
{
    TEST_ASSERT_EQUAL_INT(BGFX_RENDERER_TYPE_NOOP, backend);
    ++port.layout_begins;
    return layout;
}
static bgfx_vertex_layout_t *layout_add(bgfx_vertex_layout_t *layout,
    bgfx_attrib_t attribute, uint8_t count, bgfx_attrib_type_t type,
    bool normalized, bool as_integer)
{
    (void)attribute;
    TEST_ASSERT_TRUE(count > 0 && count <= 4);
    TEST_ASSERT_EQUAL_INT(BGFX_ATTRIB_TYPE_FLOAT, type);
    TEST_ASSERT_FALSE(normalized);
    TEST_ASSERT_FALSE(as_integer);
    return layout;
}
static void layout_end(bgfx_vertex_layout_t *layout)
{
    TEST_ASSERT_NOT_NULL(layout);
    ++port.layout_ends;
}
static void forbidden_dynamic_destroy(bgfx_dynamic_vertex_buffer_handle_t handle)
{
    (void)handle;
    TEST_FAIL_MESSAGE("Create-only owner must retain INVALID dynamic buffers");
}
static void forbidden_indirect_destroy(bgfx_indirect_buffer_handle_t handle)
{
    (void)handle;
    TEST_FAIL_MESSAGE("Create-only owner must retain INVALID indirect buffers");
}
static void forbidden_texture_destroy(bgfx_texture_handle_t handle)
{
    (void)handle;
    TEST_FAIL_MESSAGE("Create-only owner must retain INVALID textures");
}
static char *quiet_environment(const char *name) { (void)name; return NULL; }
static void counted_log(JceLogLevel level, const char *tag, const char *file,
    int line, const char *format, ...)
{
    (void)tag; (void)file; (void)line; (void)format;
    if (level == JCE_LOG_LEVEL_WARN) ++port.warnings;
}
#define jce_renderer_get_api_info observed_api
#define bgfx_get_caps observed_caps
#define jce_shader_backend_suffix suffix
#define bgfx_create_uniform create_uniform
#define bgfx_destroy_uniform destroy_uniform
#define bgfx_destroy_program destroy_program
#define bgfx_destroy_dynamic_vertex_buffer forbidden_dynamic_destroy
#define bgfx_destroy_indirect_buffer forbidden_indirect_destroy
#define bgfx_destroy_texture forbidden_texture_destroy
#define bgfx_vertex_layout_begin layout_begin
#define bgfx_vertex_layout_add layout_add
#define bgfx_vertex_layout_end layout_end
#define getenv quiet_environment
#define jce_log_write counted_log
#define JCE_FREE storage_free
#define LOG_TAG "profile-admission"
#include JCE_TEST_GPU_PROFILE_OWNER

typedef struct JceSceneRenderer {
    const JcePakArchive *pak;
    bool gbuffer_vel_prog_tried, taa_want_velocity, ssao_has_velocity;
    bgfx_program_handle_t prog_gbuffer_vel, prog_gbuffer_vel_skinned;
} JceSceneRenderer;
static JceShaderHandle velocity_load(const JcePakArchive *pak, const char *name)
{
    TEST_ASSERT_EQUAL_PTR(&pak_token, pak);
    JceShaderHandle handle = {UINT16_MAX};
    if (!strcmp(name, "gbuffer_vel")) {
        ++port.velocity_static;
        if (!port.fail_velocity_static) handle.idx = 0;
    } else {
        TEST_ASSERT_EQUAL_STRING("gbuffer_vel_skinned", name);
        ++port.velocity_skin;
        handle.idx = 1;
    }
    return handle;
}
#define shader_load_program velocity_load
#include JCE_TEST_VELOCITY_PROFILE_OWNER

void setUp(void)
{
    memset(&port, 0, sizeof port);
    port.api.build_tier = JCE_GRAPHICS_API_TIER_STABLE;
    port.api.backend = JCE_BACKEND_D3D11;
    port.caps.rendererType = BGFX_RENDERER_TYPE_DIRECT3D11;
    port.caps.supported = BGFX_CAPS_COMPUTE | BGFX_CAPS_DRAW_INDIRECT;
    port.caps.formats[BGFX_TEXTURE_FORMAT_R32F] = BGFX_CAPS_FORMAT_TEXTURE_IMAGE_WRITE;
}
void tearDown(void) { TEST_ASSERT_NULL(port.owner); }
static JceGpuScene *create_scene(void)
{
    jce_allocator_t allocator = {owner_alloc, NULL, owner_free, &port};
    return jce_gpu_scene_create((const JcePakArchive *)&pak_token, allocator);
}
static void retire(JceGpuScene *scene)
{
    jce_gpu_scene_destroy(scene);
    TEST_ASSERT_EQUAL_UINT(1, port.frees);
    TEST_ASSERT_EQUAL_UINT(port.uniform_creates, port.uniform_destroys);
    for (unsigned i = 0; i < 32; ++i) TEST_ASSERT_FALSE(port.live_uniform[i]);
    for (unsigned i = 0; i < 16; ++i) TEST_ASSERT_FALSE(port.live_program[i]);
}
static void high_capability_compatibility_gl_es_creates_no_gpu_resources(void)
{
    const uint64_t supported = port.caps.supported;
    const uint32_t format = port.caps.formats[BGFX_TEXTURE_FORMAT_R32F];
    port.api.build_tier = JCE_GRAPHICS_API_TIER_COMPATIBILITY;
    port.api.backend = JCE_BACKEND_D3D11; /* Actual backend, not this stale hint, wins. */
    const bgfx_renderer_type_t backends[] = {
        BGFX_RENDERER_TYPE_OPENGL, BGFX_RENDERER_TYPE_OPENGLES
    };
    for (unsigned i = 0; i < 2; ++i) {
        port.caps.rendererType = backends[i];
        JceGpuScene *scene = create_scene();
        TEST_ASSERT_NOT_NULL(scene);
        const bool admitted = jce_gpu_scene_is_supported(scene);
        jce_gpu_scene_destroy(scene);
        TEST_ASSERT_FALSE(admitted);
        TEST_ASSERT_EQUAL_UINT(0, port.uniform_creates + port.program_loads +
            port.suffix_queries + port.layout_begins);
        TEST_ASSERT_EQUAL_UINT64(supported, port.caps.supported);
        TEST_ASSERT_EQUAL_UINT(format, port.caps.formats[BGFX_TEXTURE_FORMAT_R32F]);
        TEST_ASSERT_EQUAL_INT(backends[i], port.caps.rendererType);
    }
    TEST_ASSERT_EQUAL_UINT(2, port.api_queries);
    TEST_ASSERT_EQUAL_UINT(2, port.allocations);
    TEST_ASSERT_EQUAL_UINT(2, port.frees);
}
static void modern_create_preserves_full_family_and_handle_zero_cleanup(void)
{
    JceGpuScene *scene = create_scene();
    TEST_ASSERT_TRUE(jce_gpu_scene_is_supported(scene));
    TEST_ASSERT_TRUE(jce_gpu_scene_is_indirect(scene));
    TEST_ASSERT_EQUAL_UINT(17, port.uniform_creates);
    TEST_ASSERT_EQUAL_UINT(9, port.program_loads);
    TEST_ASSERT_EQUAL_UINT(4, port.layout_begins);
    TEST_ASSERT_EQUAL_UINT(4, port.layout_ends);
    retire(scene);
    TEST_ASSERT_EQUAL_UINT(9, port.program_destroys);
    TEST_ASSERT_EQUAL_UINT(1, port.destroyed_uniform[0]);
    TEST_ASSERT_EQUAL_UINT(1, port.destroyed_program[0]);
}
static void compatibility_non_gl_keeps_the_actual_backend_full_profile(void)
{
    port.api.build_tier = JCE_GRAPHICS_API_TIER_COMPATIBILITY;
    port.api.backend = JCE_BACKEND_OPENGL;
    JceGpuScene *scene = create_scene();
    TEST_ASSERT_TRUE(jce_gpu_scene_is_supported(scene));
    TEST_ASSERT_EQUAL_UINT(9, port.program_loads);
    retire(scene);
}
static void modern_gl_is_not_restricted_by_the_compatibility_floor(void)
{
    port.caps.rendererType = BGFX_RENDERER_TYPE_OPENGL;
    port.api.backend = JCE_BACKEND_OPENGL;
    JceGpuScene *scene = create_scene();
    TEST_ASSERT_TRUE(jce_gpu_scene_is_supported(scene));
    TEST_ASSERT_EQUAL_UINT(9, port.program_loads);
    retire(scene);
}
static void absent_compute_or_caps_preserves_existing_noop_owner(void)
{
    for (unsigned i = 0; i < 2; ++i) {
        port.caps.supported = i == 0 ? 0 : BGFX_CAPS_COMPUTE;
        port.null_caps = i != 0;
        JceGpuScene *scene = create_scene();
        TEST_ASSERT_NOT_NULL(scene);
        const bool admitted = jce_gpu_scene_is_supported(scene);
        jce_gpu_scene_destroy(scene);
        TEST_ASSERT_FALSE(admitted);
        TEST_ASSERT_EQUAL_UINT(0, port.uniform_creates + port.program_loads);
    }
    TEST_ASSERT_EQUAL_UINT(2, port.frees);
    TEST_ASSERT_EQUAL_UINT(0, port.api_queries);
}
static void failed_cull_initialization_releases_every_acquired_uniform(void)
{
    port.fail_program_mask = 1u;
    JceGpuScene *scene = create_scene();
    TEST_ASSERT_FALSE(jce_gpu_scene_is_supported(scene));
    TEST_ASSERT_EQUAL_UINT(1, port.program_loads);
    TEST_ASSERT_EQUAL_UINT(17, port.uniform_creates);
    retire(scene);
    TEST_ASSERT_EQUAL_UINT(0, port.program_destroys);
}
static void partial_indirect_failure_retires_only_acquired_programs_once(void)
{
    port.fail_program_mask = 1u << 4; /* compact fails; reset/build were acquired. */
    JceGpuScene *scene = create_scene();
    TEST_ASSERT_TRUE(jce_gpu_scene_is_supported(scene));
    TEST_ASSERT_FALSE(jce_gpu_scene_is_indirect(scene));
    TEST_ASSERT_EQUAL_UINT(1, port.destroyed_program[3]);
    TEST_ASSERT_EQUAL_UINT(1, port.destroyed_program[5]);
    retire(scene);
    TEST_ASSERT_EQUAL_UINT(1, port.destroyed_program[3]);
    TEST_ASSERT_EQUAL_UINT(1, port.destroyed_program[5]);
    TEST_ASSERT_EQUAL_UINT(5, port.program_destroys);
}
static void allocation_failure_does_not_query_or_acquire_gpu_state(void)
{
    port.fail_allocation = true;
    TEST_ASSERT_NULL(create_scene());
    TEST_ASSERT_EQUAL_UINT(0, port.api_queries + port.cap_queries +
        port.uniform_creates + port.program_loads);
    TEST_ASSERT_EQUAL_UINT(0, port.frees);
}
static JceSceneRenderer velocity_owner(void)
{
    JceSceneRenderer scene;
    memset(&scene, 0, sizeof scene);
    scene.pak = (const JcePakArchive *)&pak_token;
    scene.prog_gbuffer_vel.idx = scene.prog_gbuffer_vel_skinned.idx = UINT16_MAX;
    scene.taa_want_velocity = scene.ssao_has_velocity = true;
    return scene;
}
static void velocity_without_demand_never_queries_or_latches(void)
{
    JceSceneRenderer scene = velocity_owner();
    sr_ensure_velocity_gbuffer_programs(&scene, false);
    TEST_ASSERT_FALSE(scene.gbuffer_vel_prog_tried);
    TEST_ASSERT_EQUAL_UINT(0, port.api_queries + port.velocity_static + port.velocity_skin);
}
static void compatibility_velocity_refusal_is_latched_and_retains_requested_settings(void)
{
    port.api.build_tier = JCE_GRAPHICS_API_TIER_COMPATIBILITY;
    port.api.backend = JCE_BACKEND_OPENGL;
    JceSceneRenderer scene = velocity_owner();
    for (unsigned i = 0; i < 20; ++i)
        sr_ensure_velocity_gbuffer_programs(&scene, true);
    TEST_ASSERT_TRUE(scene.gbuffer_vel_prog_tried);
    TEST_ASSERT_EQUAL_UINT(1, port.api_queries);
    TEST_ASSERT_EQUAL_UINT(0, port.velocity_static + port.velocity_skin);
    TEST_ASSERT_EQUAL_UINT(1, port.warnings);
    TEST_ASSERT_EQUAL_UINT(UINT16_MAX, scene.prog_gbuffer_vel.idx);
    TEST_ASSERT_EQUAL_UINT(UINT16_MAX, scene.prog_gbuffer_vel_skinned.idx);
    TEST_ASSERT_TRUE(scene.taa_want_velocity && scene.ssao_has_velocity);
}
static void modern_velocity_preserves_dual_acquisition_and_zero_handle(void)
{
    JceSceneRenderer scene = velocity_owner();
    sr_ensure_velocity_gbuffer_programs(&scene, true);
    sr_ensure_velocity_gbuffer_programs(&scene, true);
    TEST_ASSERT_EQUAL_UINT(1, port.api_queries);
    TEST_ASSERT_EQUAL_UINT(1, port.velocity_static);
    TEST_ASSERT_EQUAL_UINT(1, port.velocity_skin);
    TEST_ASSERT_EQUAL_UINT(0, scene.prog_gbuffer_vel.idx);
    TEST_ASSERT_EQUAL_UINT(1, scene.prog_gbuffer_vel_skinned.idx);
}
static void failed_velocity_static_acquire_remains_invalid_without_retries(void)
{
    port.fail_velocity_static = true;
    JceSceneRenderer scene = velocity_owner();
    sr_ensure_velocity_gbuffer_programs(&scene, true);
    sr_ensure_velocity_gbuffer_programs(&scene, true);
    TEST_ASSERT_EQUAL_UINT(UINT16_MAX, scene.prog_gbuffer_vel.idx);
    TEST_ASSERT_EQUAL_UINT(1, scene.prog_gbuffer_vel_skinned.idx);
    TEST_ASSERT_EQUAL_UINT(1, port.velocity_static);
    TEST_ASSERT_EQUAL_UINT(1, port.velocity_skin);
    TEST_ASSERT_EQUAL_UINT(1, port.warnings);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(high_capability_compatibility_gl_es_creates_no_gpu_resources);
    RUN_TEST(modern_create_preserves_full_family_and_handle_zero_cleanup);
    RUN_TEST(compatibility_non_gl_keeps_the_actual_backend_full_profile);
    RUN_TEST(modern_gl_is_not_restricted_by_the_compatibility_floor);
    RUN_TEST(absent_compute_or_caps_preserves_existing_noop_owner);
    RUN_TEST(failed_cull_initialization_releases_every_acquired_uniform);
    RUN_TEST(partial_indirect_failure_retires_only_acquired_programs_once);
    RUN_TEST(allocation_failure_does_not_query_or_acquire_gpu_state);
    RUN_TEST(velocity_without_demand_never_queries_or_latches);
    RUN_TEST(compatibility_velocity_refusal_is_latched_and_retains_requested_settings);
    RUN_TEST(modern_velocity_preserves_dual_acquisition_and_zero_handle);
    RUN_TEST(failed_velocity_static_acquire_remains_invalid_without_retries);
    return UNITY_END();
}
