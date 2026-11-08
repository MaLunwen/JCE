/* Real shadow owner failure controls and NOOP-device allocation lifecycle.
 * Backend adapters use first-party uint16 IDs; no third-party test types. */
#include "unity.h"
#include "middleware/scene/jce_sr_shadow_targets.h"
#include "middleware/scene/jce_sr_internal.h"
#include "renderer/jce_offscreen_retirement_internal.h"
#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_console.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <string.h>
#include <stdio.h>

#define HANDLE_CAP 32
static bool textures[HANDLE_CAP], frame_buffers[HANDLE_CAP];
static uint16_t next_texture, next_frame_buffer, texture_size, texture_format;
static unsigned texture_creates, frame_buffer_creates;
static unsigned texture_destroys, frame_buffer_destroys;
static unsigned failed_texture_call, failed_frame_buffer_call;
static JceRenderer *renderer;
static JceSceneRenderer *scene_renderer;
static JceScene *scene;
static JceCamera *camera;
static JceAtomicI32 *bgfx_fatal_records;
static uint16_t noop_backend_uniform_baseline;
/* Observe the real logger without suppressing any record. The engine callback
 * returns after reporting a fatal, so exit success alone cannot be our gate. */
static void observe_bgfx_fatal(const JceLogRecord *record, void *user)
{
    if (record->level == JCE_LOG_LEVEL_ERROR &&
        strcmp(record->tag, "jce_renderer") == 0 &&
        strncmp(record->message, "bgfx fatal:", 11u) == 0)
        jce_atomic_i32_add((JceAtomicI32 *)user, 1);
}

static uint16_t create_texture(uint16_t size, uint16_t format, void *user)
{
    TEST_ASSERT_EQUAL_PTR(textures, user);
    ++texture_creates;
    texture_size = size;
    texture_format = format;
    if (texture_creates == failed_texture_call) return UINT16_MAX;
    TEST_ASSERT_LESS_THAN(HANDLE_CAP, next_texture);
    uint16_t handle = next_texture++;
    textures[handle] = true;
    return handle;
}
static uint16_t create_frame_buffer(uint16_t texture, void *user)
{
    TEST_ASSERT_EQUAL_PTR(textures, user);
    TEST_ASSERT_LESS_THAN(HANDLE_CAP, texture);
    TEST_ASSERT_TRUE(textures[texture]);
    ++frame_buffer_creates;
    if (frame_buffer_creates == failed_frame_buffer_call) return UINT16_MAX;
    TEST_ASSERT_LESS_THAN(HANDLE_CAP, next_frame_buffer);
    uint16_t handle = next_frame_buffer++;
    frame_buffers[handle] = true;
    return handle;
}
static void destroy_texture(uint16_t handle, void *user)
{
    TEST_ASSERT_EQUAL_PTR(textures, user);
    TEST_ASSERT_LESS_THAN(HANDLE_CAP, handle);
    TEST_ASSERT_TRUE_MESSAGE(textures[handle], "Texture must have exactly one owner");
    textures[handle] = false;
    ++texture_destroys;
}
static void destroy_frame_buffer(uint16_t handle, void *user)
{
    TEST_ASSERT_EQUAL_PTR(textures, user);
    TEST_ASSERT_LESS_THAN(HANDLE_CAP, handle);
    TEST_ASSERT_TRUE_MESSAGE(frame_buffers[handle], "Framebuffer must have exactly one owner");
    frame_buffers[handle] = false;
    ++frame_buffer_destroys;
}
static const JceSrDepthTargetProvider provider = {
    create_texture, create_frame_buffer, destroy_texture,
    destroy_frame_buffer, textures
};
static unsigned live_textures(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < HANDLE_CAP; ++i) if (textures[i]) ++count;
    return count;
}

