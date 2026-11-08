/* Actual bounded ring + exact production admission prefixes. No window/GPU. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#ifndef JCE_TEST_EDITOR_OPERATION_METRICS_HEADER
#define JCE_TEST_EDITOR_OPERATION_METRICS_HEADER "core/jce_editor_operation_metrics.h"
#endif
#include JCE_TEST_EDITOR_OPERATION_METRICS_HEADER
#include "core/jce_editor_viewport_work.h"
#include "core/jce_editor_frame_observation.h"
#ifndef JCE_TEST_EDITOR_PROBE_MAINTENANCE_HEADER
#define JCE_TEST_EDITOR_PROBE_MAINTENANCE_HEADER "scene/jce_editor_probe_maintenance.h"
#endif
#include JCE_TEST_EDITOR_PROBE_MAINTENANCE_HEADER
#include "jce_test_imgui_context.h"
#include "jce_test_owned_directory.h"
#include <jce/os/core/jce_json.h>
#include <jce/middleware/scene/jce_scene_reflection_probe.h>
#include <jce/middleware/scene/jce_scene_probe_capture.h>
#include <jce/renderer/jce_impostor.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <cstring>
#include <cstdio>
#include <limits>
#include <vector>

#ifndef JCE_TEST_EDITOR_RENDER_ADMISSION_BODIES
#error exact production Scene/Game admission bodies are required
#endif

static JceEditorFrameObservation s_game_frame_observation;

/* Only borrowed owner objects and public operations are counted ports. */
static struct {
    bool initialized;
    JceRenderer *renderer;
    JceSceneRenderer *scene_renderer;
} s_sr;
static struct {
    bool initialized;
    JceRenderer *renderer;
    JceOffscreenTarget *bridge;
    JceCamera *play_camera;
} g;
static struct {
    bool impostor, probe;
    unsigned impostor_queries, probe_queries, probe_polls, scene_admitted, game_admitted;
    unsigned cancels, scene_destroys, scene_clears, renderer_releases, edits;
    unsigned bake_cancels, bake_polls, bake_submits, capture_submits, progress_bars;
    bool renderer_alive = true, capture_accept = true, bake_accept = true;
    const char *button = nullptr;
    JceProbeCaptureStatus probe_result = JCE_PROBE_CAPTURE_RENDERING;
    JceBakeStatus bake_result = JCE_BAKE_STATUS_CONVOLVING_SPECULAR;
    int play_state;
    JceScene *scene;
    JceCamera *camera;
    JceEntity panel_entity = 8;
    JceReflectionProbeComponent *panel_probe = nullptr;
} ports;
static JceScene *jce_state_get_scene() { return ports.scene; }
static int jce_state_get_play_state() { return ports.play_state; }
static JceSceneRenderer *jce_editor_get_scene_renderer() { return s_sr.scene_renderer; }
static JceCamera *game_display_camera_internal() { return ports.camera; }
extern "C" bool jce_impostor_bake_in_flight() {
    ++ports.impostor_queries; return ports.impostor;
}
extern "C" bool jce_scene_probe_capture_in_flight() {
    ++ports.probe_queries; return ports.probe;
}
extern "C" JceProbeCaptureStatus jce_scene_probe_capture_poll(
    JceSceneRenderer *sr, JceScene *scene, JceRenderer *renderer) {
    ++ports.probe_polls;
    CHECK(ports.renderer_alive); CHECK(sr == s_sr.scene_renderer);
    CHECK(scene == ports.scene); CHECK(renderer == s_sr.renderer);
    if (ports.probe_result != JCE_PROBE_CAPTURE_RENDERING) ports.probe = false;
    return ports.probe_result;
}
extern "C" void jce_scene_probe_capture_cancel() { ++ports.cancels; ports.probe = false; }
#include JCE_TEST_EDITOR_RENDER_ADMISSION_BODIES

/* Exact production pre-layout callsite, with real ImGui focus observations. */
static void production_probe_host(uint32_t w = 800, uint32_t h = 600) {
    const ImGuiIO &io = ImGui::GetIO();
#include JCE_TEST_EDITOR_PROBE_HOST_BODY
}

/* Complete scene-clear prefix and accepted history-clear prefix. Public scene
 * operations are counted ports; actual Engine retirement has separate tests. */
