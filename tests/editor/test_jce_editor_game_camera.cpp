// Actual GameRender camera ownership and Play transaction consumers; CPU only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "scene/jce_editor_game_render.h"
#include "core/jce_editor_play_transaction.h"
#include "core/jce_editor_play_presentation.h"
#include "core/jce_editor_operation_metrics.h"
#include "core/jce_editor_viewport_trace.h"
#include "core/jce_editor_frame_observation.h"
#include <jce/application/jce_runtime_stats_host.h>
#include <jce/middleware/audio/jce_audio.h>
#include <jce/middleware/scene/jce_scene_camera.h>
#include <jce/middleware/scene/jce_vcam_system.h>
#include <jce/middleware/scene/jce_ui_canvas.h>
#include <jce/os/platform/jce_window.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_lowlevel.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_occlusion_culler.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/renderer/jce_taa.h>
#include <jce/renderer/jce_views.h>
#include <type_traits>
#include <algorithm>
#include <cstring>
#include <vector>

static std::vector<JceCamera *> owned;
static unsigned create_attempts, destroys, bridge_destroys, runtime_creates;
static unsigned restores, closes, presentation_restores, primary_calls, draws;
static unsigned hook_clears, optional_destroys, logs;
static bool camera_failure, bridge_failure, runtime_failure, restore_success;
static int play_state;
static JceCamera *applied_camera;
static bool authored_ortho;
static bool player_available, vcam_available, primary_available;
static JceRuntime *fake_runtime = reinterpret_cast<JceRuntime *>(uintptr_t(1));
static JceRenderer *fake_renderer = reinterpret_cast<JceRenderer *>(uintptr_t(2));
static JceScene *fake_scene = reinterpret_cast<JceScene *>(uintptr_t(3));
static JceOffscreenTarget *fake_bridge = reinterpret_cast<JceOffscreenTarget *>(uintptr_t(4));
static JceSceneRenderer *fake_scene_renderer = reinterpret_cast<JceSceneRenderer *>(uintptr_t(5));

