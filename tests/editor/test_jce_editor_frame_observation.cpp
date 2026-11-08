/* Strict actual Editor wires plus the complete private observation owner. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_frame_observation.h"
#include <jce/ui/jce_imgui_renderer.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <string>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/os/core/jce_math.h>

static JceEditorFrameObservation s_game_frame_observation;
static struct {
    bool initialized = true;
    uint16_t render_width = 0, render_height = 0;
    uint32_t extent_frame = 0;
    bool extent_written = false, stats_frame_completed = false;
    JceRenderer *renderer = reinterpret_cast<JceRenderer *>(uintptr_t(1));
    JceOffscreenTarget *bridge = reinterpret_cast<JceOffscreenTarget *>(uintptr_t(2));
    JceCamera *play_camera = reinterpret_cast<JceCamera *>(uintptr_t(3));
    JcePostFXPipeline *postfx = reinterpret_cast<JcePostFXPipeline *>(uintptr_t(6));
    uint16_t postfx_output_tex = UINT16_MAX;
} g;
static struct Ports {
    bool enabled = true, extent = true, prepare = true, resources = true;
    bool impostor = false, probe = false, scene = true, renderer = true, camera = true;
    bool ui_available = true, postfx_available = false;
    uint32_t allocated_w = 640, allocated_h = 360;
    uint16_t color = 0;
    unsigned admitted = 0, frame_calls = 0, images = 0, draws = 0, reports = 0;
    uint32_t frame = 7;
    unsigned extent_queries = 0, frame_queries = 0, hud_calls = 0;
    unsigned postfx_queries = 0;
    JcePostFXOutputTarget postfx_target{};
    std::string hud;
    JceFrameViewportObservation report{};
} ports;

static JceSceneRenderer *jce_editor_get_scene_renderer() {
    return ports.renderer ? reinterpret_cast<JceSceneRenderer *>(uintptr_t(4)) : nullptr;
}
static JceScene *jce_state_get_scene() {
    return ports.scene ? reinterpret_cast<JceScene *>(uintptr_t(5)) : nullptr;
}
static int jce_state_get_play_state() { return 1; }
static JceCamera *game_display_camera_internal() { return ports.camera ? g.play_camera : nullptr; }
static bool jce_impostor_bake_in_flight() { return ports.impostor; }
static bool jce_editor_scene_render_probe_busy() { return ports.probe; }
extern "C" bool jce_offscreen_target_get_extent(const JceOffscreenTarget *, uint32_t *w, uint32_t *h) {
    ++ports.extent_queries;
    const bool valid = ports.extent && ports.allocated_w && ports.allocated_h;
    *w = valid ? ports.allocated_w : 0;
    *h = valid ? ports.allocated_h : 0;
    return valid;
}
extern "C" uint16_t jce_offscreen_target_get_color_texture(const JceOffscreenTarget *) { return ports.color; }
extern "C" bool jce_postfx_get_output_target(const JcePostFXPipeline *pipeline,
                                            JcePostFXOutputTarget *out) {
    ++ports.postfx_queries;
    REQUIRE(pipeline == g.postfx);
    REQUIRE(out->struct_size == sizeof(*out));
    REQUIRE(out->version == JCE_POSTFX_OUTPUT_TARGET_VERSION);
    if (!ports.postfx_available) return false;
    *out = ports.postfx_target;
    return true;
}
extern "C" bool jce_offscreen_target_prepare_keep(JceOffscreenTarget *, uint32_t, uint32_t,
    const float *, const float *, uint32_t, bool, bool, const char *) { return ports.prepare; }
extern "C" bool jce_scene_renderer_prepare_resources(JceSceneRenderer *, JceScene *) { return ports.resources; }
extern "C" void jce_scene_renderer_set_taa_velocity_enabled(JceSceneRenderer *, bool) {}
extern "C" bool JCE_CALL jce_engine_frame_observation_enabled(void) { return ports.enabled; }
extern "C" bool JCE_CALL jce_engine_report_frame_observations(
    const JceFrameViewportObservation *values, uint32_t count, size_t stride) {
    REQUIRE(count == 1); REQUIRE(stride == sizeof(*values));
    ports.report = *values; ++ports.reports; return true;
}
extern "C" void jce_imgui_renderer_draw(void) { ++ports.draws; }
extern "C" bool jce_imgui_renderer_draw_observed(uint16_t texture,
    JceImGuiDrawObservation *out, size_t size) {
    ++ports.draws; REQUIRE(size == sizeof(*out));
    if (!ports.ui_available || texture == UINT16_MAX) return false;
    *out = {sizeof(*out), JCE_IMGUI_DRAW_QUEUE_OBSERVED | JCE_IMGUI_DRAW_SUBMISSIONS_OBSERVED, 2, 1, 0};
    return true;
}
extern "C" uint32_t jce_renderer_get_frame_index(const JceRenderer *) {
    ++ports.frame_queries; return ports.frame;
}
extern "C" const char *jce_renderer_running_backend_name(void) { return "D3D11"; }
static void jce_editor_game_render_frame(uint32_t, uint32_t) { ++ports.frame_calls; }
static bool s_show_stats = true;
static const char *jce_editor_i18n(const char *key) {
    if (!strcmp(key, "game.renderer")) return "Renderer";
    if (!strcmp(key, "gameView.stats.ui_fps")) return "UI FPS";
    if (!strcmp(key, "gameView.stats.content")) return "Content";
    if (!strcmp(key, "gameView.stats.target_3d")) return "3D target";
    if (!strcmp(key, "profiler.value.unavailable")) return "N/A";
    return key;
}
struct ImVec2 { float x, y; };
using ImTextureID = uintptr_t;
#define IM_COL32(R, G, B, A) uint32_t(0)
struct ImDrawList {
    void AddText(ImVec2, uint32_t, const char *text) { ++ports.hud_calls; ports.hud = text; }
};
namespace ImGui {
struct IO { bool AppFocusLost = false; float Framerate = 60.0f; };
static IO io;
static IO &GetIO() { return io; }
static ImVec2 CalcTextSize(const char *) { return {800.0f, 14.0f}; }
static void Image(ImTextureID, ImVec2, ImVec2, ImVec2) { ++ports.images; }
}
#ifndef JCE_TEST_EDITOR_FRAME_OBSERVATION_WIRES
#error strict production observation wires required
#endif
#include JCE_TEST_EDITOR_FRAME_OBSERVATION_WIRES

struct Fixture {
    Fixture() {
        ports = {}; g.initialized = true;
        g.render_width = g.render_height = 0; g.extent_frame = 0;
        g.extent_written = g.stats_frame_completed = false; s_show_stats = true;
        g.renderer = reinterpret_cast<JceRenderer *>(uintptr_t(1));
        g.bridge = reinterpret_cast<JceOffscreenTarget *>(uintptr_t(2));
        g.play_camera = reinterpret_cast<JceCamera *>(uintptr_t(3));
        g.postfx = reinterpret_cast<JcePostFXPipeline *>(uintptr_t(6));
        g.postfx_output_tex = UINT16_MAX;
        ImGui::io = {}; s_game_frame_observation.begin(false);
    }
    JceFrameViewportObservation snapshot() {
        JceFrameViewportObservation result{};
        CHECK(jce_editor_game_render_observation_copy(&result));
        return result;
    }
    void success() {
        actual_begin(); actual_request(true); actual_admission(1000, 700);
        actual_prepare(1000, 700); actual_resources(); actual_tail(); actual_image(0);
    }
};

TEST_CASE_FIXTURE(Fixture, "disabled observation preserves one ordinary draw") {
    ports.enabled = false; actual_begin(); actual_request(true); actual_admission(1000, 700);
    actual_prepare(1000, 700); actual_tail();
    ports.color = 7;
    g.postfx_output_tex = 0;
    const auto extent_queries = ports.extent_queries;
    actual_image(0); actual_submit();
    CHECK(ports.draws == 1); CHECK(ports.reports == 0);
    CHECK(ports.frame_calls == 1); CHECK(ports.images == 1);
    CHECK(ports.extent_queries == extent_queries);
    CHECK(ports.postfx_queries == 0);
}
TEST_CASE_FIXTURE(Fixture, "completed CPU path observes actual allocation and exact Image texture") {
    success(); actual_submit();
    CHECK(ports.report.cpu_outcome == JCE_FRAME_VIEWPORT_CPU_COMPLETED);
    CHECK(ports.report.refusal_reason == 0);
    CHECK(ports.report.requested_width == 1000); CHECK(ports.report.requested_height == 700);
    CHECK(ports.report.allocated_width == 640); CHECK(ports.report.display_width == 640);
    CHECK(ports.report.allocated_height == 360); CHECK(ports.report.display_height == 360);
    CHECK((ports.report.flags & JCE_FRAME_VIEWPORT_PREPARE_SUCCEEDED) != 0);
    CHECK((ports.report.flags & JCE_FRAME_VIEWPORT_TEXTURE_VALID) != 0);
    CHECK(ports.report.texture_index == 0); CHECK(ports.report.image_count == 1);
    CHECK(ports.report.queued_commands == 2); CHECK(ports.report.main_submits == 1);
    CHECK(ports.report.recording_submits == 0); CHECK(ports.draws == 1); CHECK(ports.reports == 1);
    CHECK(ports.postfx_queries == 0);
}
TEST_CASE_FIXTURE(Fixture, "next unvisited or hidden interval cannot reuse a completed viewport") {
    success(); actual_submit(); actual_begin(); actual_submit();
    CHECK(ports.report.cpu_outcome == JCE_FRAME_VIEWPORT_CPU_SKIPPED);
    CHECK(ports.report.refusal_reason == 1); CHECK(ports.report.flags == JCE_FRAME_VIEWPORT_IMAGE_COUNT_VALID);
    CHECK(ports.report.requested_width == 0); CHECK(ports.report.image_count == 0);
    CHECK(ports.draws == 2); CHECK(ports.reports == 2);
    actual_begin(); jce_editor_game_render_observation_policy(static_cast<uint32_t>(JceEditorViewportPolicyReason::Hidden));
    actual_submit(); CHECK(ports.report.refusal_reason == 105);
}
TEST_CASE_FIXTURE(Fixture, "current prepare refusal may display retained allocation without fresh completion") {
    actual_begin(); ports.prepare = false; actual_admission(1000, 700); actual_prepare(1000, 700); actual_image(0); actual_submit();
    CHECK(ports.report.cpu_outcome == JCE_FRAME_VIEWPORT_CPU_FAILED);
    CHECK(ports.report.refusal_reason == 206);
    CHECK_FALSE((ports.report.flags & JCE_FRAME_VIEWPORT_PREPARE_SUCCEEDED) != 0);
    CHECK((ports.report.flags & JCE_FRAME_VIEWPORT_ALLOCATED_SIZE_VALID) != 0);
    CHECK(ports.report.display_width == 640); CHECK(ports.report.image_count == 1);
}
TEST_CASE_FIXTURE(Fixture, "resource refusal keeps allocation distinct from CPU completion") {
    actual_begin(); actual_admission(1000, 700); actual_prepare(1000, 700);
    ports.resources = false; actual_resources(); actual_image(0); actual_submit();
    CHECK(ports.report.cpu_outcome == JCE_FRAME_VIEWPORT_CPU_FAILED);
    CHECK(ports.report.refusal_reason == 207); CHECK((ports.report.flags & JCE_FRAME_VIEWPORT_PREPARE_SUCCEEDED) != 0);
}
TEST_CASE_FIXTURE(Fixture, "actual early owner guards retain refusal and never claim completion") {
    for (int reason = 1; reason <= 5; ++reason) {
        actual_begin(); g.initialized = reason != 1; ports.impostor = reason == 3;
        ports.probe = reason == 4; ports.scene = reason != 5;
        actual_admission(reason == 2 ? 0 : 1000, 700);
        CHECK(snapshot().cpu_outcome == JCE_FRAME_VIEWPORT_CPU_SKIPPED);
        CHECK(snapshot().refusal_reason == 200u + reason);
    }
    CHECK(ports.admitted == 0);
}
TEST_CASE_FIXTURE(Fixture, "natural focus or clip refusal records request without invoking render") {
    actual_begin(); ImGui::io.AppFocusLost = true; actual_request(false);
    CHECK(snapshot().refusal_reason == 103); CHECK(snapshot().requested_width == 1000);
    actual_begin(); ImGui::io.AppFocusLost = false; actual_request(false);
    CHECK(snapshot().refusal_reason == 104); CHECK(ports.frame_calls == 0);
}
TEST_CASE_FIXTURE(Fixture, "owned PostFX image reports its exact extent including handle zero") {
    ports.color = 7;
    g.postfx_output_tex = 0;
    ports.postfx_available = true;
    ports.postfx_target = {sizeof(JcePostFXOutputTarget), JCE_POSTFX_OUTPUT_TARGET_VERSION,
        JCE_POSTFX_OUTPUT_OWNED, 512, 288, JCE_POSTFX_OUTPUT_FORMAT_RGBA16F, {0}, 0};

    actual_begin(); actual_request(true); actual_admission(1000, 700);
    actual_prepare(1000, 700); actual_resources(); actual_tail(); actual_image(0);
    actual_submit();

    CHECK(ports.postfx_queries == 1);
    CHECK(ports.report.cpu_outcome == JCE_FRAME_VIEWPORT_CPU_COMPLETED);
    CHECK(ports.report.requested_width == 1000); CHECK(ports.report.requested_height == 700);
    CHECK(ports.report.allocated_width == 640); CHECK(ports.report.allocated_height == 360);
    CHECK((ports.report.flags & JCE_FRAME_VIEWPORT_DISPLAY_SIZE_VALID) != 0);
    CHECK(ports.report.display_width == 512); CHECK(ports.report.display_height == 288);
    CHECK(ports.report.texture_index == 0); CHECK(ports.report.image_count == 1);
}
TEST_CASE_FIXTURE(Fixture, "unknown PostFX extent never borrows bridge display dimensions") {
    actual_begin(); actual_image(9); actual_submit();
    CHECK(ports.report.texture_index == 9); CHECK(ports.report.allocated_width == 640);
    CHECK_FALSE((ports.report.flags & JCE_FRAME_VIEWPORT_DISPLAY_SIZE_VALID) != 0);
    CHECK(ports.report.display_width == 0); CHECK(ports.report.display_height == 0);
    CHECK(ports.postfx_queries == 0);
}
TEST_CASE_FIXTURE(Fixture, "failed borrowed or mismatched PostFX metadata stays unavailable") {
    g.postfx_output_tex = 9;
    for (unsigned i = 0; i < 3; ++i) {
        ports.postfx_available = i != 0;
        ports.postfx_target = {sizeof(JcePostFXOutputTarget), JCE_POSTFX_OUTPUT_TARGET_VERSION,
            JCE_POSTFX_OUTPUT_OWNED, 512, 288, JCE_POSTFX_OUTPUT_FORMAT_RGBA16F, {8}, 0};
        if (i == 1) {
            ports.postfx_target.kind = JCE_POSTFX_OUTPUT_BORROWED;
            ports.postfx_target.texture.idx = 9;
            ports.postfx_target.framebuffer = UINT16_MAX;
            ports.postfx_target.width = ports.postfx_target.height = 0;
            ports.postfx_target.format = JCE_POSTFX_OUTPUT_FORMAT_UNKNOWN;
        }
        actual_begin(); actual_image(9); actual_submit();
        CHECK(ports.report.texture_index == 9); CHECK(ports.report.image_count == 1);
        CHECK(ports.report.allocated_width == 640); CHECK(ports.report.allocated_height == 360);
        CHECK_FALSE((ports.report.flags & JCE_FRAME_VIEWPORT_DISPLAY_SIZE_VALID) != 0);
        CHECK(ports.report.display_width == 0); CHECK(ports.report.display_height == 0);
    }
    CHECK(ports.postfx_queries == 3);
}
TEST_CASE_FIXTURE(Fixture, "later unqualified image cannot inherit a prior texture extent") {
    actual_begin(); actual_image(0); actual_image(9); actual_submit();
    CHECK(ports.report.texture_index == 9); CHECK(ports.report.image_count == 2);
    CHECK_FALSE((ports.report.flags & JCE_FRAME_VIEWPORT_DISPLAY_SIZE_VALID) != 0);
    CHECK(ports.report.display_width == 0); CHECK(ports.report.display_height == 0);
}
TEST_CASE_FIXTURE(Fixture, "Image invocation with unavailable UI observation stays unknown") {
    success(); ports.ui_available = false; actual_submit();
    CHECK(ports.report.image_count == 1); CHECK(ports.draws == 1);
    CHECK_FALSE((ports.report.flags & JCE_FRAME_VIEWPORT_QUEUE_COUNT_VALID) != 0);
    CHECK_FALSE((ports.report.flags & JCE_FRAME_VIEWPORT_SUBMIT_COUNTS_VALID) != 0);
    CHECK(ports.report.main_submits == 0);
}
TEST_CASE_FIXTURE(Fixture, "missing allocation does not manufacture a requested display extent") {
    actual_begin(); ports.extent = false; actual_prepare(1000, 700); actual_image(0); actual_tail(); actual_submit();
    CHECK((ports.report.flags & JCE_FRAME_VIEWPORT_PREPARE_SUCCEEDED) != 0);
    CHECK_FALSE((ports.report.flags & JCE_FRAME_VIEWPORT_ALLOCATED_SIZE_VALID) != 0);
    CHECK_FALSE((ports.report.flags & JCE_FRAME_VIEWPORT_DISPLAY_SIZE_VALID) != 0);
    CHECK(ports.report.cpu_outcome == JCE_FRAME_VIEWPORT_CPU_COMPLETED);
}

TEST_CASE_FIXTURE(Fixture, "ordinary Stats shows committed 3D allocation separately from content and UI cadence") {
    ports.enabled = false; success();
    actual_stats(true); actual_submit();
    CHECK(ports.hud.find("UI FPS 60.0") != std::string::npos);
    CHECK(ports.hud.find("Content 1000x700") != std::string::npos);
    CHECK(ports.hud.find("3D target 640x360") != std::string::npos);
    CHECK(ports.hud.find("X 64.0% Y 51.4%") != std::string::npos);
    CHECK(ports.draws == 1); CHECK(ports.reports == 0);
    JceFrameViewportObservation unused{};
    CHECK_FALSE(jce_editor_game_render_observation_copy(&unused));
}
TEST_CASE_FIXTURE(Fixture, "Stats off and natural viewport refusal perform no target metadata query") {
    success(); const auto eq = ports.extent_queries, fq = ports.frame_queries;
    s_show_stats = false; actual_stats(true);
    CHECK(ports.hud_calls == 0); CHECK(ports.extent_queries == eq); CHECK(ports.frame_queries == fq);
    s_show_stats = true; actual_stats(false);
    CHECK(ports.hud.find("3D target N/A") != std::string::npos);
    CHECK(ports.extent_queries == eq); CHECK(ports.frame_queries == fq);
}
TEST_CASE_FIXTURE(Fixture, "same renderer frame prepare refusal retires a previous completed Stats target") {
    success(); actual_stats(true);
    REQUIRE(ports.hud.find("3D target 640x360") != std::string::npos);
    ports.prepare = false; actual_admission(1000, 700); actual_prepare(1000, 700);
    actual_stats(true);
    CHECK(ports.hud.find("3D target N/A") != std::string::npos);
}
TEST_CASE_FIXTURE(Fixture, "new renderer frame cannot reuse the former target stamp") {
    success(); ++ports.frame; actual_stats(true);
    CHECK(ports.hud.find("3D target N/A") != std::string::npos);
}
TEST_CASE_FIXTURE(Fixture, "early owner refusal and missing current target stay unavailable") {
    success(); g.initialized = false; actual_admission(1000, 700); g.initialized = true;
    actual_stats(true); CHECK(ports.hud.find("3D target N/A") != std::string::npos);
    success(); g.bridge = nullptr; actual_stats(true);
    CHECK(ports.hud.find("3D target N/A") != std::string::npos);
    g.bridge = reinterpret_cast<JceOffscreenTarget *>(uintptr_t(2));
    success(); g.renderer = nullptr; actual_stats(true);
    CHECK(ports.hud.find("3D target N/A") != std::string::npos);
}
TEST_CASE_FIXTURE(Fixture, "failed or zero public allocation and empty content do not fabricate ratios") {
    for (int mode = 0; mode < 3; ++mode) {
        ports.extent = mode != 0; ports.allocated_w = mode == 1 ? 0 : 640;
        ports.allocated_h = mode == 2 ? 0 : 360;
        success(); actual_stats(true);
        CHECK(ports.hud.find("3D target N/A") != std::string::npos);
    }
    ports.extent = true; ports.allocated_w = 640; ports.allocated_h = 360;
    success(); const auto eq = ports.extent_queries, fq = ports.frame_queries;
    actual_stats(true, 0, 700);
    CHECK(ports.hud.find("3D target N/A") != std::string::npos);
    CHECK(ports.extent_queries == eq); CHECK(ports.frame_queries == fq);
}
TEST_CASE_FIXTURE(Fixture, "resource refusal never publishes completed Stats target") {
    success(); actual_admission(1000, 700); actual_prepare(1000, 700);
    ports.resources = false; actual_resources(); actual_stats(true);
    CHECK(ports.hud.find("3D target N/A") != std::string::npos);
}
TEST_CASE_FIXTURE(Fixture, "committed supersampling remains visible and Stats preserves capture observation") {
    ports.allocated_w = 2000; ports.allocated_h = 1400;
    success(); const auto before = snapshot(); actual_stats(true);
    CHECK(ports.hud.find("3D target 2000x1400") != std::string::npos);
    CHECK(ports.hud.find("X 200.0% Y 200.0%") != std::string::npos);
    const auto after = snapshot(); CHECK(std::memcmp(&before, &after, sizeof(before)) == 0);
    actual_submit(); CHECK(ports.reports == 1); CHECK(ports.draws == 1);
    CHECK(ports.report.cpu_outcome == JCE_FRAME_VIEWPORT_CPU_COMPLETED);
}