static struct { JceScene *scene; } s;
static std::vector<int> g_entity_order, g_entity_sidecar;
static uint64_t g_entity_order_gen;
static void jce_roots_invalidate() {}
static void jce_state_clear_selection() {}
static void probe_renderer_release_port() {
    CHECK_FALSE(ports.probe); CHECK(ports.cancels > 0);
    ++ports.renderer_releases; ports.renderer_alive = false;
}
extern "C" void jce_scene_destroy(JceScene *scene) {
    CHECK(scene == s.scene); CHECK_FALSE(ports.probe); CHECK(ports.cancels > 0);
    ++ports.scene_destroys;
}
extern "C" JceScene *jce_scene_create() { return ports.scene; }
extern "C" int jce_scene_clear(JceScene *scene) {
    CHECK(scene == s.scene); CHECK_FALSE(ports.probe); CHECK(ports.cancels > 0);
    ++ports.scene_clears; return 0;
}
/* Play final recovery/discard has its own actual-history fixture. */
static void jce_editor_play_finish_shutdown() {}
/* Scene lifecycle registration has its own actual-registry Unity fixture. */
static void sr_temporal_lifecycle_end() {}
struct HistorySuspendScope {};
#define LOG_WARN(...) ((void)0)
#include JCE_TEST_EDITOR_PROBE_DESTRUCTIVE_BODIES
#undef LOG_WARN

/* Exact Inspector branch with counted UI requests, independent of native input. */
static const char *jce_editor_i18n(const char *key) { return key; }
static void insp_track_edit() { ++ports.edits; }
namespace ProbeUi {
static bool Checkbox(const char *, bool *) { return false; }
static void TextDisabled(const char *, ...) {}
static void TextUnformatted(const char *) {}
static void ProgressBar(float, ImVec2) { ++ports.progress_bars; }
static bool Button(const char *key) { return ports.button && std::strcmp(ports.button, key) == 0; }
}
extern "C" bool jce_reflection_probe_bake_poll(JceReflectionProbeBakeHandle handle,
    JceReflectionProbeBakeProgress *out) {
    ++ports.bake_polls; CHECK(handle == 73); *out = {};
    out->status = ports.bake_result; out->overall = 0.5f; return true;
}
extern "C" void jce_reflection_probe_bake_cancel(JceReflectionProbeBakeHandle handle) {
    CHECK(handle == 73); ++ports.bake_cancels;
}
extern "C" bool jce_scene_reflection_probe_capture(JceScene *, JceEntity, char *out, int size) {
    ++ports.capture_submits;
    if (!ports.capture_accept || ports.probe) return false;
    std::snprintf(out, static_cast<size_t>(size), "%s", "captured.ktx");
    ports.probe = true; return true;
}
extern "C" JceReflectionProbeBakeHandle jce_scene_reflection_probe_bake(
    JceScene *, JceEntity, bool, char *out, int size) {
    ++ports.bake_submits;
    if (!ports.bake_accept) return 0;
    std::snprintf(out, static_cast<size_t>(size), "%s", "baked.ktx"); return 73;
}
#define ImGui ProbeUi
#include JCE_TEST_EDITOR_PROBE_INSPECTOR_BODY
#undef ImGui
extern "C" JceReflectionProbeComponent *jce_scene_get_reflection_probe(JceScene *scene, JceEntity entity) {
    CHECK(scene == ports.scene);
    return entity == ports.panel_entity ? ports.panel_probe : nullptr;
}
#include JCE_TEST_EDITOR_PROBE_PANEL_BODY