static JceCamera *camera_create_port(const JceCameraDesc *desc)
{
    ++create_attempts;
    if (camera_failure) return nullptr;
    JceCamera *camera = jce_camera_create(desc);
    if (camera) owned.push_back(camera);
    return camera;
}
static JceCamera *camera_clone_port(const JceCamera *source)
{
    ++create_attempts;
    if (camera_failure) return nullptr;
    JceCamera *camera = jce_camera_clone(source);
    if (camera) owned.push_back(camera);
    return camera;
}
static void camera_destroy_port(JceCamera *camera)
{
    auto it = std::find(owned.begin(), owned.end(), camera);
    CHECK(it != owned.end());
    if (it == owned.end()) return;
    ++destroys;
    owned.erase(it);
    jce_camera_destroy(camera);
}
static JceGfxCaps JCE_CALL caps_port() { return {}; }
static JceOffscreenTarget *bridge_create_port(JceRenderer *r, uint16_t)
{ CHECK(r == fake_renderer); return bridge_failure ? nullptr : fake_bridge; }
static void bridge_destroy_port(JceOffscreenTarget *p)
{ CHECK(p == fake_bridge); ++bridge_destroys; }
static void JCE_CALL hook_port(JceAutoCaptureFn fn, void *user)
{ CHECK(fn == nullptr); CHECK(user == nullptr); ++hook_clears; }
static void mouse_rect_port(JceWindow *, int, int, int, int) {}
static void relative_port(JceWindow *, bool) {}
static void grab_port(JceWindow *, bool) {}
static void culler_destroy_port(JceOcclusionCuller *) { ++optional_destroys; }
static void canvas_destroy_port(JceUICanvas *) { ++optional_destroys; }
static void postfx_destroy_port(JcePostFXPipeline *) { ++optional_destroys; }
static JceSceneRenderer *scene_renderer_port() { return fake_scene_renderer; }
static JceScene *scene_port() { return fake_scene; }
static int state_port() { return play_state; }
static float dt_port() { return 1.0f / 60.0f; }
static void module_draw_port() { ++draws; }
static bool JCE_CALL player_port(const JceRuntime *, jce_vec3 *position)
{ *position = jce_v3(1, 2, 3); return player_available; }
static JceSceneCameraResolveResult primary_port(JceScene *, JceCamera *camera, JceSceneCameraPose *)
{
    ++primary_calls; applied_camera = camera;
    if (!primary_available) return JCE_SCENE_CAMERA_RESOLVE_NOT_FOUND;
    jce_camera_set_mode(camera, authored_ortho ? JCE_CAMERA_ORTHO : JCE_CAMERA_PERSPECTIVE);
    jce_camera_set_ortho_height(camera, 42.0f);
    jce_camera_set_fov(camera, 35.0f);
    jce_camera_set_near_far(camera, 0.7f, 777.0f);
    CHECK(jce_camera_set_pose(camera, jce_v3(30, 40, 50), jce_v3(0, -1, 0), jce_v3(1, 0, 0)));
    return JCE_SCENE_CAMERA_RESOLVE_OK;
}
static void JCE_CALL vcam_port(JceScene *, float, JceVcamOutput *out, bool *present)
{
    *present = vcam_available;
    *out = {};
    out->position[0] = 3; out->position[1] = 4; out->position[2] = 5;
    out->target[0] = 0; out->target[1] = 0; out->target[2] = 0;
    out->fov_deg = 24;
}
static void JCE_CALL vcam_reset_port() {}
static_assert(std::is_same_v<decltype(&player_port), decltype(&jce_runtime_get_player_position)>);
static_assert(std::is_same_v<decltype(&primary_port), decltype(&jce_scene_camera_apply_primary)>);
static_assert(std::is_same_v<decltype(&vcam_port), decltype(&jce_vcam_system_evaluate)>);
constexpr uint16_t JCE_EDITOR_GAME_VIEW_BASE = JCE_VIEW_EDITOR_GAME;
#define LOG_TAG "game-camera-test"
#define LOG_WARN(...) (++logs)
#define LOG_ERROR(...) (++logs)
#define jce_camera_clone camera_clone_port
#define jce_camera_create camera_create_port
#define jce_camera_destroy camera_destroy_port
#define jce_gfx_caps caps_port
#define jce_offscreen_target_create bridge_create_port
#define jce_offscreen_target_destroy bridge_destroy_port
#define jce_renderer_set_auto_capture_hook hook_port
#define jce_window_set_mouse_rect mouse_rect_port
#define jce_window_set_relative_mouse_mode relative_port
#define jce_window_set_mouse_grab grab_port
#define jce_occlusion_culler_destroy culler_destroy_port
#define jce_ui_canvas_destroy canvas_destroy_port
#define jce_postfx_destroy postfx_destroy_port
#define jce_editor_get_scene_renderer scene_renderer_port
#define jce_state_get_scene scene_port
#define jce_state_get_play_state state_port
#define jce_editor_play_render_delta dt_port
#define jce_editor_play_draw_module module_draw_port
#define jce_runtime_get_player_position player_port
#define jce_scene_camera_apply_primary primary_port
#define jce_vcam_system_evaluate vcam_port
#define jce_vcam_system_reset vcam_reset_port
#include JCE_EDITOR_GAME_CAMERA_OWNER
#undef jce_camera_clone
#undef jce_camera_create
#undef jce_camera_destroy