void setUp(void)
{
    memset(textures, 0, sizeof(textures));
    memset(frame_buffers, 0, sizeof(frame_buffers));
    next_texture = next_frame_buffer = texture_size = texture_format = 0;
    texture_creates = frame_buffer_creates = 0;
    texture_destroys = frame_buffer_destroys = 0;
    failed_texture_call = failed_frame_buffer_call = 0;
    renderer = NULL; scene_renderer = NULL; scene = NULL; camera = NULL;
    bgfx_fatal_records = NULL;
    noop_backend_uniform_baseline = 0;
}
void tearDown(void)
{
    if (scene_renderer) {
        fputs("shadow lifecycle: scene renderer teardown begin\n", stderr);
        jce_scene_renderer_destroy(scene_renderer); scene_renderer = NULL;
        fputs("shadow lifecycle: scene renderer teardown end\n", stderr);
    }
    if (camera) { jce_camera_destroy(camera); camera = NULL; }
    if (scene) { jce_scene_destroy(scene); scene = NULL; }
    if (renderer) {
        fputs("shadow lifecycle: renderer teardown begin\n", stderr);
        jce_renderer_destroy(renderer); renderer = NULL;
        fputs("shadow lifecycle: renderer teardown end\n", stderr);
    }
    if (bgfx_fatal_records) {
        jce_log_flush();
        jce_log_set_sink(NULL, NULL);
        const int32_t fatals = jce_atomic_i32_load(bgfx_fatal_records);
        jce_atomic_i32_destroy(bgfx_fatal_records); bgfx_fatal_records = NULL;
        TEST_ASSERT_EQUAL_INT32_MESSAGE(0, fatals,
            "Real bgfx fatal is a failure even when its callback returns");
    }
}
static bool ensure_pair(uint16_t *texture, uint16_t *frame_buffer)
{
    return sr_shadow_depth_target_ensure(texture, frame_buffer, 1024, 7, &provider);
}
static void test_valid_handle_zero_preserves_size_and_format(void)
{
    uint16_t texture = UINT16_MAX, frame_buffer = UINT16_MAX;
    TEST_ASSERT_TRUE(ensure_pair(&texture, &frame_buffer));
    TEST_ASSERT_EQUAL_UINT16(0, texture);
    TEST_ASSERT_EQUAL_UINT16(0, frame_buffer);
    TEST_ASSERT_EQUAL_UINT16(1024, texture_size);
    TEST_ASSERT_EQUAL_UINT16(7, texture_format);
    TEST_ASSERT_EQUAL_UINT(1, live_textures());
}
static void test_texture_failure_leaves_empty_pair(void)
{
    uint16_t texture = UINT16_MAX, frame_buffer = UINT16_MAX;
    failed_texture_call = 1;
    TEST_ASSERT_FALSE(ensure_pair(&texture, &frame_buffer));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, texture);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, frame_buffer);
    TEST_ASSERT_EQUAL_UINT(0, frame_buffer_creates);
    TEST_ASSERT_EQUAL_UINT(0, live_textures());
}
static void test_framebuffer_failure_does_not_retain_texture(void)
{
    uint16_t texture = UINT16_MAX, frame_buffer = UINT16_MAX;
    failed_frame_buffer_call = 1;
    TEST_ASSERT_FALSE(ensure_pair(&texture, &frame_buffer));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, frame_buffer);
    TEST_ASSERT_EQUAL_UINT(0, live_textures());
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, texture);
    TEST_ASSERT_EQUAL_UINT(1, texture_destroys);
}
static void test_retry_after_framebuffer_failure_has_one_texture_owner(void)
{
    uint16_t texture = UINT16_MAX, frame_buffer = UINT16_MAX;
    failed_frame_buffer_call = 1;
    TEST_ASSERT_FALSE(ensure_pair(&texture, &frame_buffer));
    TEST_ASSERT_TRUE(ensure_pair(&texture, &frame_buffer));
    TEST_ASSERT_EQUAL_UINT(1, live_textures());
    sr_shadow_depth_target_destroy(&texture, &frame_buffer, &provider);
    TEST_ASSERT_EQUAL_UINT(0, live_textures());
    TEST_ASSERT_EQUAL_UINT(2, texture_destroys);
    TEST_ASSERT_EQUAL_UINT(1, frame_buffer_destroys);
}
static void test_existing_pair_is_reused_without_allocation(void)
{
    uint16_t texture = UINT16_MAX, frame_buffer = UINT16_MAX;
    TEST_ASSERT_TRUE(ensure_pair(&texture, &frame_buffer));
    TEST_ASSERT_TRUE(ensure_pair(&texture, &frame_buffer));
    TEST_ASSERT_EQUAL_UINT(1, texture_creates);
    TEST_ASSERT_EQUAL_UINT(1, frame_buffer_creates);
    TEST_ASSERT_EQUAL_UINT(0, texture_destroys);
    TEST_ASSERT_EQUAL_UINT(0, frame_buffer_destroys);
}
static void test_partial_pair_is_retired_before_replacement(void)
{
    uint16_t texture = create_texture(1024, 7, textures);
    uint16_t frame_buffer = UINT16_MAX;
    TEST_ASSERT_TRUE(ensure_pair(&texture, &frame_buffer));
    TEST_ASSERT_EQUAL_UINT(1, live_textures());
    TEST_ASSERT_EQUAL_UINT(1, texture_destroys);
    TEST_ASSERT_EQUAL_UINT16(1, texture);
}
static void test_destroy_is_idempotent_including_handle_zero(void)
{
    uint16_t texture = UINT16_MAX, frame_buffer = UINT16_MAX;
    TEST_ASSERT_TRUE(ensure_pair(&texture, &frame_buffer));
    sr_shadow_depth_target_destroy(&texture, &frame_buffer, &provider);
    sr_shadow_depth_target_destroy(&texture, &frame_buffer, &provider);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, texture);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, frame_buffer);
    TEST_ASSERT_EQUAL_UINT(1, texture_destroys);
    TEST_ASSERT_EQUAL_UINT(1, frame_buffer_destroys);
}
static void test_texture_failure_can_retry_without_spurious_destroy(void)
{
    uint16_t texture = UINT16_MAX, frame_buffer = UINT16_MAX;
    failed_texture_call = 1;
    TEST_ASSERT_FALSE(ensure_pair(&texture, &frame_buffer));
    TEST_ASSERT_TRUE(ensure_pair(&texture, &frame_buffer));
    TEST_ASSERT_EQUAL_UINT(1, live_textures());
    TEST_ASSERT_EQUAL_UINT(0, texture_destroys);
}

static JceGpuStats actual_stats(void)
{
    for (unsigned i = 0; i < 4; ++i) jce_renderer_submit_frame(renderer);
    JceGpuStats stats;
    TEST_ASSERT_TRUE(jce_renderer_get_gpu_stats(&stats));
    TEST_ASSERT_TRUE(stats.valid);
    return stats;
}
static JceGpuStats create_noop(void)
{
    bgfx_fatal_records = jce_atomic_i32_create(0);
    TEST_ASSERT_NOT_NULL(bgfx_fatal_records);
    jce_log_set_sink(observe_bgfx_fatal, bgfx_fatal_records);
    renderer = jce_renderer_create_headless();
    TEST_ASSERT_NOT_NULL(renderer);
    JceRenderPipelineDesc low;
    jce_render_pipeline_preset_low(&low);
    low.enable_csm = true;
    jce_render_pipeline_apply(&low);
    JceGpuStats before = actual_stats();
    /* Private Noop observation: public GPU stats omit uniform slot counts. */
    noop_backend_uniform_baseline = bgfx_get_stats()->numUniforms;
    scene_renderer = jce_scene_renderer_create(renderer, NULL, NULL);
    TEST_ASSERT_NOT_NULL(scene_renderer);
    scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    JceCameraDesc desc = {0};
    desc.mode = JCE_CAMERA_PERSPECTIVE;
    desc.position.z = 10.0f;
    desc.up.y = 1.0f;
    desc.fov_deg = 60.0f;
    desc.near_plane = 0.1f;
    desc.far_plane = 100.0f;
    camera = jce_camera_create(&desc);
    TEST_ASSERT_NOT_NULL(camera);
    return before;
}
static JceGpuStats configure_targets(uint8_t cascades, uint16_t size)
{
    JceSceneRenderConfig cfg = jce_scene_render_config_default();
    cfg.draw_skybox = cfg.draw_shadows = cfg.draw_opaque = false;
    cfg.draw_sprites = cfg.draw_transparent = cfg.apply_postfx = false;
    cfg.viewport_width = cfg.viewport_height = 16;
    cfg.shadow_map_size = size;
    cfg.csm_cascades = cascades;
    jce_scene_renderer_render(scene_renderer, scene, camera, 3, 0.0f, &cfg);
    return actual_stats();
}
/* No shaders are loaded on this Noop fixture. Demand the actual target owner
 * directly after public configuration; do not pretend this proves drawing. */