namespace {
struct TraceFixture {
    TraceFixture(bool enabled = true) {
        jce_trace_set_enabled(false); jce_trace_thread_unregister();
        jce_trace_task_restore_id(0); jce_trace_reset();
        jce_perf_phase_set_enabled(0);
        jce_trace_set_enabled(enabled);
        if (enabled) jce_trace_thread_register("editor_render_fixture");
        s_sr = {true, reinterpret_cast<JceRenderer *>(uintptr_t(1)),
            reinterpret_cast<JceSceneRenderer *>(uintptr_t(2))};
        g = {true, s_sr.renderer, reinterpret_cast<JceOffscreenTarget *>(uintptr_t(3)),
            reinterpret_cast<JceCamera *>(uintptr_t(4))};
        ports = {};
        s_probe_maintenance = {};
        ports.scene = reinterpret_cast<JceScene *>(uintptr_t(5));
        ports.camera = g.play_camera;
        s.scene = ports.scene;
        rp_bake_queue().clear(); rp_active_handle() = 0;
    }
    ~TraceFixture() {
        jce_trace_thread_unregister(); jce_trace_set_enabled(false);
        jce_trace_task_restore_id(0); jce_perf_phase_set_enabled(0);
    }
    std::vector<JceTraceEvent> events() {
        JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
        JceTraceEvent buffer[256];
        const uint32_t count = jce_trace_read(&cursor, buffer, 256);
        CHECK(cursor.lost_events == 0);
        return {buffer, buffer + count};
    }
};
struct GuiFixture {
    JceTestImGuiContext gui;
    GuiFixture() {
        gui.begin();
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_Always);
        ImGui::Begin("Render trace policy", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    }
    ~GuiFixture() { ImGui::End(); gui.end(); }
};
static unsigned counters(const std::vector<JceTraceEvent> &events, const char *name,
                         int64_t value, uint64_t *id = nullptr) {
    unsigned count = 0;
    for (const auto &event : events)
        if (event.type == JCE_TRACE_EVENT_COUNTER && std::strcmp(event.name, name) == 0) {
            ++count; CHECK(event.category == JCE_TRACE_CATEGORY_RENDER);
            CHECK(event.value_u64 == static_cast<uint64_t>(value));
            CHECK(event.id == event.parent_id);
            if (id) *id = event.id;
        }
    return count;
}
static uint64_t closed(const std::vector<JceTraceEvent> &events, const char *name,
                       uint64_t parent, JceTraceTaskState state) {
    unsigned submitted = 0, begun = 0, ended = 0;
    uint64_t id = 0;
    for (const auto &event : events) {
        if (std::strcmp(event.name, name) != 0) continue;
        CHECK(event.category == JCE_TRACE_CATEGORY_TASK);
        if (event.type == JCE_TRACE_EVENT_TASK_SUBMIT) {
            ++submitted; id = event.id;
            CHECK(event.parent_id == parent); CHECK(event.value_u32[0] == JCE_TRACE_TASK_SERVICE);
        } else if (event.type == JCE_TRACE_EVENT_TASK_BEGIN) {
            ++begun; CHECK(event.id == id); CHECK(event.parent_id == id);
        } else if (event.type == JCE_TRACE_EVENT_TASK_END) {
            ++ended; CHECK(event.id == id); CHECK(event.parent_id == id);
            CHECK(event.value_u32[0] == state);
        } else CHECK_MESSAGE(false, "unexpected scope event");
    }
    CHECK(submitted == 1); CHECK(begun == 1); CHECK(ended == 1); CHECK(id != 0);
    return id;
}
static void balanced() {
    JceTraceStats stats = {}; jce_trace_get_stats(&stats);
    CHECK(stats.active_work_items == 0); CHECK(stats.active_waits == 0);
    CHECK(stats.work_items_started == stats.work_items_completed);
}
}

TEST_CASE("operation scopes close nested CPU work and restore caller on ordinary early return") {
    TraceFixture fixture;
    const uint64_t parent = UINT64_C(9007199254741017);
    jce_trace_task_restore_id(parent);
    uint64_t outer = 0;
    {
        JceEditorOperationScope operation("fixture.play.stop");
        outer = jce_trace_task_current_id();
        auto create = [] {
            JceEditorOperationScope operation("ed_play_create");
            operation.result(JCE_TRACE_TASK_FAILED);
            return false;
        };
        CHECK_FALSE(create()); CHECK(jce_trace_task_current_id() == outer);
        { JceEditorOperationScope teardown("ed_play_teardown"); }
        CHECK(jce_trace_task_current_id() == outer);
    }
    CHECK(jce_trace_task_current_id() == parent);
    const auto events = fixture.events();
    CHECK(closed(events, "fixture.play.stop", parent, JCE_TRACE_TASK_SUCCEEDED) == outer);
    closed(events, "ed_play_create", outer, JCE_TRACE_TASK_FAILED);
    closed(events, "ed_play_teardown", outer, JCE_TRACE_TASK_SUCCEEDED);
    balanced();
}

