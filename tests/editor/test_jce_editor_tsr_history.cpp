/* Exact Scene TSR source blocks and actual scene/camera transition entries.
 * Unity reports ordinary assertion failures. Only SDK boundary ports are fake;
 * scene state, camera projection, Halton and matrix math are production code.
 * This is CPU submission/history admission evidence, never GPU image proof. */
#include "unity.h"
#include "scene/jce_scene_render_internal.h"
#include <jce/os/core/jce_console.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_taa.h>
#include <jce/renderer/jce_views.h>
#include <jce/middleware/animation/jce_animation.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "constants.inc.h"

SceneRenderState s_sr;
#include "query_cache.inc.h"
static struct { bool busy_this_frame; } s_probe_maintenance;
static uint16_t s_view_id_override = UINT16_MAX;
static JceCvar *upscaler;
static JceScene *provided_scene;
static JcePlayState provided_play;
static uint32_t provided_frame;
static uint64_t provided_context;
static unsigned resolve_calls, orbit_calls, model_invalidations;
static bool resolve_ok, pipeline_available, source_available;
static uint32_t render_width, render_height, output_width, output_height;
static bool provided_taa;
struct ResolveObservation {
    float feedback;
    bool have_previous;
    uint16_t external_motion;
    float previous[16];
};
static ResolveObservation last_resolve;
static unsigned registration_calls, unregistration_calls, registration_errors;
static bool refuse_registration, token_cleared_before_unregister;
static bool listener_removed_before_release;
struct ReleaseCounts {
    unsigned canvas, pick, renderer, player, model, offscreen, camera;
    unsigned texture, program, uniform, streamer, filesystem, occlusion, cache;
};
static ReleaseCounts released;

/* These are typed constructor/destructor boundaries, not a second cleanup
 * implementation. The exact init guard/tail and complete actual shutdown own
 * the transaction. Registration/dispatch/removal use the real lifecycle.c. */
static JceLifecycleHandle register_port(JceLifecycleCallback callback,
    int32_t priority, void *user)
{
    ++registration_calls;
    if (refuse_registration) return {};
    return jce_lifecycle_register(callback, priority, user);
}
static void unregister_port(JceLifecycleHandle listener)
{
    ++unregistration_calls;
    token_cleared_before_unregister &= s_sr.temporal_listener.id == 0;
    jce_lifecycle_unregister(listener);
}
static void observe_release(void)
{
    const bool saved = s_sr.tsr_history.valid;
    s_sr.tsr_history.valid = true;
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
    listener_removed_before_release &= s_sr.temporal_listener.id == 0 &&
                                      s_sr.tsr_history.valid;
    s_sr.tsr_history.valid = saved;
}
static void canvas_destroy_port(JceUICanvas *)
{ observe_release(); ++released.canvas; }
static void pick_destroy_port(JceScenePickPass *)
{ observe_release(); ++released.pick; }
static void renderer_destroy_port(JceSceneRenderer *)
{ observe_release(); ++released.renderer; }
static void player_destroy_port(JceAnimPlayer *)
{ observe_release(); ++released.player; }
static void model_destroy_port(JceModel *)
{ observe_release(); ++released.model; }
static void offscreen_destroy_port(JceOffscreenTarget *)
{ observe_release(); ++released.offscreen; }
static void camera_destroy_port(JceCamera *camera)
{ observe_release(); ++released.camera; jce_camera_destroy(camera); }
static void texture_destroy_port(JceTextureHandle)
{ observe_release(); ++released.texture; }
static void program_destroy_port(JceProgramHandle)
{ observe_release(); ++released.program; }
static void uniform_destroy_port(JceUniformHandle)
{ observe_release(); ++released.uniform; }
static void streamer_destroy_port(JceWorldStreamer *)
{ observe_release(); ++released.streamer; }
static void filesystem_destroy_port(JceFileSystem *)
{ observe_release(); ++released.filesystem; }
static void occlusion_destroy_port(JceOcclusionCuller *)
{ observe_release(); ++released.occlusion; }
static void asset_shutdown_port(void)
{ observe_release(); ++released.cache; }
static void cancel_probe_port(void) { observe_release(); }