static JceGpuStats render_targets(uint8_t cascades, uint16_t size)
{
    (void)configure_targets(cascades, size);
    TEST_ASSERT_TRUE(sr_ensure_csm_count(scene_renderer));
    return actual_stats();
}
static void test_noop_disabled_render_resolves_policy_without_allocating(void)
{
    (void)create_noop();
    JceGpuStats owned = actual_stats();
    JceGpuStats configured = configure_targets(4, 4096);
    TEST_ASSERT_EQUAL_UINT16(4096, scene_renderer->shadow_map_size);
    TEST_ASSERT_EQUAL_UINT16(4, scene_renderer->csm_cascade_count);
    TEST_ASSERT_EQUAL_UINT16(owned.num_frame_buffers, configured.num_frame_buffers);
    TEST_ASSERT_EQUAL_INT64(owned.rt_memory_used, configured.rt_memory_used);
    for (unsigned i = 0; i < 4; ++i)
        TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, scene_renderer->csm_tex[i].idx);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, scene_renderer->dyn_csm_atlas_tex.idx);
}
static SrShadowTargetAdmission admit_dynamic(bool dual, bool percascade,
    unsigned count, uint16_t *texture, uint16_t *frame_buffer)
{
    return sr_shadow_dynamic_target_admit(dual, percascade, count, texture,
                                          frame_buffer, 4096, 7, &provider);
}
static void test_dynamic_cold_requests_require_every_need_predicate(void)
{
    uint16_t texture = UINT16_MAX, frame_buffer = UINT16_MAX;
    TEST_ASSERT_EQUAL(SR_SHADOW_TARGET_NOT_NEEDED,
        admit_dynamic(false, true, 1, &texture, &frame_buffer));
    TEST_ASSERT_EQUAL(SR_SHADOW_TARGET_NOT_NEEDED,
        admit_dynamic(true, false, 1, &texture, &frame_buffer));
    TEST_ASSERT_EQUAL(SR_SHADOW_TARGET_NOT_NEEDED,
        admit_dynamic(true, true, 0, &texture, &frame_buffer));
    TEST_ASSERT_EQUAL_UINT(0, texture_creates);
    TEST_ASSERT_EQUAL_UINT(0, frame_buffer_creates);
    TEST_ASSERT_EQUAL(SR_SHADOW_TARGET_READY,
        admit_dynamic(true, true, 1, &texture, &frame_buffer));
    TEST_ASSERT_EQUAL_UINT16(4096, texture_size);
    TEST_ASSERT_EQUAL_UINT16(7, texture_format);
    sr_shadow_depth_target_destroy(&texture, &frame_buffer, &provider);
}
static void test_dynamic_failure_retries_and_idle_preserves_existing_pair(void)
{
    uint16_t texture = UINT16_MAX, frame_buffer = UINT16_MAX;
    failed_texture_call = 1;
    TEST_ASSERT_EQUAL(SR_SHADOW_TARGET_FAILED,
        admit_dynamic(true, true, 1, &texture, &frame_buffer));
    TEST_ASSERT_EQUAL_UINT(0, frame_buffer_creates);
    failed_frame_buffer_call = 1;
    TEST_ASSERT_EQUAL(SR_SHADOW_TARGET_FAILED,
        admit_dynamic(true, true, 1, &texture, &frame_buffer));
    TEST_ASSERT_EQUAL_UINT(0, live_textures());
    TEST_ASSERT_EQUAL_UINT(1, texture_destroys);
    TEST_ASSERT_EQUAL(SR_SHADOW_TARGET_READY,
        admit_dynamic(true, true, 1, &texture, &frame_buffer));
    const uint16_t resident = texture;
    TEST_ASSERT_EQUAL(SR_SHADOW_TARGET_NOT_NEEDED,
        admit_dynamic(true, true, 0, &texture, &frame_buffer));
    TEST_ASSERT_EQUAL(SR_SHADOW_TARGET_NOT_NEEDED,
        admit_dynamic(false, true, 2, &texture, &frame_buffer));
    TEST_ASSERT_EQUAL(SR_SHADOW_TARGET_READY,
        admit_dynamic(true, true, 2, &texture, &frame_buffer));
    TEST_ASSERT_EQUAL_UINT16(resident, texture);
    TEST_ASSERT_EQUAL_UINT(3, texture_creates);
    TEST_ASSERT_EQUAL_UINT(2, frame_buffer_creates);
    TEST_ASSERT_EQUAL_UINT(1, live_textures());
    sr_shadow_depth_target_destroy(&texture, &frame_buffer, &provider);
    sr_shadow_depth_target_destroy(&texture, &frame_buffer, &provider);
    TEST_ASSERT_EQUAL_UINT(2, texture_destroys);
    TEST_ASSERT_EQUAL_UINT(1, frame_buffer_destroys);
}
static void test_noop_dynamic_atlas_owner_creates_on_demand_and_retains_idle(void)
{
    JceGpuStats before = create_noop();
    (void)configure_targets(4, 1024);
    JceGpuStats owned = actual_stats();
    scene_renderer->shadow_dyn_count = 0;
    TEST_ASSERT_FALSE(sr_ensure_dyn_csm_atlas(scene_renderer, true, true));
    scene_renderer->shadow_dyn_count = 1;
    TEST_ASSERT_FALSE(sr_ensure_dyn_csm_atlas(scene_renderer, false, true));
    TEST_ASSERT_FALSE(sr_ensure_dyn_csm_atlas(scene_renderer, true, false));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, scene_renderer->dyn_csm_atlas_tex.idx);
    TEST_ASSERT_TRUE(sr_ensure_dyn_csm_atlas(scene_renderer, true, true));
    JceGpuStats demanded = actual_stats();
    TEST_ASSERT_EQUAL_UINT16(owned.num_frame_buffers + 1, demanded.num_frame_buffers);
    TEST_ASSERT_GREATER_THAN_INT64(owned.rt_memory_used, demanded.rt_memory_used);
    const uint16_t handle = scene_renderer->dyn_csm_atlas_tex.idx;
    scene_renderer->shadow_dyn_count = 0;
    TEST_ASSERT_FALSE(sr_ensure_dyn_csm_atlas(scene_renderer, true, true));
    TEST_ASSERT_TRUE(scene_renderer->dyn_csm_valid);
    TEST_ASSERT_EQUAL_UINT16(handle, scene_renderer->dyn_csm_atlas_tex.idx);
    JceGpuStats idle = actual_stats();
    TEST_ASSERT_EQUAL_UINT16(demanded.num_frame_buffers, idle.num_frame_buffers);
    TEST_ASSERT_EQUAL_INT64(demanded.rt_memory_used, idle.rt_memory_used);
    scene_renderer->shadow_dyn_count = 1;
    TEST_ASSERT_TRUE(sr_ensure_dyn_csm_atlas(scene_renderer, true, true));
    TEST_ASSERT_EQUAL_UINT16(handle, scene_renderer->dyn_csm_atlas_tex.idx);
    /* Count was injected only for admission; no unallocated caster list is
     * dereferenced or rendered by this actual-owner Noop control. */
    scene_renderer->shadow_dyn_count = 0;
    jce_scene_renderer_destroy(scene_renderer); scene_renderer = NULL;
    JceGpuStats released = actual_stats();
    TEST_ASSERT_EQUAL_UINT16(before.num_frame_buffers, released.num_frame_buffers);
    TEST_ASSERT_EQUAL_INT64(before.rt_memory_used, released.rt_memory_used);
}
static void test_noop_constructor_defers_shadow_depth_targets(void)
{
    JceGpuStats before = create_noop();
    JceGpuStats after = actual_stats();
    TEST_ASSERT_EQUAL_UINT16(before.num_frame_buffers, after.num_frame_buffers);
    TEST_ASSERT_EQUAL_INT64(before.rt_memory_used, after.rt_memory_used);
}
/* A never-rendered cloud owner must not steal backend texture zero. This
 * observes real Noop allocation counts before backend shutdown, independently
 * of the later fatal==0 guard; it certifies ownership, not pixels. */
