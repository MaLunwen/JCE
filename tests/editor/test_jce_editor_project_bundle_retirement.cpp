/* Actual transition/close/Play-guard source plus real Bundle override and RP.
 * Root publication, runtime, streamer and project adapters are typed ports.
 * No native UI, ECS rollback, module unloading or GPU execution is certified. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "io/jce_editor_bundle_render_pipeline.h"
#include "core/jce_editor_project_context.h"
#include "core/jce_editor_project_state.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_build_profile.h"
#include "core/jce_assetdb.h"
#include "core/jce_editor_project.h"
#include "core/jce_editor_script_backends.h"
#include "core/jce_editor_project_render_pipeline.h"
#include "core/jce_project_settings.h"
#include "core/jce_editor_game_l10n.h"
#include "core/jce_pak_key.h"
#include "ui/jce_editor_panels.h"
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/os/core/jce_log.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static struct {
    std::string root = "A";
    bool flush_ok = true, commit_ok = true, has_pipeline = true;
    bool streaming = true;
    JcePlayState play = JCE_PLAY_PLAYING;
    unsigned commits = 0, modules = 0, errors = 0;
    std::vector<std::string> events;
    JceEditorBundlePipeline bundle;
} port;
/* Only the field read by the exact existing pending-Play guard. */
static struct { struct { bool pending; } viewport; } s_play_host;
static bool s_play_snapshot_valid;
static constexpr const char *LOG_TAG = "project-bundle-retirement-test";

extern "C" {
JceRendererBackend jce_renderer_get_active_backend(void) { return JCE_BACKEND_D3D11; }
JceGpuTier jce_renderer_get_tier(void) { return JCE_GPU_TIER_HIGH; }
uint32_t jce_renderer_get_caps(void) { return JCE_CAP_TEXTURE_FLOAT | JCE_CAP_FRAMEBUFFER_FLOAT; }
JceRenderRecommendation jce_renderer_get_recommendation(void)
{ JceRenderRecommendation r{}; r.has_discrete_gpu = true; return r; }
const char *jce_gpu_tier_name(JceGpuTier) { return "controlled"; }
}
const char *jce_editor_current_project_root(void) { return port.root.c_str(); }
bool jce_editor_pstate_flush_now(void) { port.events.emplace_back("flush"); return port.flush_ok; }
void jce_editor_build_profile_snapshot(void) { port.events.emplace_back("profile"); }
void jce_editor_favorites_snapshot(void) { port.events.emplace_back("favorites"); }
void jce_editor_console_log_level(JceConsoleLevel level, const char *, ...)
{ CHECK(level == JCE_CONSOLE_ERROR); ++port.errors; }
JcePlayState jce_state_get_play_state(void) { return port.play; }
void jce_state_stop(void)
{
    CHECK(port.root == "A"); CHECK(port.bundle.active);
    port.events.emplace_back("stop"); port.play = JCE_PLAY_STOPPED;
    s_play_host.viewport.pending = false;
    s_play_snapshot_valid = false;
}
void jce_editor_scene_render_streaming_teardown(void)
{
    CHECK(port.play == JCE_PLAY_STOPPED); CHECK_FALSE(s_play_host.viewport.pending);
    port.events.emplace_back("stream"); port.streaming = false;
}
static void close_active_bundle_mount(void)
{
    CHECK(port.play == JCE_PLAY_STOPPED); CHECK_FALSE(s_play_host.viewport.pending);
    CHECK_FALSE(port.streaming);
    port.events.emplace_back("bundle"); jce_editor_bundle_pipeline_close(&port.bundle);
}
bool jce_editor_current_project_root_set(const char *root)
{
    ++port.commits; port.events.emplace_back("commit");
    CHECK(port.root == "A"); CHECK(port.play == JCE_PLAY_STOPPED);
    CHECK_FALSE(s_play_host.viewport.pending); CHECK_FALSE(port.streaming);
    CHECK_FALSE(port.bundle.active);
    if (!port.commit_ok) return false;
    port.root = root ? root : ""; return true;
}
void jce_project_settings_set_root(const char *) { port.events.emplace_back("settings"); }
void jce_assetdb_set_root(const char *) { port.events.emplace_back("assets"); }
void jce_editor_project_set_root(const char *) { port.events.emplace_back("project"); }
void jce_editor_script_modules_reload(const char *)
{
    CHECK(port.play == JCE_PLAY_STOPPED); CHECK_FALSE(port.bundle.active);
    port.events.emplace_back("modules"); ++port.modules;
}
void jce_editor_gl10n_unload(void) { port.events.emplace_back("l10n"); }
void jce_pak_key_install_process(const std::string &) { port.events.emplace_back("key"); }
bool jce_editor_project_render_pipeline_load(const char *root, JceRenderPipelineDesc *out)
{
    CHECK(std::string(root) == "B"); CHECK_FALSE(port.bundle.active);
    port.events.emplace_back("pipeline");
    jce_render_pipeline_preset_high(out); out->render_scale = 0.65f;
    out->enable_volumetric_fog = false; return port.has_pipeline;
}
#include JCE_TEST_PROJECT_BUNDLE_RETIREMENT_BODY