struct EditorHistorySnapshot {};
static EditorHistorySnapshot s_play_snapshot;
static bool s_play_snapshot_valid;
static struct {
    JceRuntimeStatsHost runtime_stats{};
    JceEditorPlayPresentationState presentation{};
} s_play_host;
static JceAudio *s_play_audio;
static float observed_feedback;
static unsigned applies;
static void taa_port(JcePostFXPipeline *, bool enabled, float feedback, float, float)
{ CHECK(enabled); observed_feedback = feedback; }
static void apply_port(JcePostFXPipeline *, JceTextureHandle, JceTextureHandle) { ++applies; }
#define jce_postfx_set_taa taa_port
#define jce_postfx_apply apply_port
static void actual_taa_bind()
{
    const float taa_fb = 0.9f, taa_lc = 1, taa_mc = 1;
#include JCE_EDITOR_GAME_CAMERA_TAA_WIRE
}
static void actual_postfx_apply()
{
    JceTextureHandle game_color{1}, game_depth{2};
#include JCE_EDITOR_GAME_CAMERA_APPLY_WIRE
}
#undef jce_postfx_set_taa
#undef jce_postfx_apply
static JceRuntime *JCE_CALL runtime_create_port(const JceRuntimeDesc *, JcePlatformServices *, const JceRuntimeCreateOptions *, size_t)
{ ++runtime_creates; CHECK(g.play_camera != nullptr); return runtime_failure ? nullptr : fake_runtime; }
static_assert(std::is_same_v<decltype(&runtime_create_port), decltype(&jce_runtime_create_with_options)>);
static void presentation_restore_port(JceEditorPlayPresentationState &) { ++presentation_restores; }
static bool stats_close_port(JceRuntimeStatsHost *host)
{ ++closes; CHECK(host->runtime == nullptr); return true; }
static bool history_restore_snapshot(EditorHistorySnapshot &, const char *)
{ ++restores; return restore_success; }
static void audio_destroy_port(JceAudio *) {}
#define jce_runtime_create_with_options runtime_create_port
#define jce_editor_play_presentation_restore presentation_restore_port
#define jce_runtime_stats_host_close stats_close_port
#define jce_audio_destroy audio_destroy_port
static void actual_play_create()
{
    JceRuntimeDesc rd{};
    JceRuntimeCreateOptions create_options{800, 600};
    const bool configured = true, capable = true;
#include JCE_EDITOR_GAME_CAMERA_CREATE_WIRE
}
static void actual_play_detach()
{
#include JCE_EDITOR_GAME_CAMERA_STOP_WIRE
}
enum { JCE_PLAY_STOPPED = 0 };
static struct { int play_state = 0; } s;
static void actual_stopped_retry_camera()
{
#include JCE_EDITOR_GAME_CAMERA_RETRY_WIRE
}
#undef jce_runtime_create_with_options
#undef jce_runtime_stats_host_close
#undef jce_audio_destroy
#undef jce_editor_play_presentation_restore