static void test_noop_cold_scene_teardown_preserves_backend_texture_baseline(void)
{
    JceGpuStats before = create_noop();
    JceGpuStats allocated = actual_stats();
    TEST_ASSERT_GREATER_THAN_UINT16(before.num_textures, allocated.num_textures);
    jce_scene_renderer_destroy(scene_renderer); scene_renderer = NULL;
    JceGpuStats released = actual_stats();
    TEST_ASSERT_EQUAL_UINT16(before.num_textures, released.num_textures);
    TEST_ASSERT_EQUAL_UINT16(before.num_frame_buffers, released.num_frame_buffers);
    TEST_ASSERT_EQUAL_INT64(before.rt_memory_used, released.rt_memory_used);
}
static void test_noop_lazy_handles_begin_invalid(void)
{
    (void)create_noop();
#define ASSERT_LAZY_INVALID(field) \
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(UINT16_MAX, scene_renderer->field.idx, #field)
    ASSERT_LAZY_INVALID(ssao_albedo_tex);
    ASSERT_LAZY_INVALID(s_water_depth);
    ASSERT_LAZY_INVALID(u_water_absorb);
    ASSERT_LAZY_INVALID(u_water_absorb_tint);
    ASSERT_LAZY_INVALID(u_water_depth_params);
    ASSERT_LAZY_INVALID(u_water_caustics);
    ASSERT_LAZY_INVALID(s_water_caustic_disp);
    ASSERT_LAZY_INVALID(u_water_shore);
    ASSERT_LAZY_INVALID(u_sky_stylise);
    ASSERT_LAZY_INVALID(u_sky_tint_shadow);
    ASSERT_LAZY_INVALID(u_sky_tint_mid);
    ASSERT_LAZY_INVALID(u_sky_tint_high);
    ASSERT_LAZY_INVALID(prog_particle_sprite);
    ASSERT_LAZY_INVALID(particle_quad_vb);
    ASSERT_LAZY_INVALID(particle_quad_ib);
    ASSERT_LAZY_INVALID(u_particle_misc);
    ASSERT_LAZY_INVALID(u_particle_soft);
    ASSERT_LAZY_INVALID(s_particle_scene_depth);
    ASSERT_LAZY_INVALID(prog_gbuffer_vel_inst);
#undef ASSERT_LAZY_INVALID
    TEST_ASSERT_NULL(scene_renderer->velocity_instance_queue);
    TEST_ASSERT_FALSE(scene_renderer->gbuffer_vel_inst_prog_tried);
}
/* Actual queue allocation and the public scene destruction wire execute on
 * the Noop owner. Program retirement has a separate counted-source control;
 * this test does not claim a shader upload or velocity pixels. */
static void test_noop_velocity_queue_release_and_scene_recreation(void)
{
    JceGpuStats before = create_noop();
    scene_renderer->velocity_instance_queue = jce_rq_create(1024);
    TEST_ASSERT_NOT_NULL(scene_renderer->velocity_instance_queue);
    scene_renderer->gbuffer_vel_inst_prog_tried = true;
    sr_velocity_inst_release(scene_renderer);
    TEST_ASSERT_NULL(scene_renderer->velocity_instance_queue);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, scene_renderer->prog_gbuffer_vel_inst.idx);
    TEST_ASSERT_FALSE(scene_renderer->gbuffer_vel_inst_prog_tried);
    sr_velocity_inst_release(scene_renderer);
    scene_renderer->velocity_instance_queue = jce_rq_create(1024);
    TEST_ASSERT_NOT_NULL(scene_renderer->velocity_instance_queue);
    scene_renderer->gbuffer_vel_inst_prog_tried = true;
    jce_scene_renderer_destroy(scene_renderer); scene_renderer = NULL;
    JceGpuStats released = actual_stats();
    TEST_ASSERT_EQUAL_UINT16(before.num_textures, released.num_textures);
    TEST_ASSERT_EQUAL_UINT16(before.num_frame_buffers, released.num_frame_buffers);
    TEST_ASSERT_EQUAL_UINT16(noop_backend_uniform_baseline, bgfx_get_stats()->numUniforms);
    scene_renderer = jce_scene_renderer_create(renderer, NULL, NULL);
    TEST_ASSERT_NOT_NULL(scene_renderer);
    TEST_ASSERT_NULL(scene_renderer->velocity_instance_queue);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, scene_renderer->prog_gbuffer_vel_inst.idx);
    TEST_ASSERT_FALSE(scene_renderer->gbuffer_vel_inst_prog_tried);
    scene_renderer->velocity_instance_queue = jce_rq_create(1024);
    TEST_ASSERT_NOT_NULL(scene_renderer->velocity_instance_queue);
    jce_scene_renderer_destroy(scene_renderer); scene_renderer = NULL;
    released = actual_stats();
    TEST_ASSERT_EQUAL_UINT16(before.num_textures, released.num_textures);
    TEST_ASSERT_EQUAL_UINT16(before.num_frame_buffers, released.num_frame_buffers);
    TEST_ASSERT_EQUAL_UINT16(noop_backend_uniform_baseline, bgfx_get_stats()->numUniforms);
}
/* CPU queue lifecycle only: no fake draw reaches a backend encoder. */
static void test_noop_depth_queue_reuse_isolation_and_cpu_release(void)
{
    (void)create_noop();
    TEST_ASSERT_NULL(scene_renderer->depth_prepass_queue);
    JceSceneRenderer *other = (JceSceneRenderer *)jce_malloc(sizeof(*other));
    TEST_ASSERT_NOT_NULL(other);
    memset(other, 0, sizeof(*other)); /* second CPU-only owner */
    JceAllocTrack before, released;
    const bool tracking = jce_alloc_track_snapshot(&before);
    TEST_ASSERT_FALSE(sr_depth_prepass_queue_prepare(NULL));
    sr_depth_prepass_queue_release(NULL);
    TEST_ASSERT_TRUE(sr_depth_prepass_queue_prepare(scene_renderer));
    TEST_ASSERT_TRUE(sr_depth_prepass_queue_prepare(other));
    JceRenderQueue *first = scene_renderer->depth_prepass_queue;
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_TRUE(first != other->depth_prepass_queue);
    JceDrawCmd cmd = {0};
    jce_rq_push(first, &cmd);
    jce_rq_push(other->depth_prepass_queue, &cmd);
    TEST_ASSERT_EQUAL_UINT32(1, jce_rq_count(first));
    TEST_ASSERT_TRUE(sr_depth_prepass_queue_prepare(scene_renderer));
    TEST_ASSERT_EQUAL_PTR(first, scene_renderer->depth_prepass_queue);
    TEST_ASSERT_EQUAL_UINT32(0, jce_rq_count(first));
    TEST_ASSERT_EQUAL_UINT32(1, jce_rq_count(other->depth_prepass_queue));
    sr_depth_prepass_queue_release(scene_renderer);
    sr_depth_prepass_queue_release(scene_renderer);
    TEST_ASSERT_NULL(scene_renderer->depth_prepass_queue);
    TEST_ASSERT_EQUAL_UINT32(1, jce_rq_count(other->depth_prepass_queue));
    sr_depth_prepass_queue_release(other);
    if (tracking) {
        TEST_ASSERT_TRUE(jce_alloc_track_snapshot(&released));
        /* No asset jobs or queue flush runs here. The Noop foreign bridge is
         * outside this tracked CPU total; this is not a process-RSS claim. */
        TEST_ASSERT_EQUAL_UINT64(before.live_bytes, released.live_bytes);
        TEST_ASSERT_EQUAL_UINT64(before.live_count, released.live_count);
    }
    jce_free(other);
}