TEST_CASE("disabled observation preserves caller identity and actual admission side effects") {
    TraceFixture fixture(false);
    const uint64_t parent = 331;
    jce_trace_task_restore_id(parent);
    { JceEditorOperationScope operation("ed_play_create"); }
    production_scene_admission(640, 360); production_game_admission(640, 360);
    ports.probe = true;
    jce_editor_scene_render_maintenance(true);
    jce_editor_scene_render_cancel_probe_capture();
    CHECK(ports.probe_polls == 1); CHECK(ports.cancels == 1);
    char path[64] = {};
    const uint64_t request = jce_editor_scene_render_begin_probe_capture(ports.scene, 7, path, sizeof(path));
    CHECK(request != 0);
    CHECK(jce_editor_scene_render_probe_capture_matches(request, ports.scene, 7));
    jce_editor_scene_render_cancel_probe_capture();
    CHECK_FALSE(jce_editor_scene_render_probe_capture_matches(request, ports.scene, 7));
    jce_editor_viewport_policy_trace("editor.game.policy", JceEditorViewportPolicyReason::Hidden);
    CHECK(ports.scene_admitted == 1); CHECK(ports.game_admitted == 1);
    CHECK(jce_trace_task_current_id() == parent);
    JceTraceStats stats = {}; jce_trace_get_stats(&stats);
    CHECK(stats.recorded_events == 0); CHECK(stats.tasks_submitted == 0);
    balanced();
}

TEST_CASE("phase collection remains independent from disabled canonical tracing") {
    TraceFixture fixture(false);
    jce_perf_phase_set_enabled(1);
    { JceEditorOperationScope operation("fixture.phase_only"); }
    jce_perf_phase_frame_tick();
    bool found = false;
    for (int i = 0; i < jce_perf_phase_count(); ++i) {
        const char *name = nullptr; double ms = -1;
        if (jce_perf_phase_peek_frame(i, &name, &ms) && name &&
            std::strcmp(name, "fixture.phase_only") == 0) {
            found = true; CHECK(ms >= 0);
        }
    }
    CHECK(found);
    JceTraceStats stats = {}; jce_trace_get_stats(&stats);
    CHECK(stats.recorded_events == 0); CHECK(stats.tasks_submitted == 0);
}

TEST_CASE("real ImGui focus clip invalid and explicit capture keep original policy and typed causes") {
    TraceFixture trace; GuiFixture gui;
    const ImVec2 start(16, 24), size(200, 100);
    JceEditorViewportPolicyReason reason = JceEditorViewportPolicyReason::Admitted;
    bool expected = true;
    SUBCASE("visible") {}
    SUBCASE("focus lost") { ImGui::GetIO().AppFocusLost = true; reason = JceEditorViewportPolicyReason::FocusLost; expected = false; }
    SUBCASE("clipped") { reason = JceEditorViewportPolicyReason::Clipped; expected = false; }
    SUBCASE("invalid before capture") { reason = JceEditorViewportPolicyReason::InvalidLayout; expected = false; }
    SUBCASE("capture overrides focus and clip") { ImGui::GetIO().AppFocusLost = true; reason = JceEditorViewportPolicyReason::Capture; }
    const bool capture = reason == JceEditorViewportPolicyReason::Capture ||
        reason == JceEditorViewportPolicyReason::InvalidLayout;
    ImVec2 pos = reason == JceEditorViewportPolicyReason::Clipped || capture ? ImVec2(900, 900) : start;
    ImVec2 extent = reason == JceEditorViewportPolicyReason::InvalidLayout
        ? ImVec2(std::numeric_limits<float>::quiet_NaN(), 100) : size;
    CHECK(jce_editor_viewport_wants_render(pos, extent, capture, "editor.scene.policy") == expected);
    CHECK(counters(trace.events(), "editor.scene.policy", static_cast<int64_t>(reason)) == 1);
}

TEST_CASE("actual Scene early admission preserves precedence probe progress and one closed reason") {
    TraceFixture fixture;
    JceEditorViewportRenderReason reason = JceEditorViewportRenderReason::Admitted;
    unsigned expected_admit = 0, expected_poll = 0;
    uint32_t width = 640;
    SUBCASE("admitted") { expected_admit = 1; }
    SUBCASE("uninitialized wins over zero extent and both bakes") { s_sr.initialized = false; width = 0; ports.impostor = ports.probe = true; reason = JceEditorViewportRenderReason::Unavailable; }
    SUBCASE("renderer missing") { s_sr.renderer = nullptr; reason = JceEditorViewportRenderReason::Unavailable; }
    SUBCASE("zero extent wins over bakes") { width = 0; ports.impostor = ports.probe = true; reason = JceEditorViewportRenderReason::EmptyExtent; }
    SUBCASE("impostor wins over probe without polling") { ports.impostor = ports.probe = true; reason = JceEditorViewportRenderReason::ImpostorBusy; }
    SUBCASE("probe owner still progresses then yields") { ports.probe = true; reason = JceEditorViewportRenderReason::ProbeBusy; expected_poll = 1; }
    if (expected_poll) jce_editor_scene_render_maintenance(true);
    production_scene_admission(width, 360);
    CHECK(ports.scene_admitted == expected_admit); CHECK(ports.probe_polls == expected_poll);
    const auto events = fixture.events();
    const auto id = closed(events, "editor.scene.render", 0,
        expected_admit ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_CANCELLED);
    uint64_t counter_id = 0;
    CHECK(counters(events, "editor.scene.render.reason", static_cast<int64_t>(reason), &counter_id) == 1);
    CHECK(counter_id == id);
    CHECK(counters(events, "editor.scene.render.admitted", 1) == expected_admit);
    balanced();
}