static JceScene *scene_port(void) { return provided_scene; }
static JcePlayState play_port(void) { return provided_play; }
static uint32_t frame_port(const JceRenderer *) { return provided_frame; }
static bool payload_port(const JceRenderer *, JceSubmittedPayloadStats *out)
{
    if (!out) return false;
    *out = {};
    out->valid = true;
    out->context_generation = provided_context;
    out->frame_index = provided_frame;
    return true;
}
static JcePostFXPipeline *postfx_port(JceSceneRenderer *)
{
    return pipeline_available ? reinterpret_cast<JcePostFXPipeline *>(uintptr_t(5)) : nullptr;
}
static uint16_t color_port(const JceOffscreenTarget *)
{ return source_available ? 17 : UINT16_MAX; }
static uint16_t depth_port(const JceOffscreenTarget *) { return 18; }
static uint16_t velocity_port(const JceSceneRenderer *) { return 19; }
static void model_invalidate_port(JceSceneRenderer *) { ++model_invalidations; }
static void pick_invalidate_port(JceScenePickPass *) {}
static void occlusion_reset_port(void) {}
static uint16_t resolve_port(JcePostFXPipeline *, uint16_t, JceTextureHandle,
    JceTextureHandle, const float *inverse, const float *previous, JceTextureHandle motion,
    uint32_t, uint32_t, uint32_t, uint32_t, float, float, float feedback, bool)
{
    ++resolve_calls;
    last_resolve = {};
    last_resolve.feedback = feedback;
    last_resolve.have_previous = inverse && previous;
    last_resolve.external_motion = motion.idx;
    if (previous) std::memcpy(last_resolve.previous, previous, sizeof(last_resolve.previous));
    return resolve_ok ? 23 : UINT16_MAX;
}

#define jce_state_get_scene scene_port
#define jce_state_get_play_state play_port
#define jce_renderer_get_frame_index frame_port
#define jce_renderer_get_submitted_payload_stats payload_port
#define jce_scene_renderer_get_postfx postfx_port
#define jce_offscreen_target_get_color_texture color_port
#define jce_offscreen_target_get_depth_texture depth_port
#define jce_scene_renderer_get_velocity_texture velocity_port
#define jce_postfx_tsr_resolve resolve_port
#define jce_scene_renderer_invalidate_model_cache model_invalidate_port
#define jce_scene_pick_invalidate_model_cache pick_invalidate_port
#define jce_editor_scene_render_reset_occlusion occlusion_reset_port
#define jce_lifecycle_register register_port
#define jce_lifecycle_unregister unregister_port
#define jce_ui_canvas_destroy canvas_destroy_port
#define jce_scene_pick_destroy pick_destroy_port
#define jce_scene_renderer_destroy renderer_destroy_port
#define jce_anim_player_destroy player_destroy_port
#define jce_model_destroy model_destroy_port
#define jce_offscreen_target_destroy offscreen_destroy_port
#define jce_camera_destroy camera_destroy_port
#define jce_gfx_texture_destroy texture_destroy_port
#define jce_program_destroy program_destroy_port
#define jce_uniform_destroy uniform_destroy_port
#define jce_world_streamer_destroy streamer_destroy_port
#define jce_fs_destroy filesystem_destroy_port
#define jce_occlusion_culler_destroy occlusion_destroy_port
#define jce_editor_scene_asset_cache_shutdown asset_shutdown_port
#define jce_editor_scene_render_cancel_probe_capture cancel_probe_port
#undef LOG_INFO
#undef LOG_ERROR
#define LOG_INFO(...) ((void)0)
#define LOG_ERROR(...) (++registration_errors)

uint16_t scene_view_id(void) { return JCE_VIEW_EDITOR_SCENE; }
void orbit_apply(void) { ++orbit_calls; }
static void orbit_cancel_focus_anim(void) {}

#include "lifecycle.inc.h"
#include "view_destroy.inc.h"
#include "init.inc.h"
#include "shutdown.inc.h"
#include "scene_invalidate.inc.h"
#include "camera_set.inc.h"
#include "camera_projection.inc.h"

/* Only declarations/adapters surround these exact real production blocks. */
static void production_frame(void)
{
    ++provided_frame;
    uint32_t width = render_width, height = render_height;
    const uint32_t disp_w = output_width, disp_h = output_height;
    const bool taa_on = provided_taa;
    JceScene *scene = provided_scene;
    jce_mat4 view = jce_camera_view(s_sr.camera);
    jce_mat4 proj = jce_camera_proj(s_sr.camera, float(width) / float(height), false);
    jce_mat4 color_proj = proj;
#include "prepare.inc.h"
#include "resolve.inc.h"
    (void)scene;
}