static void test_noop_depth_queue_scene_destroy_and_recreation(void)
{
    JceGpuStats before = create_noop();
    TEST_ASSERT_TRUE(sr_depth_prepass_queue_prepare(scene_renderer));
    TEST_ASSERT_NOT_NULL(scene_renderer->depth_prepass_queue);
    jce_scene_renderer_destroy(scene_renderer); scene_renderer = NULL;
    (void)actual_stats();
    scene_renderer = jce_scene_renderer_create(renderer, NULL, NULL);
    TEST_ASSERT_NOT_NULL(scene_renderer);
    TEST_ASSERT_NULL(scene_renderer->depth_prepass_queue);
    TEST_ASSERT_TRUE(sr_depth_prepass_queue_prepare(scene_renderer));
    TEST_ASSERT_EQUAL_UINT32(0, jce_rq_count(scene_renderer->depth_prepass_queue));
    jce_scene_renderer_destroy(scene_renderer); scene_renderer = NULL;
    JceGpuStats released = actual_stats();
    TEST_ASSERT_EQUAL_UINT16(before.num_textures, released.num_textures);
    TEST_ASSERT_EQUAL_UINT16(before.num_frame_buffers, released.num_frame_buffers);
    TEST_ASSERT_EQUAL_UINT16(noop_backend_uniform_baseline, bgfx_get_stats()->numUniforms);
}