TEST_CASE("actual Game early admission preserves missing-owner Play guard and bake yields") {
    TraceFixture fixture;
    JceEditorViewportRenderReason reason = JceEditorViewportRenderReason::MissingSceneOwner;
    unsigned expected_admit = 0;
    uint32_t width = 640;
    SUBCASE("admitted stopped") { reason = JceEditorViewportRenderReason::Admitted; expected_admit = 1; }
    SUBCASE("admitted playing") { ports.play_state = 1; reason = JceEditorViewportRenderReason::Admitted; expected_admit = 1; }
    SUBCASE("admitted paused") { ports.play_state = 2; reason = JceEditorViewportRenderReason::Admitted; expected_admit = 1; }
    SUBCASE("unavailable") { g.initialized = false; reason = JceEditorViewportRenderReason::Unavailable; }
    SUBCASE("empty extent") { width = 0; reason = JceEditorViewportRenderReason::EmptyExtent; }
    SUBCASE("impostor busy") { ports.impostor = ports.probe = true; reason = JceEditorViewportRenderReason::ImpostorBusy; }
    SUBCASE("probe yields without owning progress") { ports.probe = true; reason = JceEditorViewportRenderReason::ProbeBusy; }
    SUBCASE("renderer owner missing") { s_sr.scene_renderer = nullptr; }
    SUBCASE("scene missing") { ports.scene = nullptr; }
    SUBCASE("display camera missing") { ports.camera = nullptr; }
    SUBCASE("target missing") { g.bridge = nullptr; }
    SUBCASE("playing camera missing") { ports.play_state = 1; g.play_camera = nullptr; }
    SUBCASE("paused camera missing") { ports.play_state = 2; g.play_camera = nullptr; }
    production_game_admission(width, 360);
    CHECK(ports.game_admitted == expected_admit); CHECK(ports.probe_polls == 0);
    const auto events = fixture.events();
    closed(events, "editor.game.render", 0,
        expected_admit ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_CANCELLED);
    CHECK(counters(events, "editor.game.render.reason", static_cast<int64_t>(reason)) == 1);
    CHECK(counters(events, "editor.game.render.admitted", 1) == expected_admit);
    balanced();
}

TEST_CASE("admission is distinct from later target or resource refusal") {
    TraceFixture fixture;
    JceEditorViewportRenderReason reason = JceEditorViewportRenderReason::TargetRefused;
    SUBCASE("target refused") {}
    SUBCASE("resource preparation refused") { reason = JceEditorViewportRenderReason::ResourcesRefused; }
    {
        JceEditorViewportRenderTrace render("fixture.viewport", "fixture.reason", "fixture.admitted");
        render.admitted(); render.reason(reason);
    }
    const auto events = fixture.events();
    closed(events, "fixture.viewport", 0, JCE_TRACE_TASK_FAILED);
    CHECK(counters(events, "fixture.admitted", 1) == 1);
    CHECK(counters(events, "fixture.reason", static_cast<int64_t>(reason)) == 1);
    balanced();
}