static JceRenderPipelineDesc requested()
{ JceRenderPipelineDesc d; jce_render_pipeline_get_requested(&d); return d; }
struct Fixture {
    Fixture()
    {
        port = {}; port.root = "A"; port.flush_ok = port.commit_ok = port.has_pipeline = true;
        port.streaming = true; port.play = JCE_PLAY_PLAYING; s_play_host.viewport.pending = false;
        s_play_snapshot_valid = false;
        JceRenderPipelineDesc a; jce_render_pipeline_preset_high(&a); a.render_scale = 0.9f;
        jce_render_pipeline_apply(&a);
        jce_render_pipeline_preset_low(&port.bundle.prepared);
        port.bundle.prepared.render_scale = 0.4f; port.bundle.ready = true;
        REQUIRE(jce_editor_bundle_pipeline_apply(&port.bundle));
    }
    ~Fixture() { jce_editor_bundle_pipeline_close(&port.bundle); }
};
static size_t event_index(const char *name)
{
    for (size_t i = 0; i < port.events.size(); ++i) if (port.events[i] == name) return i;
    return port.events.size();
}
TEST_CASE("Open Project retires old ownership before B RP even without a last scene")
{
    Fixture f;
    SUBCASE("playing") {}
    SUBCASE("paused") { port.play = JCE_PLAY_PAUSED; }
    SUBCASE("pending only") { port.play = JCE_PLAY_STOPPED; s_play_host.viewport.pending = true; }
    SUBCASE("stopped with retained snapshot") { port.play = JCE_PLAY_STOPPED; s_play_snapshot_valid = true; }
    REQUIRE(set_current_project_root("B/jce_project.json"));
    CHECK(port.root == "B"); CHECK_FALSE(port.bundle.active); CHECK_FALSE(port.bundle.ready);
    CHECK(requested().render_scale == doctest::Approx(0.65));
    CHECK(event_index("stop") < event_index("stream"));
    CHECK(event_index("stream") < event_index("bundle"));
    CHECK(event_index("bundle") < event_index("commit"));
    CHECK(event_index("commit") < event_index("modules"));
    CHECK(event_index("modules") < event_index("pipeline"));
    /* The later plain-scene close, or an explicit repeated close, cannot replay A. */
    CHECK(jce_state_close_bundle_preview()); CHECK(jce_state_close_bundle_preview());
    CHECK(requested().render_scale == doctest::Approx(0.65));
    CHECK(port.commits == 1); CHECK(port.modules == 1);
}
TEST_CASE("Project close and missing RP keep the new default after repeated close")
{
    Fixture f;
    SUBCASE("close") { REQUIRE(set_current_project_root(nullptr)); CHECK(port.root.empty()); }
    SUBCASE("missing B RP") { port.has_pipeline = false; REQUIRE(set_current_project_root("B")); }
    JceRenderPipelineDesc tier; jce_render_pipeline_preset_for_current_tier(&tier);
    CHECK(requested().render_scale == tier.render_scale);
    CHECK(requested().post_quality == tier.post_quality);
    CHECK_FALSE(port.bundle.active); CHECK(jce_state_close_bundle_preview());
    CHECK(requested().render_scale == tier.render_scale);
}
TEST_CASE("Same normalized identity retains live Bundle and pending Play")
{
    Fixture f; s_play_host.viewport.pending = true;
    REQUIRE(set_current_project_root("A\\jce_project.json"));
    CHECK(port.events.empty()); CHECK(port.bundle.active); CHECK(s_play_host.viewport.pending);
    CHECK(port.play == JCE_PLAY_PLAYING); CHECK(requested().render_scale == doctest::Approx(0.4));
}
TEST_CASE("Admission refuses before retirement; late publication refusal remains safely retired")
{
    Fixture f;
    SUBCASE("preflight") {
        port.flush_ok = false; CHECK_FALSE(set_current_project_root("B"));
        CHECK(port.bundle.active); CHECK(port.play == JCE_PLAY_PLAYING); CHECK(port.streaming);
        CHECK(port.commits == 0); CHECK(requested().render_scale == doctest::Approx(0.4));
    }
    SUBCASE("root publication") {
        port.commit_ok = false; CHECK_FALSE(set_current_project_root("B"));
        CHECK_FALSE(port.bundle.active); CHECK(port.play == JCE_PLAY_STOPPED); CHECK_FALSE(port.streaming);
        CHECK(port.commits == 1); CHECK(requested().render_scale == doctest::Approx(0.9));
    }
    SUBCASE("root input overflow") {
        CHECK_FALSE(set_current_project_root(std::string(600, 'x').c_str()));
        CHECK(port.events.empty()); CHECK(port.bundle.active); CHECK(port.commits == 0);
    }
    CHECK(port.root == "A"); CHECK(port.modules == 0); CHECK(port.errors == 1);
}