static void test_noop_cold_scene_teardown_restores_uniform_handle_baseline(void)
{
    (void)create_noop();
    (void)actual_stats();
    TEST_ASSERT_GREATER_THAN_UINT16(noop_backend_uniform_baseline,
                                   bgfx_get_stats()->numUniforms);
    jce_scene_renderer_destroy(scene_renderer); scene_renderer = NULL;
    (void)actual_stats();
    /* This is used-handle occupancy, not a per-reference leak or pixel proof. */
    TEST_ASSERT_EQUAL_UINT16(noop_backend_uniform_baseline,
                             bgfx_get_stats()->numUniforms);
}
static void test_noop_prepass_targets_resize_velocity_and_destroy_restore_baseline(void)
{
    JceGpuStats before = create_noop();
    JceGpuStats owned = actual_stats();
    sr_ensure_ssao_target(scene_renderer, 0, 16, 8, false);
    JceGpuStats first = actual_stats();
    TEST_ASSERT_TRUE(scene_renderer->ssao_valid);
    TEST_ASSERT_EQUAL_UINT16(owned.num_textures + 3, first.num_textures);
    TEST_ASSERT_EQUAL_UINT16(owned.num_frame_buffers + 1, first.num_frame_buffers);
    TEST_ASSERT_GREATER_THAN_INT64(owned.rt_memory_used, first.rt_memory_used);
    const uint16_t first_fbo = scene_renderer->ssao_depth_fbo.idx;
    sr_ensure_ssao_target(scene_renderer, 0, 16, 8, false);
    JceGpuStats same = actual_stats();
    TEST_ASSERT_EQUAL_UINT16(first_fbo, scene_renderer->ssao_depth_fbo.idx);
    TEST_ASSERT_EQUAL_UINT16(first.num_textures, same.num_textures);
    TEST_ASSERT_EQUAL_INT64(first.rt_memory_used, same.rt_memory_used);
    sr_ensure_ssao_target(scene_renderer, 0, 16, 8, true);
    JceGpuStats velocity = actual_stats();
    TEST_ASSERT_TRUE(scene_renderer->ssao_valid);
    TEST_ASSERT_TRUE(scene_renderer->ssao_has_velocity);
    TEST_ASSERT_EQUAL_UINT16(owned.num_textures + 4, velocity.num_textures);
    TEST_ASSERT_EQUAL_UINT16(owned.num_frame_buffers + 1, velocity.num_frame_buffers);
    TEST_ASSERT_GREATER_THAN_INT64(first.rt_memory_used, velocity.rt_memory_used);
    sr_ensure_ssao_target(scene_renderer, 0, 8, 4, true);
    JceGpuStats resized = actual_stats();
    TEST_ASSERT_TRUE(scene_renderer->ssao_valid);
    TEST_ASSERT_EQUAL_UINT16(owned.num_textures + 4, resized.num_textures);
    TEST_ASSERT_EQUAL_UINT16(owned.num_frame_buffers + 1, resized.num_frame_buffers);
    TEST_ASSERT_EQUAL_INT64((velocity.rt_memory_used - owned.rt_memory_used) / 4,
                           resized.rt_memory_used - owned.rt_memory_used);
    sr_ensure_ssao_target(scene_renderer, 0, 8, 4, false);
    JceGpuStats no_velocity = actual_stats();
    TEST_ASSERT_TRUE(scene_renderer->ssao_valid);
    TEST_ASSERT_FALSE(scene_renderer->ssao_has_velocity);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, scene_renderer->ssao_velocity_tex.idx);
    TEST_ASSERT_EQUAL_UINT16(owned.num_textures + 3, no_velocity.num_textures);
    TEST_ASSERT_EQUAL_UINT16(owned.num_frame_buffers + 1, no_velocity.num_frame_buffers);
    jce_scene_renderer_destroy(scene_renderer); scene_renderer = NULL;
    JceGpuStats released = actual_stats();
    TEST_ASSERT_EQUAL_UINT16(before.num_textures, released.num_textures);
    TEST_ASSERT_EQUAL_UINT16(before.num_frame_buffers, released.num_frame_buffers);
    TEST_ASSERT_EQUAL_INT64(before.rt_memory_used, released.rt_memory_used);
}
static void test_noop_prepass_views_reuse_reject_and_reset_aliases(void)
{
    JceGpuStats before = create_noop();
    JceGpuStats owned = actual_stats();
    sr_ensure_ssao_target(scene_renderer, 4, 16, 8, false);
    const uint16_t scene_fbo = scene_renderer->ssao_depth_fbo.idx;
    const uint64_t scene_generation = scene_renderer->ssao_target_generation;
    sr_ensure_ssao_target(scene_renderer, 120, 8, 4, true);
    const uint16_t game_fbo = scene_renderer->ssao_depth_fbo.idx;
    JceGpuStats two = actual_stats();
    TEST_ASSERT_NOT_EQUAL(scene_fbo, game_fbo);
    TEST_ASSERT_EQUAL_UINT16(owned.num_textures + 7, two.num_textures);
    TEST_ASSERT_EQUAL_UINT16(owned.num_frame_buffers + 2, two.num_frame_buffers);
    for (unsigned i = 0; i < 8; ++i) {
        sr_ensure_ssao_target(scene_renderer, 4, 16, 8, false);
        TEST_ASSERT_EQUAL_UINT16(scene_fbo, scene_renderer->ssao_depth_fbo.idx);
        TEST_ASSERT_EQUAL_UINT64(scene_generation, scene_renderer->ssao_target_generation);
        TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, scene_renderer->ssao_velocity_tex.idx);
        sr_ensure_ssao_target(scene_renderer, 120, 8, 4, true);
        TEST_ASSERT_EQUAL_UINT16(game_fbo, scene_renderer->ssao_depth_fbo.idx);
    }
    JceGpuStats stable = actual_stats();
    TEST_ASSERT_EQUAL_UINT16(two.num_textures, stable.num_textures);
    TEST_ASSERT_EQUAL_UINT16(two.num_frame_buffers, stable.num_frame_buffers);
    TEST_ASSERT_EQUAL_UINT64(2, scene_renderer->prepass_targets.creates);
    /* Reject an unrepresentable viewport instead of submitting into old size. */
    sr_ensure_ssao_target(scene_renderer, 4, UINT32_MAX, 8, false);
    TEST_ASSERT_FALSE(scene_renderer->ssao_valid);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, scene_renderer->ssao_depth_fbo.idx);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, scene_renderer->ssao_depth_tex.idx);
    TEST_ASSERT_FALSE(scene_renderer->depth_prepass_frame);
    TEST_ASSERT_EQUAL_UINT32(1u, scene_renderer->prepass_target_reported_reasons);
    TEST_ASSERT_EQUAL_UINT16(two.num_textures, actual_stats().num_textures);
    sr_ensure_ssao_target(scene_renderer, 4, 16, 8, false);
    TEST_ASSERT_EQUAL_UINT16(scene_fbo, scene_renderer->ssao_depth_fbo.idx);
    /* Valid uint16 extents beyond the actual backend limit reject before create. */
    const uint32_t hardware_limit = bgfx_get_caps()->limits.maxTextureSize;
    TEST_ASSERT_TRUE(hardware_limit < UINT16_MAX);
    sr_ensure_ssao_target(scene_renderer, 4, hardware_limit + 1u, 8, false);
    TEST_ASSERT_FALSE(scene_renderer->ssao_valid);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, scene_renderer->ssao_depth_fbo.idx);
    TEST_ASSERT_EQUAL_UINT32(5u, scene_renderer->prepass_target_reported_reasons);
    TEST_ASSERT_EQUAL_UINT16(two.num_textures, actual_stats().num_textures);
    sr_ensure_ssao_target(scene_renderer, 120, 8, 4, true);
    TEST_ASSERT_EQUAL_UINT16(game_fbo, scene_renderer->ssao_depth_fbo.idx);
    /* Another successful view cannot rearm a persistent failure's WARN. */
    TEST_ASSERT_EQUAL_UINT32(5u, scene_renderer->prepass_target_reported_reasons);
    sr_ensure_ssao_target(scene_renderer, 4, hardware_limit + 1u, 8, false);
    TEST_ASSERT_EQUAL_UINT32(5u, scene_renderer->prepass_target_reported_reasons);
    sr_ensure_ssao_target(scene_renderer, 4, 16, 8, false);
    TEST_ASSERT_EQUAL_UINT16(scene_fbo, scene_renderer->ssao_depth_fbo.idx);
    jce_scene_renderer_reset_entity_caches(scene_renderer);
    JceGpuStats reset = actual_stats();
    TEST_ASSERT_EQUAL_UINT16(owned.num_textures, reset.num_textures);
    TEST_ASSERT_EQUAL_UINT16(owned.num_frame_buffers, reset.num_frame_buffers);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, scene_renderer->ssao_depth_fbo.idx);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, scene_renderer->ssao_normal_tex.idx);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, scene_renderer->ssao_albedo_tex.idx);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, scene_renderer->ssao_velocity_tex.idx);
    TEST_ASSERT_FALSE(scene_renderer->ssao_valid);
    TEST_ASSERT_EQUAL_UINT32(0, scene_renderer->prepass_target_reported_reasons);
    jce_scene_renderer_reset_entity_caches(scene_renderer);
    sr_ensure_ssao_target(scene_renderer, 4, 16, 8, false);
    TEST_ASSERT_GREATER_THAN_UINT64(scene_generation,
                                   scene_renderer->ssao_target_generation);
    jce_scene_renderer_destroy(scene_renderer); scene_renderer = NULL;
    JceGpuStats released = actual_stats();
    TEST_ASSERT_EQUAL_UINT16(before.num_textures, released.num_textures);
    TEST_ASSERT_EQUAL_UINT16(before.num_frame_buffers, released.num_frame_buffers);
    TEST_ASSERT_EQUAL_INT64(before.rt_memory_used, released.rt_memory_used);
}
static void test_noop_resolved_cfg_count_grows_beyond_global_rp(void)
{
    JceGpuStats before = create_noop();
    JceGpuStats one = render_targets(1, 1024);
    TEST_ASSERT_EQUAL_UINT16(before.num_frame_buffers + 1, one.num_frame_buffers);
    JceRenderPipelineDesc rp;
    jce_render_pipeline_get(&rp);
    TEST_ASSERT_EQUAL_UINT(1, rp.csm_cascade_count);
    JceGpuStats three = render_targets(3, 1024);
    TEST_ASSERT_EQUAL_UINT16(before.num_frame_buffers + 3, three.num_frame_buffers);
    TEST_ASSERT_GREATER_THAN_INT64(one.rt_memory_used, three.rt_memory_used);
}
static void test_noop_scene_authored_count_grows_beyond_global_rp(void)
{
    JceGpuStats before = create_noop();
    (void)render_targets(1, 1024);
    JceSceneRenderingSettings settings = jce_scene_rendering_settings_default();
    settings.cascade_count = 3;
    settings.shadow_resolution = 1024;
    jce_scene_set_rendering_settings(scene, &settings);
    JceGpuStats three = render_targets(0, 1024);
    TEST_ASSERT_EQUAL_UINT16(before.num_frame_buffers + 3, three.num_frame_buffers);
}
static void test_noop_unauthored_first_render_preserves_tier_fallback(void)
{
    JceGpuStats before = create_noop();
    jce_scene_clear_rendering_settings(scene);
    JceGpuTier tier = jce_renderer_get_tier();
    unsigned count = tier == JCE_GPU_TIER_LOW ? 1 :
                     tier == JCE_GPU_TIER_MEDIUM ? 2 : 4;
    JceGpuStats after = render_targets(0, 0);
    TEST_ASSERT_EQUAL_UINT16(before.num_frame_buffers + count,
                            after.num_frame_buffers);
}
static void test_noop_same_size_count_alternation_keeps_bounded_cache(void)
{
    JceGpuStats before = create_noop();
    (void)render_targets(1, 1024);
    JceGpuStats four = render_targets(4, 1024);
    TEST_ASSERT_EQUAL_UINT16(before.num_frame_buffers + 4, four.num_frame_buffers);
    for (unsigned i = 0; i < 8; ++i) {
        JceGpuStats current = render_targets((i & 1) ? 4 : 1, 1024);
        TEST_ASSERT_EQUAL_UINT16(four.num_frame_buffers, current.num_frame_buffers);
        TEST_ASSERT_EQUAL_INT64(four.rt_memory_used, current.rt_memory_used);
    }
}
static void test_noop_resize_and_destroy_restore_resource_baseline(void)
{
    JceGpuStats before = create_noop();
    JceGpuStats first = render_targets(3, 1024);
    JceGpuStats resized = render_targets(3, 512);
    TEST_ASSERT_EQUAL_UINT16(before.num_frame_buffers + 3, resized.num_frame_buffers);
    TEST_ASSERT_EQUAL_INT64((first.rt_memory_used - before.rt_memory_used) / 4,
                          resized.rt_memory_used - before.rt_memory_used);
    jce_scene_renderer_destroy(scene_renderer);
    scene_renderer = NULL;
    JceGpuStats released = actual_stats();
    TEST_ASSERT_EQUAL_UINT16(before.num_frame_buffers, released.num_frame_buffers);
    TEST_ASSERT_EQUAL_INT64(before.rt_memory_used, released.rt_memory_used);
}