TEST_CASE("host pump progresses capture with Scene hidden and retains terminal-frame Game yield") {
    TraceFixture trace; GuiFixture gui;
    ports.probe = true;
    production_probe_host(); production_game_admission(640, 360);
    CHECK(ports.probe_polls == 1); CHECK(ports.game_admitted == 0);
    SUBCASE("completed capture") { ports.probe_result = JCE_PROBE_CAPTURE_DONE; }
    SUBCASE("failed capture") { ports.probe_result = JCE_PROBE_CAPTURE_FAILED; }
    production_probe_host(); production_game_admission(640, 360);
    CHECK_FALSE(ports.probe); CHECK(ports.probe_polls == 2);
    CHECK(ports.game_admitted == 0); CHECK(ports.scene_admitted == 0);
    CHECK(jce_editor_scene_render_probe_busy());
    production_probe_host(); production_game_admission(640, 360);
    CHECK(ports.probe_polls == 2); CHECK(ports.game_admitted == 1);
    CHECK_FALSE(jce_editor_scene_render_probe_busy());
    const auto events = trace.events();
    unsigned status_samples = 0, poll_ends = 0;
    for (const auto &event : events) {
        if (event.type == JCE_TRACE_EVENT_COUNTER &&
            std::strcmp(event.name, "editor.probe.capture.status") == 0) {
            CHECK(status_samples < 2);
            CHECK(event.value_u64 == static_cast<uint64_t>(status_samples == 0
                ? JCE_PROBE_CAPTURE_RENDERING : ports.probe_result));
            ++status_samples;
            CHECK(event.id != 0); CHECK(event.id == event.parent_id);
        }
        if (event.type == JCE_TRACE_EVENT_TASK_END &&
            std::strcmp(event.name, "editor.probe.capture.poll") == 0) {
            CHECK(event.value_u32[0] == (poll_ends == 1 && ports.probe_result == JCE_PROBE_CAPTURE_FAILED
                ? JCE_TRACE_TASK_FAILED : JCE_TRACE_TASK_SUCCEEDED));
            ++poll_ends;
        }
    }
    CHECK(status_samples == 2); CHECK(poll_ends == 2);
    balanced();
}

TEST_CASE("actual host wire preserves focus explicit-capture and positive-drawable policy") {
    TraceFixture trace; GuiFixture gui;
    JceTestEnvironment frame("JCE_KPI_FRAME_LOG", nullptr), draw("JCE_KPI_DRAW_LOG", nullptr);
    JceTestEnvironment policy("JCE_KPI_FRAME_LOG_VIEWPORT", nullptr);
    ports.probe = true;
    uint32_t width = 800, height = 600;
    unsigned expected = 0;
    SUBCASE("focus lost") { ImGui::GetIO().AppFocusLost = true; }
    SUBCASE("zero width") { width = 0; }
    SUBCASE("zero height") { height = 0; }
    SUBCASE("foreground") { expected = 1; }
    SUBCASE("explicit capture retains its prior background override") {
        ImGui::GetIO().AppFocusLost = true; frame.set("capture.csv"); expected = 1;
    }
    SUBCASE("natural FrameLog does not override focus") {
        ImGui::GetIO().AppFocusLost = true; frame.set("natural.csv"); policy.set("natural");
    }
    production_probe_host(width, height);
    CHECK(ports.probe_polls == expected); CHECK(ports.cancels == 0);
    balanced();
}

TEST_CASE("layout-started capture waits until next pump and impostor keeps priority") {
    TraceFixture trace; GuiFixture gui;
    production_probe_host();
    ports.probe = true; // The existing Inspector begins capture later in layout.
    production_scene_admission(640, 360); production_game_admission(640, 360);
    CHECK(ports.probe_polls == 0); CHECK(ports.scene_admitted == 0); CHECK(ports.game_admitted == 0);
    ports.impostor = true;
    production_probe_host(); production_scene_admission(640, 360); production_game_admission(640, 360);
    CHECK(ports.probe_polls == 0); CHECK(ports.scene_admitted == 0); CHECK(ports.game_admitted == 0);
    ports.impostor = false;
    production_probe_host(); CHECK(ports.probe_polls == 1);
    balanced();
}

TEST_CASE("capture cancellation preserves this frame busy and permits next-frame view admission") {
    TraceFixture trace; GuiFixture gui;
    ports.probe = true;
    production_probe_host();
    jce_editor_scene_render_cancel_probe_capture();
    CHECK(ports.cancels == 1); CHECK_FALSE(ports.probe);
    CHECK(jce_editor_scene_render_probe_busy());
    production_scene_admission(640, 360); production_game_admission(640, 360);
    CHECK(ports.scene_admitted == 0); CHECK(ports.game_admitted == 0);
    production_probe_host();
    production_scene_admission(640, 360); production_game_admission(640, 360);
    CHECK(ports.scene_admitted == 1); CHECK(ports.game_admitted == 1);
    balanced();
}

