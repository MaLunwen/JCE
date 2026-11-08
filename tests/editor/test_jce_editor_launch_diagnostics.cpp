/* Actual private launch helpers/branches and lifecycle wires, with counted SDK
 * destinations. No native Editor/window/renderer or performance claim. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_cadence.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_defaults.h"
#include <jce/application/jce_app_interface.h>
#include <jce/os/core/jce_env.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_timer.h>
#include <jce/renderer/jce_renderer.h>
#include <cstdlib>
#include <cstring>

#ifndef JCE_EDITOR_LAUNCH_DIAGNOSTICS_OWNER
#error actual private launch diagnostic compiler input is required
#endif
namespace {
const char *keys[] = {"JCE_DBG_OVERVIEW", "JCE_STATIC_BATCH_BAKE",
    "JCE_EDITOR_OPERATION_METRICS", "JCE_EDITOR_CADENCE_DIAG"};
const char *values[4] = {};
unsigned reads[4] = {}, scene_focus = 0, game_focus = 0, bakes = 0;
unsigned metric_logs = 0, cadence_logs = 0;
int fps = 60;
struct {
    bool initialized = false, overview_diagnostics = false;
    bool static_batch_bake_requested = false;
    int overview_focus_frames = 0, static_batch_bake_frames = 0;
} s_editor;
char *counted_getenv(const char *key)
{
    for (unsigned i = 0; i < 4; ++i) {
        if (!strcmp(key, keys[i])) { ++reads[i]; return const_cast<char *>(values[i]); }
    }
    return !strcmp(key, "JCE_EDITOR_FOREGROUND_GAME_VIEW") ? const_cast<char *>("1") : nullptr;
}
void counted_log(int, const char *tag, const char *, int, const char *, ...)
{
    if (!strcmp(tag, "editor.cadence")) ++cadence_logs;
}
void jce_editor_layout_request_focus_scene_view() { ++scene_focus; }
void jce_editor_layout_request_focus_game_view() { ++game_focus; }
void jce_editor_panel_static_batch_bake_now() { ++bakes; }
}
extern "C" {
int JCE_CALL jce_engine_get_target_fps(void) { return fps; }
void JCE_CALL jce_engine_set_target_fps(int value) { fps = value; }
uint64_t JCE_CALL jce_time_perf_freq(void) { return 1000; }
bool jce_window_get_flags(const JceWindow *, uint32_t *flags) { *flags = JCE_WINDOW_INPUT_FOCUS; return true; }
void jce_input_capture(const JceInput *, JceInputFrame *out) { *out = {}; }
JcePlayState jce_state_get_play_state(void) { return JCE_PLAY_STOPPED; }
void jce_editor_play_host_tick(int, int, int) {}
bool jce_renderer_is_fallback(const JceRenderer *) { return true; }
bool jce_renderer_get_vsync(const JceRenderer *) { return false; }
}
struct EditorState;
static void editor_cadence_refresh(EditorState &) {}
#undef LOG_INFO
#undef LOG_WARN
#define LOG_INFO(...) (++metric_logs)
#define LOG_WARN(...) ((void)0)
#define jce_log_write counted_log
#define getenv counted_getenv
#include JCE_EDITOR_LAUNCH_DIAGNOSTICS_OWNER
#undef getenv
#undef jce_log_write
#undef LOG_INFO
#undef LOG_WARN

static void clear_counts()
{
    for (auto &count : reads) count = 0;
    scene_focus = game_focus = bakes = metric_logs = cadence_logs = 0;
}
TEST_CASE("presence launch selectors read once across real frame branches and refresh on reinit")
{
    for (const char *value : {static_cast<const char *>(nullptr), "", "0", "1"}) {
        CAPTURE(value ? value : "absent");
        actual_editor_reset();
        clear_counts();
        values[0] = values[1] = value;
        REQUIRE(jce_editor_init(nullptr, nullptr));
        s_editor.initialized = true; // The rest of initialization is a typed unavailable port.
        REQUIRE(jce_editor_init(nullptr, nullptr));
        CHECK(reads[0] == 1);
        CHECK(reads[1] == 1);
        const bool present = value != nullptr;
        CHECK(s_editor.overview_diagnostics == present);
        CHECK(s_editor.static_batch_bake_requested == present);
        for (unsigned tick = 0; tick < 1000; ++tick) actual_launch_tick();
        CHECK(reads[0] == 1);
        CHECK(reads[1] == 1);
        CHECK(scene_focus == (present ? 10u : 0u));
        CHECK(bakes == (present ? 1u : 0u));
        if (present) CHECK(game_focus == 0); // The actual nested overview precedence consumes its snapshot.
        values[0] = values[1] = present ? nullptr : "1";
        for (unsigned tick = 0; tick < 1000; ++tick) actual_launch_tick();
        CHECK(reads[0] == 1);
        CHECK(reads[1] == 1);
        CHECK(scene_focus == (present ? 10u : 0u));
        CHECK(bakes == (present ? 1u : 0u));
        actual_editor_reset();
        CHECK_FALSE(s_editor.overview_diagnostics);
        CHECK_FALSE(s_editor.static_batch_bake_requested);
    }
}
TEST_CASE("operation metrics first-use selector belongs to the state reset")
{
    for (const char *value : {static_cast<const char *>(nullptr), "", "0", "1"}) {
        CAPTURE(value ? value : "absent");
        actual_metrics_reset();
        clear_counts();
        values[2] = value;
        s.operation_metrics.tick = 50;
        for (unsigned tick = 0; tick < 1000; ++tick) {
            jce_editor_hierarchy_metric(1, 1, 1, 1);
            jce_editor_hierarchy_reveal_metric(1, true);
        }
        CHECK(reads[2] == 1);
        CHECK(metric_logs == (value ? 2000u : 0u));
        values[2] = value ? nullptr : "1";
        for (unsigned tick = 0; tick < 1000; ++tick) jce_editor_hierarchy_reveal_metric(1, true);
        CHECK(reads[2] == 1);
        CHECK(metric_logs == (value ? 3000u : 0u));
        actual_metrics_reset();
        CHECK_FALSE(s.operation_metrics.selector_initialized);
        CHECK(observe() == (value == nullptr));
        CHECK(reads[2] == 2);
    }
}
TEST_CASE("cadence selector retains value semantics until actual host reinitialization")
{
    for (const char *value : {static_cast<const char *>(nullptr), "", "0", "01", "1", "false"}) {
        CAPTURE(value ? value : "absent");
        g_state = {};
        clear_counts();
        values[3] = value;
        actual_host_init(nullptr);
        jce_editor_cadence_begin(g_state.cadence, g_state.cadence_rates, 0, 1000);
        const bool enabled = value && value[0] && value[0] != '0';
        for (unsigned tick = 0; tick < 1000; ++tick) editor_cadence_update(g_state, tick);
        CHECK(reads[3] == 1);
        CHECK(cadence_logs == (enabled ? 1u : 0u));
        values[3] = enabled ? nullptr : "1";
        for (unsigned tick = 1000; tick < 2000; ++tick) editor_cadence_update(g_state, tick);
        CHECK(reads[3] == 1);
        CHECK(cadence_logs == (enabled ? 1u : 0u));
        actual_host_reset();
        CHECK_FALSE(g_state.cadence_diagnostics_enabled);
        actual_host_init(nullptr);
        CHECK(reads[3] == 2);
        CHECK(g_state.cadence_diagnostics_enabled == !enabled);
        CHECK(g_state.cadence_diagnostics == 0);
        CHECK(g_state.cadence_logged_fps == -1);
        CHECK(g_state.cadence_logged_flags == UINT32_MAX);
        editor_cadence_update(g_state, 2000);
        CHECK(g_state.cadence_diagnostics == (enabled ? 0u : 1u));
    }
}