/* Actual scene/PostFX request admission before any render call. Noop validates
 * policy and ownership only, not temporal pixels or GPU completion. */
static void test_noop_taa_first_projection_consumes_applied_pipeline(void)
{
    (void)create_noop();
    TEST_ASSERT_NOT_NULL(jce_scene_renderer_get_postfx(scene_renderer));
    TEST_ASSERT_FALSE(scene_renderer->rp_seen);
    jce_cvar_set_bool(scene_renderer->cv_taa, true); /* stale HIGH default */
    jce_mat4 clean = jce_m4_identity(), out;
    uint32_t frame_index = scene_renderer->taa_state.frame_index;
    TEST_ASSERT_FALSE(jce_scene_renderer_taa_begin_frame(
        scene_renderer, 32, 32, &clean, &clean, &out));
    TEST_ASSERT_TRUE(scene_renderer->rp_taa_seen);
    TEST_ASSERT_FALSE(jce_cvar_get_bool(scene_renderer->cv_taa));
    TEST_ASSERT_EQUAL_UINT32(frame_index, scene_renderer->taa_state.frame_index);
    TEST_ASSERT_EQUAL_MEMORY(&clean, &out, sizeof(clean));
    TEST_ASSERT_FALSE(scene_renderer->rp_seen); /* no shadow/MSAA latching */
}
static void test_noop_taa_stable_pipeline_preserves_console_override(void)
{
    (void)create_noop();
    TEST_ASSERT_FALSE(jce_scene_renderer_taa_requested(scene_renderer));
    jce_cvar_set_bool(scene_renderer->cv_taa, true);
    TEST_ASSERT_TRUE(jce_scene_renderer_taa_requested(scene_renderer));
    TEST_ASSERT_FALSE(jce_postfx_taa_supported()); /* real LOW HDR-off policy */
    jce_mat4 clean = jce_m4_identity(), out;
    uint32_t frame_index = scene_renderer->taa_state.frame_index;
    TEST_ASSERT_FALSE(jce_scene_renderer_taa_begin_frame(
        scene_renderer, 32, 32, &clean, &clean, &out));
    TEST_ASSERT_EQUAL_UINT32(frame_index, scene_renderer->taa_state.frame_index);
    TEST_ASSERT_EQUAL_MEMORY(&clean, &out, sizeof(clean));
    jce_cvar_set_bool(scene_renderer->cv_taa, false);
}