/* Real GPU constructors are deliberately outside this CPU fixture. Their
 * successful outputs are prestaged; no registration failure is called OOM. */
static void stage_resources(bool complete)
{
    s_sr = {};
    s_sr.white_tex.idx = s_sr.prog_grid.idx = UINT16_MAX;
    s_sr.u_grid_camera.idx = s_sr.u_grid_fade.idx = UINT16_MAX;
    s_sr.u_light_dir.idx = s_sr.u_light_color.idx = UINT16_MAX;
    s_sr.renderer = reinterpret_cast<JceRenderer *>(uintptr_t(1));
    s_sr.scene_renderer = reinterpret_cast<JceSceneRenderer *>(uintptr_t(2));
    s_sr.bridge = reinterpret_cast<JceOffscreenTarget *>(uintptr_t(3));
    s_sr.present = reinterpret_cast<JceOffscreenTarget *>(uintptr_t(4));
    JceCameraDesc desc{};
    desc.position = jce_v3(0, 2, 8);
    desc.target = jce_v3(0, 0, 0);
    desc.up = jce_v3(0, 1, 0);
    desc.fov_deg = 60;
    desc.near_plane = .1f;
    desc.far_plane = 1000;
    s_sr.camera = jce_camera_create(&desc);
    TEST_ASSERT_NOT_NULL(s_sr.camera);
    if (complete) {
        s_sr.ui_canvas = reinterpret_cast<JceUICanvas *>(uintptr_t(20));
        s_sr.pick_pass = reinterpret_cast<JceScenePickPass *>(uintptr_t(21));
        s_sr.world_streamer = reinterpret_cast<JceWorldStreamer *>(uintptr_t(22));
        s_sr.stream_fs = reinterpret_cast<JceFileSystem *>(uintptr_t(23));
        s_sr.occlusion_culler = reinterpret_cast<JceOcclusionCuller *>(uintptr_t(24));
        s_sr.white_tex.idx = 25; s_sr.prog_grid.idx = 26;
        s_sr.u_grid_camera.idx = 27; s_sr.u_grid_fade.idx = 28;
        s_sr.u_light_dir.idx = 29; s_sr.u_light_color.idx = 30;
        s_query_cache[0].used = true;
        s_query_cache[0].player = reinterpret_cast<JceAnimPlayer *>(uintptr_t(31));
        s_query_cache[0].model = reinterpret_cast<JceModel *>(uintptr_t(32));
    }
}
void setUp(void)
{
    jce_lifecycle_shutdown();
    registration_calls = unregistration_calls = registration_errors = 0;
    refuse_registration = false;
    token_cleared_before_unregister = listener_removed_before_release = true;
    released = {};
    std::memset(s_query_cache, 0, sizeof(s_query_cache));
    stage_resources(false);
    provided_scene = reinterpret_cast<JceScene *>(uintptr_t(11));
    provided_play = JCE_PLAY_STOPPED;
    provided_frame = 100;
    provided_context = 7;
    resolve_calls = orbit_calls = model_invalidations = 0;
    last_resolve = {};
    resolve_ok = pipeline_available = source_available = true;
    render_width = 640; render_height = 360;
    output_width = 1280; output_height = 720;
    provided_taa = false;
    s_view_id_override = UINT16_MAX;
    if (!upscaler)
        upscaler = jce_cvar_register_int("r.upscaler", 1, JCE_CVAR_FLAG_NONE, "TSR fixture");
    TEST_ASSERT_NOT_NULL(upscaler);
    jce_cvar_set_int(upscaler, 2);
    TEST_ASSERT_TRUE(jce_editor_scene_render_init(s_sr.renderer, nullptr, nullptr));
}
void tearDown(void)
{
    jce_editor_scene_render_shutdown();
    jce_lifecycle_shutdown();
}
static void warm_history(void)
{
    production_frame();
    production_frame();
    TEST_ASSERT_EQUAL_UINT(2, resolve_calls);
    TEST_ASSERT_FLOAT_WITHIN(.000001f, .9f, last_resolve.feedback);
    TEST_ASSERT_TRUE(last_resolve.have_previous);
}
static void expect_new_history(void)
{
    const unsigned before = resolve_calls;
    production_frame();
    TEST_ASSERT_EQUAL_UINT(before + 1, resolve_calls);
    TEST_ASSERT_FLOAT_WITHIN(.000001f, 0.0f, last_resolve.feedback);
    TEST_ASSERT_FALSE(last_resolve.have_previous);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, last_resolve.external_motion);
}
static void test_continuous_motion_retains_existing_feedback_and_previous_camera(void)
{
    warm_history();
    jce_mat4 view = jce_camera_view(s_sr.camera);
    jce_mat4 proj = jce_camera_proj(s_sr.camera, float(render_width) / render_height, false);
    jce_mat4 previous = jce_m4_multiply(&proj, &view);
    TEST_ASSERT_TRUE(jce_camera_set_pose(s_sr.camera, jce_v3(2, 2, 8),
        jce_v3(0, 0, -1), jce_v3(0, 1, 0)));
    production_frame();
    TEST_ASSERT_FLOAT_WITHIN(.000001f, .9f, last_resolve.feedback);
    TEST_ASSERT_TRUE(last_resolve.have_previous);
    TEST_ASSERT_EQUAL_FLOAT_ARRAY(previous.raw[0], last_resolve.previous, 16);
}
static void test_same_address_scene_replacement_uses_actual_invalidation_entry(void)
{
    warm_history();
    jce_editor_scene_render_invalidate_model_caches();
    TEST_ASSERT_EQUAL_UINT(1, model_invalidations);
    expect_new_history();
}
static void test_direct_camera_cut_uses_actual_camera_entry(void)
{
    warm_history();
    const float target[] = {20, 4, -8};
    jce_editor_scene_camera_set_state(target, .5f, .25f, 12);
    TEST_ASSERT_EQUAL_UINT(1, orbit_calls);
    TEST_ASSERT_EQUAL_FLOAT(20, s_sr.orbit_target.x);
    expect_new_history();
}
static void test_projection_change_uses_actual_projection_entry(void)
{
    warm_history();
    jce_editor_scene_camera_set_projection(true, 45, .5f, 300, 0, 24);
    TEST_ASSERT_EQUAL_INT(JCE_CAMERA_ORTHO, jce_camera_get_mode(s_sr.camera));
    expect_new_history();
}
static void test_play_and_stop_each_start_new_history_at_same_extent(void)
{
    warm_history();
    provided_play = JCE_PLAY_PLAYING;
    expect_new_history();
    production_frame();
    TEST_ASSERT_FLOAT_WITHIN(.000001f, .9f, last_resolve.feedback);
    provided_play = JCE_PLAY_STOPPED;
    expect_new_history();
}
static void test_input_extent_change_starts_new_history(void)
{
    warm_history();
    render_width = 480; render_height = 270;
    expect_new_history();
}
static void test_output_extent_change_starts_new_history(void)
{
    warm_history();
    output_width = 1600; output_height = 900;
    expect_new_history();
}
static void test_disabled_then_reenabled_tsr_does_not_reuse_old_history(void)
{
    warm_history();
    jce_cvar_set_int(upscaler, 1);
    production_frame();
    TEST_ASSERT_EQUAL_UINT(2, resolve_calls);
    jce_cvar_set_int(upscaler, 2);
    expect_new_history();
}
static void test_taa_gate_then_tsr_reentry_starts_new_history(void)
{
    warm_history();
    provided_taa = true;
    production_frame();
    TEST_ASSERT_EQUAL_UINT(2, resolve_calls);
    provided_taa = false;
    expect_new_history();
}
static void test_skipped_submission_and_context_replacement_start_new_history(void)
{
    warm_history();
    provided_frame += 2;
    expect_new_history();
    production_frame();
    ++provided_context;
    expect_new_history();
}
static void test_failed_resolve_keeps_cut_pending_until_success(void)
{
    warm_history();
    jce_editor_scene_render_invalidate_model_caches();
    resolve_ok = false;
    production_frame();
    TEST_ASSERT_EQUAL_UINT(3, resolve_calls);
    resolve_ok = true;
    expect_new_history();
    production_frame();
    TEST_ASSERT_FLOAT_WITHIN(.000001f, .9f, last_resolve.feedback);
}
static void test_missing_pipeline_is_not_a_committed_previous_frame(void)
{
    warm_history();
    pipeline_available = false;
    production_frame();
    TEST_ASSERT_EQUAL_UINT(2, resolve_calls);
    pipeline_available = true;
    expect_new_history();
}
static void test_real_lifecycle_events_invalidate_without_a_submitted_frame(void)
{
    warm_history();
    const JceLifecycleEvent events[] = {
        JCE_LIFECYCLE_FOCUS_LOST, JCE_LIFECYCLE_FOCUS_GAINED,
        JCE_LIFECYCLE_PAUSE, JCE_LIFECYCLE_RESUME,
        JCE_LIFECYCLE_DEVICE_LOST, JCE_LIFECYCLE_DEVICE_RESET
    };
    for (JceLifecycleEvent event : events) {
        const uint32_t frame = provided_frame;
        jce_lifecycle_emit(event);
        TEST_ASSERT_EQUAL_UINT32(frame, provided_frame);
        TEST_ASSERT_FALSE(s_sr.tsr_history.valid);
        expect_new_history();
        production_frame();
        TEST_ASSERT_FLOAT_WITHIN(.000001f, .9f, last_resolve.feedback);
        TEST_ASSERT_TRUE(last_resolve.have_previous);
    }
}
static void test_unrelated_lifecycle_event_preserves_continuous_history(void)
{
    warm_history();
    jce_lifecycle_emit(JCE_LIFECYCLE_LOW_MEMORY);
    production_frame();
    TEST_ASSERT_FLOAT_WITHIN(.000001f, .9f, last_resolve.feedback);
    TEST_ASSERT_TRUE(last_resolve.have_previous);
}
static void test_repeated_init_keeps_one_listener_and_existing_history(void)
{
    warm_history();
    const uint32_t token = s_sr.temporal_listener.id;
    TEST_ASSERT_NOT_EQUAL(0, token);
    TEST_ASSERT_TRUE(jce_editor_scene_render_init(s_sr.renderer, nullptr, nullptr));
    TEST_ASSERT_EQUAL_UINT32(token, s_sr.temporal_listener.id);
    TEST_ASSERT_EQUAL_UINT(1, registration_calls);
    TEST_ASSERT_EQUAL_UINT(0, unregistration_calls);
    production_frame();
    TEST_ASSERT_FLOAT_WITHIN(.000001f, .9f, last_resolve.feedback);
}
static void assert_complete_release(void)
{
    TEST_ASSERT_FALSE(s_sr.initialized);
    TEST_ASSERT_EQUAL_UINT32(0, s_sr.temporal_listener.id);
    TEST_ASSERT_TRUE(token_cleared_before_unregister);
    TEST_ASSERT_TRUE(listener_removed_before_release);
    TEST_ASSERT_NULL(s_sr.ui_canvas);
    TEST_ASSERT_NULL(s_sr.pick_pass);
    TEST_ASSERT_NULL(s_sr.scene_renderer);
    TEST_ASSERT_NULL(s_sr.present);
    TEST_ASSERT_NULL(s_sr.bridge);
    TEST_ASSERT_NULL(s_sr.camera);
    TEST_ASSERT_NULL(s_sr.world_streamer);
    TEST_ASSERT_NULL(s_sr.stream_fs);
    TEST_ASSERT_NULL(s_sr.occlusion_culler);
    TEST_ASSERT_EQUAL_PTR(reinterpret_cast<JceRenderer *>(uintptr_t(1)), s_sr.renderer);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, s_sr.white_tex.idx);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, s_sr.prog_grid.idx);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, s_sr.u_grid_camera.idx);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, s_sr.u_grid_fade.idx);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, s_sr.u_light_dir.idx);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, s_sr.u_light_color.idx);
    TEST_ASSERT_FALSE(s_query_cache[0].used);
    TEST_ASSERT_NULL(s_query_cache[0].player);
    TEST_ASSERT_NULL(s_query_cache[0].model);
    TEST_ASSERT_EQUAL_UINT(1, released.canvas);
    TEST_ASSERT_EQUAL_UINT(1, released.pick);
    TEST_ASSERT_EQUAL_UINT(1, released.renderer);
    TEST_ASSERT_EQUAL_UINT(1, released.player);
    TEST_ASSERT_EQUAL_UINT(1, released.model);
    TEST_ASSERT_EQUAL_UINT(2, released.offscreen);
    TEST_ASSERT_EQUAL_UINT(1, released.camera);
    TEST_ASSERT_EQUAL_UINT(1, released.texture);
    TEST_ASSERT_EQUAL_UINT(1, released.program);
    TEST_ASSERT_EQUAL_UINT(4, released.uniform);
    TEST_ASSERT_EQUAL_UINT(1, released.streamer);
    TEST_ASSERT_EQUAL_UINT(1, released.filesystem);
    TEST_ASSERT_EQUAL_UINT(1, released.occlusion);
    TEST_ASSERT_EQUAL_UINT(1, released.cache);
}
static void test_shutdown_unsubscribes_before_any_release_and_allows_reinit(void)
{
    jce_editor_scene_render_shutdown();
    stage_resources(true);
    TEST_ASSERT_TRUE(jce_editor_scene_render_init(s_sr.renderer, nullptr, nullptr));
    const uint32_t token = s_sr.temporal_listener.id;
    const unsigned unregister_before = unregistration_calls;
    released = {};
    warm_history();
    jce_editor_scene_render_shutdown();
    assert_complete_release();
    TEST_ASSERT_EQUAL_UINT(unregister_before + 1, unregistration_calls);
    s_sr.tsr_history.valid = true;
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
    TEST_ASSERT_TRUE(s_sr.tsr_history.valid);
    const ReleaseCounts once = released;
    jce_editor_scene_render_shutdown();
    TEST_ASSERT_EQUAL_MEMORY(&once, &released, sizeof(released));
    TEST_ASSERT_EQUAL_UINT(unregister_before + 1, unregistration_calls);
    stage_resources(false);
    TEST_ASSERT_TRUE(jce_editor_scene_render_init(s_sr.renderer, nullptr, nullptr));
    TEST_ASSERT_NOT_EQUAL(0, s_sr.temporal_listener.id);
    TEST_ASSERT_NOT_EQUAL(token, s_sr.temporal_listener.id);
    s_sr.tsr_history.valid = true;
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);
    TEST_ASSERT_FALSE(s_sr.tsr_history.valid);
}
static void test_registration_refusal_rolls_back_all_owners_and_can_retry(void)
{
    jce_editor_scene_render_shutdown();
    stage_resources(true);
    released = {};
    const unsigned unregister_before = unregistration_calls;
    refuse_registration = true; /* API-zero port; not an allocator OOM claim. */
    TEST_ASSERT_FALSE(jce_editor_scene_render_init(s_sr.renderer, nullptr, nullptr));
    TEST_ASSERT_EQUAL_UINT(1, registration_errors);
    TEST_ASSERT_EQUAL_UINT(unregister_before, unregistration_calls);
    assert_complete_release();
    s_sr.tsr_history.valid = true;
    jce_lifecycle_emit(JCE_LIFECYCLE_DEVICE_LOST);
    TEST_ASSERT_TRUE(s_sr.tsr_history.valid);
    refuse_registration = false;
    stage_resources(false);
    TEST_ASSERT_TRUE(jce_editor_scene_render_init(s_sr.renderer, nullptr, nullptr));
    TEST_ASSERT_NOT_EQUAL(0, s_sr.temporal_listener.id);
    s_sr.tsr_history.valid = true;
    jce_lifecycle_emit(JCE_LIFECYCLE_RESUME);
    TEST_ASSERT_FALSE(s_sr.tsr_history.valid);
}
int main(void)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    UNITY_BEGIN();
    RUN_TEST(test_continuous_motion_retains_existing_feedback_and_previous_camera);
    RUN_TEST(test_same_address_scene_replacement_uses_actual_invalidation_entry);
    RUN_TEST(test_direct_camera_cut_uses_actual_camera_entry);
    RUN_TEST(test_projection_change_uses_actual_projection_entry);
    RUN_TEST(test_play_and_stop_each_start_new_history_at_same_extent);
    RUN_TEST(test_input_extent_change_starts_new_history);
    RUN_TEST(test_output_extent_change_starts_new_history);
    RUN_TEST(test_disabled_then_reenabled_tsr_does_not_reuse_old_history);
    RUN_TEST(test_taa_gate_then_tsr_reentry_starts_new_history);
    RUN_TEST(test_skipped_submission_and_context_replacement_start_new_history);
    RUN_TEST(test_failed_resolve_keeps_cut_pending_until_success);
    RUN_TEST(test_missing_pipeline_is_not_a_committed_previous_frame);
    RUN_TEST(test_real_lifecycle_events_invalidate_without_a_submitted_frame);
    RUN_TEST(test_unrelated_lifecycle_event_preserves_continuous_history);
    RUN_TEST(test_repeated_init_keeps_one_listener_and_existing_history);
    RUN_TEST(test_shutdown_unsubscribes_before_any_release_and_allows_reinit);
    RUN_TEST(test_registration_refusal_rolls_back_all_owners_and_can_retry);
    const int result = UNITY_END();
    jce_console_shutdown();
    return result;
}