TEST_CASE("missing maintenance owner cancels safely without a stale poll and closes nested parent") {
    TraceFixture trace;
    char path[64] = {};
    REQUIRE(jce_editor_scene_render_begin_probe_capture(ports.scene, 7, path, sizeof(path)) != 0);
    SUBCASE("initialization missing") { s_sr.initialized = false; }
    SUBCASE("scene renderer missing") { s_sr.scene_renderer = nullptr; }
    SUBCASE("scene missing") { ports.scene = nullptr; }
    SUBCASE("renderer missing") { s_sr.renderer = nullptr; }
    jce_editor_scene_render_maintenance(true);
    CHECK(ports.cancels == 1); CHECK(ports.probe_polls == 0); CHECK_FALSE(ports.probe);
    CHECK(s_probe_maintenance.active_request == 0);
    CHECK(jce_editor_scene_render_probe_busy());
    const auto events = trace.events();
    const auto poll = closed(events, "editor.probe.capture.poll", 0, JCE_TRACE_TASK_FAILED);
    closed(events, "editor.probe.capture.cancel", poll, JCE_TRACE_TASK_SUCCEEDED);
    balanced();
}

TEST_CASE("actual destructive prefixes cancel before scene clear or renderer release") {
    TraceFixture trace;
    char path[64] = {};
    REQUIRE(jce_editor_scene_render_begin_probe_capture(ports.scene, 7, path, sizeof(path)) != 0);
    SUBCASE("scene object replacement") { clear_scene_entities(); CHECK(ports.scene_destroys == 1); }
    SUBCASE("history keeps object but clears content") { CHECK(production_probe_history_clear_prefix()); CHECK(ports.scene_clears == 1); }
    SUBCASE("normal view shutdown") { production_probe_shutdown_prefix(); CHECK(ports.renderer_releases == 1); }
    SUBCASE("failed initialization shutdown") { s_sr.initialized = false; production_probe_shutdown_prefix(); CHECK(ports.renderer_releases == 0); }
    SUBCASE("state shutdown") { production_probe_state_shutdown_prefix(); }
    CHECK(ports.cancels == 1); CHECK_FALSE(ports.probe); CHECK(ports.probe_polls == 0);
    CHECK(s_probe_maintenance.active_request == 0);
    balanced();
}

TEST_CASE("actual Inspector Cancel reaches capture and existing bake owners independently") {
    TraceFixture trace;
    static JceReflectionProbeComponent a{}, b{};
    // Retire a previous subcase's local static bake owner through its real branch.
    ports.bake_result = JCE_BAKE_STATUS_DONE;
    production_probe_inspector(ports.scene, 7, &a);
    ports.bake_result = JCE_BAKE_STATUS_CONVOLVING_SPECULAR;
    ports.edits = ports.bake_polls = ports.progress_bars = 0;
    ports.capture_accept = true;
    SUBCASE("capture") {}
    SUBCASE("procedural bake") { ports.capture_accept = false; }
    ports.button = "inspector.reflection_probe.bake";
    production_probe_inspector(ports.scene, 7, &a);
    const bool captured = ports.capture_accept;
    CHECK(ports.capture_submits == 1); CHECK(ports.bake_submits == (captured ? 0u : 1u));
    ports.button = "inspector.reflection_probe.cancel";
    production_probe_inspector(ports.scene, 8, &b);
    CHECK(ports.cancels == 0); CHECK(ports.bake_cancels == 0);
    production_probe_inspector(ports.scene, 7, &a);
    CHECK(ports.cancels == (captured ? 1u : 0u));
    CHECK(ports.bake_cancels == (captured ? 0u : 1u));
    CHECK(ports.progress_bars == (captured ? 0u : 1u));
    CHECK(ports.probe_polls == 0); // UI cancellation never drives capture rendering.
    ports.button = nullptr;
    balanced();
}