int main(void)
{
    /* Retain the precise failing control if backend teardown aborts before
     * UnityEnd can flush redirected stdout. This does not alter assertions. */
    setvbuf(stdout, NULL, _IONBF, 0);
    UNITY_BEGIN();
    RUN_TEST(test_valid_handle_zero_preserves_size_and_format);
    RUN_TEST(test_texture_failure_leaves_empty_pair);
    RUN_TEST(test_framebuffer_failure_does_not_retain_texture);
    RUN_TEST(test_retry_after_framebuffer_failure_has_one_texture_owner);
    RUN_TEST(test_existing_pair_is_reused_without_allocation);
    RUN_TEST(test_partial_pair_is_retired_before_replacement);
    RUN_TEST(test_destroy_is_idempotent_including_handle_zero);
    RUN_TEST(test_texture_failure_can_retry_without_spurious_destroy);
    RUN_TEST(test_dynamic_cold_requests_require_every_need_predicate);
    RUN_TEST(test_dynamic_failure_retries_and_idle_preserves_existing_pair);
    RUN_TEST(test_noop_disabled_render_resolves_policy_without_allocating);
    RUN_TEST(test_noop_dynamic_atlas_owner_creates_on_demand_and_retains_idle);
    RUN_TEST(test_noop_constructor_defers_shadow_depth_targets);
    RUN_TEST(test_noop_cold_scene_teardown_preserves_backend_texture_baseline);
    RUN_TEST(test_noop_lazy_handles_begin_invalid);
    RUN_TEST(test_noop_velocity_queue_release_and_scene_recreation);
    RUN_TEST(test_noop_depth_queue_reuse_isolation_and_cpu_release);
    RUN_TEST(test_noop_depth_queue_scene_destroy_and_recreation);
    RUN_TEST(test_noop_cold_scene_teardown_restores_uniform_handle_baseline);
    RUN_TEST(test_noop_prepass_targets_resize_velocity_and_destroy_restore_baseline);
    RUN_TEST(test_noop_prepass_views_reuse_reject_and_reset_aliases);
    RUN_TEST(test_noop_resolved_cfg_count_grows_beyond_global_rp);
    RUN_TEST(test_noop_scene_authored_count_grows_beyond_global_rp);
    RUN_TEST(test_noop_unauthored_first_render_preserves_tier_fallback);
    RUN_TEST(test_noop_same_size_count_alternation_keeps_bounded_cache);
    RUN_TEST(test_noop_resize_and_destroy_restore_resource_baseline);
    RUN_TEST(test_noop_taa_first_projection_consumes_applied_pipeline);
    RUN_TEST(test_noop_taa_stable_pipeline_preserves_console_override);
    return UNITY_END();
}