struct Fixture {
    Fixture()
    {
        g = {}; s_play_host = {}; s_play_snapshot_valid = false; s_play_audio = nullptr;
        create_attempts = destroys = bridge_destroys = runtime_creates = 0;
        restores = closes = presentation_restores = primary_calls = draws = 0;
        hook_clears = optional_destroys = logs = 0;
        camera_failure = bridge_failure = runtime_failure = false; restore_success = true;
        player_available = vcam_available = authored_ortho = false; primary_available = true;
        play_state = 0; s.play_state = 0; applied_camera = nullptr;
        observed_feedback = -1; applies = 0;
        REQUIRE(owned.empty());
    }
    ~Fixture()
    {
        // Test cleanup remains safe after ordinary mutation assertions fail.
        // It does not count as a production release or erase prior assertions.
        for (auto *camera : owned) jce_camera_destroy(camera);
        owned.clear(); g = {}; s_play_host = {};
    }
    void preview()
    { REQUIRE(test_actual_game_camera_init(fake_renderer, nullptr, nullptr)); REQUIRE(g.camera); }
    void play()
    { s_play_snapshot_valid = true; actual_play_create(); REQUIRE(g.play_camera); play_state = 1; g.play_runtime = fake_runtime; }
    void stop()
    { actual_play_detach(); s_play_host.runtime_stats.runtime = nullptr; play_state = 0; }
};
struct CameraWitness {
    JceCamera *camera;
    jce_mat4 view, proj_wide, proj_square;
    JceCameraMode mode;
    float fov, near_plane, far_plane;
    explicit CameraWitness(JceCamera *p) : camera(p), view(jce_camera_view(p)),
        proj_wide(jce_camera_proj(p, 16.0f/9.0f, false)),
        proj_square(jce_camera_proj(p, 1, true)), mode(jce_camera_get_mode(p)),
        fov(jce_camera_get_fov(p)), near_plane(jce_camera_get_near(p)), far_plane(jce_camera_get_far(p)) {}
    void unchanged()
    {
        CHECK(jce_editor_game_render_get_camera() == camera);
        jce_mat4 current = jce_camera_view(camera);
        CHECK(std::memcmp(&view, &current, sizeof current) == 0);
        current = jce_camera_proj(camera, 16.0f/9.0f, false);
        CHECK(std::memcmp(&proj_wide, &current, sizeof current) == 0);
        current = jce_camera_proj(camera, 1, true);
        CHECK(std::memcmp(&proj_square, &current, sizeof current) == 0);
        CHECK(jce_camera_get_mode(camera) == mode); CHECK(jce_camera_get_fov(camera) == fov);
        CHECK(jce_camera_get_near(camera) == near_plane); CHECK(jce_camera_get_far(camera) == far_plane);
    }
};
static void configure_preview(JceCamera *camera, int projection)
{
    jce_camera_set_fov(camera, 73);
    jce_camera_set_near_far(camera, 0.3f, 321);
    jce_camera_set_mode(camera, projection ? JCE_CAMERA_ORTHO : JCE_CAMERA_PERSPECTIVE);
    if (projection == 1) jce_camera_set_ortho_size(camera, 13, 7);
    if (projection == 2) jce_camera_set_ortho_height(camera, 9);
    CHECK(jce_camera_set_pose(camera, jce_v3(9, 8, 7), jce_v3(0, 0, -1), jce_v3(1, 1, 0)));
}
TEST_CASE_FIXTURE(Fixture, "repeated Play Stop keeps complete rolled perspective preview")
{
    preview(); configure_preview(g.camera, 0); CameraWitness before(g.camera);
    for (unsigned i = 0; i < 3; ++i) {
        play(); CHECK(jce_editor_game_render_get_camera() == g.play_camera);
        CHECK(g.play_camera != g.camera); CHECK_FALSE(g.taa_prev_valid);
        test_actual_game_camera_frame(1280, 720);
        CHECK(applied_camera == g.play_camera);
        stop(); before.unchanged(); CHECK(g.play_camera == nullptr);
        CHECK(destroys == i + 1); CHECK(owned.size() == 1);
    }
    CHECK(primary_calls == 3); CHECK(runtime_creates == 3);
}
TEST_CASE_FIXTURE(Fixture, "fixed orthographic extent and roll survive authored perspective")
{
    preview(); configure_preview(g.camera, 1); CameraWitness before(g.camera);
    play(); test_actual_game_camera_frame(800, 600); stop(); before.unchanged();
    CHECK(destroys == 1); CHECK(owned.size() == 1);
}
TEST_CASE_FIXTURE(Fixture, "aspect derived orthographic preview survives authored orthographic VCam")
{
    preview(); configure_preview(g.camera, 2); CameraWitness before(g.camera);
    authored_ortho = vcam_available = player_available = true;
    play(); test_actual_game_camera_frame(1200, 500); stop(); before.unchanged();
    CHECK(applied_camera != before.camera); CHECK(owned.size() == 1);
}
TEST_CASE_FIXTURE(Fixture, "hidden Game performs Play admission and Stop without a draw or preview")
{
    play(); CHECK(g.camera == nullptr); CHECK_FALSE(g.initialized); CHECK(draws == 0);
    stop(); CHECK(g.play_camera == nullptr); CHECK(destroys == 1); CHECK(owned.empty());
    CHECK(jce_editor_game_render_get_camera() == nullptr);
}
TEST_CASE_FIXTURE(Fixture, "camera allocation failure refuses Runtime and permits clean retry")
{
    preview(); CameraWitness before(g.camera); camera_failure = true;
    s_play_snapshot_valid = true; actual_play_create();
    CHECK(runtime_creates == 0); CHECK(restores == 1); CHECK(closes == 1);
    CHECK_FALSE(s_play_snapshot_valid); CHECK(g.play_camera == nullptr); before.unchanged();
    camera_failure = false; play(); stop(); CHECK(runtime_creates == 1); CHECK(owned.size() == 1);
}
TEST_CASE_FIXTURE(Fixture, "Runtime failure releases admitted camera before retained scene retry")
{
    preview(); CameraWitness before(g.camera); runtime_failure = true; restore_success = false;
    s_play_snapshot_valid = true; actual_play_create();
    CHECK(runtime_creates == 1); CHECK(restores == 1); CHECK(s_play_snapshot_valid);
    CHECK(g.play_camera == nullptr); CHECK(destroys == 1); CHECK(owned.size() == 1); before.unchanged();
    actual_stopped_retry_camera(); CHECK(destroys == 1); before.unchanged();
}
TEST_CASE_FIXTURE(Fixture, "repeated admission is idempotent and selected reset never writes preview")
{
    preview(); configure_preview(g.camera, 1); CameraWitness before(g.camera); play();
    JceCamera *admitted = g.play_camera; CHECK(jce_editor_game_render_begin_play_camera());
    CHECK(g.play_camera == admitted); CHECK(create_attempts == 2);
    jce_camera_set_fov(admitted, 20); jce_editor_game_render_reset_camera();
    CHECK(jce_camera_get_fov(admitted) == 60); stop(); before.unchanged();
}
TEST_CASE_FIXTURE(Fixture, "view initialization failure preserves Play owner until shutdown and retry")
{
    play(); JceCamera *admitted = g.play_camera; bridge_failure = true;
    CHECK_FALSE(test_actual_game_camera_init(fake_renderer, nullptr, nullptr));
    CHECK(g.camera == nullptr); CHECK(g.play_camera == admitted); CHECK(owned.size() == 1);
    CHECK(destroys == 1); bridge_failure = false;
    CHECK(test_actual_game_camera_init(fake_renderer, nullptr, nullptr));
    CHECK(g.play_camera == admitted); CHECK(g.play_runtime == fake_runtime);
    jce_editor_game_render_shutdown(); CHECK(owned.empty()); CHECK(destroys == 3);
    CHECK(g.camera == nullptr); CHECK(g.play_camera == nullptr); CHECK(g.play_runtime == nullptr);
    CHECK(bridge_destroys == 1); CHECK_FALSE(g.initialized);
    jce_editor_game_render_shutdown(); CHECK(destroys == 3); CHECK(bridge_destroys == 1);
}
TEST_CASE_FIXTURE(Fixture, "shutdown releases a camera when Game never initialized")
{
    play(); jce_editor_game_render_shutdown(); CHECK(owned.empty()); CHECK(destroys == 1);
    CHECK(g.play_camera == nullptr); CHECK(g.play_runtime == nullptr);
}
TEST_CASE_FIXTURE(Fixture, "active Play without camera refuses render rather than corrupting preview")
{
    preview(); configure_preview(g.camera, 2); CameraWitness before(g.camera);
    play_state = 1; test_actual_game_camera_frame(800, 600);
    CHECK(draws == 0); CHECK(primary_calls == 0); before.unchanged();
}
TEST_CASE_FIXTURE(Fixture, "stopped retry cleanup cannot retire the stopped preview")
{
    preview(); CameraWitness before(g.camera); play(); stop();
    actual_stopped_retry_camera(); actual_stopped_retry_camera(); before.unchanged();
    CHECK(destroys == 1); CHECK(owned.size() == 1);
}