TEST_CASE("shared Editor capture identity admits only successful non-reused requests") {
    TraceFixture trace;
    char path[64] = {};
    const uint64_t a = jce_editor_scene_render_begin_probe_capture(ports.scene, 7, path, sizeof(path));
    REQUIRE(a != 0);
    CHECK(jce_editor_scene_render_probe_capture_matches(a, ports.scene, 7));
    CHECK_FALSE(jce_editor_scene_render_probe_capture_matches(a, ports.scene, 8));
    CHECK_FALSE(jce_editor_scene_render_probe_capture_matches(a,
        reinterpret_cast<JceScene *>(uintptr_t(11)), 7));
    CHECK_FALSE(jce_editor_scene_render_probe_capture_matches(0, ports.scene, 7));
    const uint64_t refused = jce_editor_scene_render_begin_probe_capture(ports.scene, 8, path, sizeof(path));
    CHECK(refused == 0); CHECK(s_probe_maintenance.active_request == a);
    CHECK(jce_editor_scene_render_probe_capture_matches(a, ports.scene, 7));
    jce_editor_scene_render_cancel_probe_capture();
    CHECK(s_probe_maintenance.active_request == 0);
    CHECK_FALSE(jce_editor_scene_render_probe_capture_matches(a, ports.scene, 7));
    const uint64_t b = jce_editor_scene_render_begin_probe_capture(ports.scene, 7, path, sizeof(path));
    CHECK(b > a); CHECK_FALSE(jce_editor_scene_render_probe_capture_matches(a, ports.scene, 7));
    CHECK(jce_editor_scene_render_probe_capture_matches(b, ports.scene, 7));
    CHECK(jce_editor_scene_render_probe_busy()); // Same-frame restart retains the modal band.
    balanced();
}

TEST_CASE("capture request generation exhaustion refuses before Engine admission") {
    TraceFixture trace;
    char path[64] = {};
    s_probe_maintenance.generation = UINT64_MAX - 1;
    const uint64_t final_request = jce_editor_scene_render_begin_probe_capture(ports.scene, 7, path, sizeof(path));
    REQUIRE(final_request == UINT64_MAX);
    jce_editor_scene_render_cancel_probe_capture();
    const unsigned attempts = ports.capture_submits;
    CHECK(jce_editor_scene_render_begin_probe_capture(ports.scene, 8, path, sizeof(path)) == 0);
    CHECK(ports.capture_submits == attempts); CHECK(s_probe_maintenance.active_request == 0);
    CHECK(s_probe_maintenance.generation == UINT64_MAX);
    CHECK_FALSE(jce_editor_scene_render_probe_capture_matches(final_request, ports.scene, 7));
    balanced();
}

TEST_CASE("Inspector A cannot cancel a later capture started by the actual batch-panel owner") {
    TraceFixture trace;
    static JceReflectionProbeComponent a{}, b{};
    ports.button = "inspector.reflection_probe.bake";
    production_probe_inspector(ports.scene, 7, &a);
    const uint64_t a_request = s_probe_maintenance.active_request;
    REQUIRE(a_request != 0);
    REQUIRE(jce_editor_scene_render_probe_capture_matches(a_request, ports.scene, 7));
    bool observed_retirement = true;
    SUBCASE("A completed") {
        ports.probe_result = JCE_PROBE_CAPTURE_DONE;
        jce_editor_scene_render_maintenance(true);
    }
    SUBCASE("A failed") {
        ports.probe_result = JCE_PROBE_CAPTURE_FAILED;
        jce_editor_scene_render_maintenance(true);
    }
    SUBCASE("external Editor owner cancellation then restart in the same frame") {
        jce_editor_scene_render_cancel_probe_capture();
    }
    SUBCASE("external Engine cancellation observed by the host") {
        jce_scene_probe_capture_cancel();
        jce_editor_scene_render_maintenance(true);
    }
    SUBCASE("external Engine cancellation then another Editor entry restarts without a pump") {
        jce_scene_probe_capture_cancel(); observed_retirement = false;
    }
    CHECK_FALSE(jce_editor_scene_render_probe_capture_matches(a_request, ports.scene, 7));
    if (observed_retirement) CHECK(s_probe_maintenance.active_request == 0);
    ports.panel_probe = &b;
    rp_bake_queue().push_back(ports.panel_entity);
    rp_kick_next(ports.scene);
    const uint64_t b_request = s_probe_maintenance.active_request;
    REQUIRE(b_request > a_request);
    REQUIRE(ports.probe);
    REQUIRE(jce_editor_scene_render_probe_capture_matches(b_request, ports.scene, ports.panel_entity));
    const unsigned before = ports.cancels;
    ports.button = "inspector.reflection_probe.cancel";
    production_probe_inspector(ports.scene, 7, &a);
    CHECK(ports.cancels == before); CHECK(ports.probe);
    CHECK(s_probe_maintenance.active_request == b_request);
    CHECK(jce_editor_scene_render_probe_capture_matches(b_request, ports.scene, ports.panel_entity));
    ports.button = nullptr;
    balanced();
}