TEST_CASE_FIXTURE(Fixture, "camera cuts discard colour feedback only after legal PostFX apply")
{
    preview(); play();
    CHECK(g.camera_cut_pending); actual_taa_bind(); CHECK(observed_feedback == 0);
    CHECK(g.camera_cut_pending); // Hidden/skipped view did not apply anything.
    actual_taa_bind(); CHECK(observed_feedback == 0); CHECK(applies == 0);
    actual_postfx_apply(); CHECK_FALSE(g.camera_cut_pending); CHECK(applies == 1);
    actual_taa_bind(); CHECK(observed_feedback == doctest::Approx(0.9f));
    stop(); CHECK(g.camera_cut_pending); CHECK_FALSE(g.taa_prev_valid);
    actual_taa_bind(); CHECK(observed_feedback == 0); actual_postfx_apply();
    CHECK_FALSE(g.camera_cut_pending); CHECK(applies == 2);
}

TEST_CASE_FIXTURE(Fixture, "camera less scene starts Play from complete preview navigation")
{
    for (int projection = 0; projection < 3; ++projection) {
        if (!g.camera) preview();
        configure_preview(g.camera, projection); CameraWitness before(g.camera);
        primary_available = false; player_available = vcam_available = false;
        play();
        CameraWitness initial(g.play_camera);
        CHECK(std::memcmp(&before.view, &initial.view, sizeof before.view) == 0);
        CHECK(std::memcmp(&before.proj_wide, &initial.proj_wide, sizeof before.proj_wide) == 0);
        CHECK(std::memcmp(&before.proj_square, &initial.proj_square, sizeof before.proj_square) == 0);
        test_actual_game_camera_frame(800, 600);
        jce_mat4 after = jce_camera_view(g.play_camera);
        CHECK(std::memcmp(&before.view, &after, sizeof after) == 0);
        after = jce_camera_proj(g.play_camera, 16.0f/9.0f, false);
        CHECK(std::memcmp(&before.proj_wide, &after, sizeof after) == 0);
        stop(); before.unchanged(); CHECK(owned.size() == 1);
    }
}
