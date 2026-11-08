/* Complete actual event/should_quit/maintenance bodies + shared service owner; real process
 * child from the existing Test Runner fixture. No Editor/window/GPU is started.
 * Window/cadence/manager ports are counted: they do not certify native minimize. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor.h"
#include "core/jce_editor_cadence.h"
#include "core/jce_editor_host_services.h"
#include "core/jce_editor_lan_discovery.h"
#include "core/jce_editor_test_run.h"
#include "core/jce_editor_config.h"
#include "core/jce_editor_project_state.h"
#include "ui/jce_editor_layout.h"
#include "jce_test_owned_directory.h"
#include <jce/application/jce_app_interface.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <cstring>

#ifndef JCE_TEST_EDITOR_HOST_BODIES
#error complete actual Editor event/poll-and-quit/maintenance bodies are required
#endif
#ifndef JCE_TEST_RECORDER_MAIN_DIRECT_CALLS
#error complete actual main-source Recorder wire count is required
#endif
#ifndef JCE_TEST_RUNNER_CHILD_EXE
#error existing real Test Runner self-child executable is required
#endif

/* Only prerequisites borrowed by the extracted production bodies. */
struct EditorState {
    const JceServices *svc = nullptr;
    bool cpu_services_ready = false;
    float update_dt = 0;
    JceEditorCadence cadence;
};
static EditorState g_state;
static bool g_headless_build_mode;
static uint64_t g_last_update_counter;
static struct {
    bool initialized = false, active = true;
    uint64_t last_time = 0;
    uint32_t frame_kpi_index = 19, autoplay_frame = 23;
} s_editor;
static struct {
    unsigned build, run, cook, serial, index, runner, config, pstate, headless, cadence, console;
    unsigned headless_quit, visible_quit, recorder, lan;
    unsigned quit_requests, event_forwards, requests_at_forward, checks_at_forward;
    const JceEvent *forwarded_event;
    uint64_t activity_at_forward;
    bool headless_done, visible_done, process_consumed;
    JceRunState run_state = JCE_RUN_IDLE;
    uint32_t flags = 0, width = 1280, height = 720;
} ports;
static JceEditorTestRun *child;
static void editor_cadence_update(EditorState &, uint64_t) { ++ports.cadence; }
extern "C" void jce_build_manager_poll() { ++ports.build; }
extern "C" JceRunState jce_run_manager_state() { return ports.run_state; }
extern "C" void jce_run_manager_poll() {
    ++ports.run;
    if (ports.run_state == JCE_RUN_BUILDING) {
        jce_build_manager_poll(); /* Counted pending-build delegation port. */
        ports.run_state = JCE_RUN_RUNNING;
    }
}
extern "C" void jce_cook_manager_poll() { ++ports.cook; }
extern "C" void jce_state_scene_serial_poll() { ++ports.serial; }
extern "C" void jce_asset_path_index_poll() { ++ports.index; }
extern "C" void jce_editor_panel_test_runner_tick() {
    ++ports.runner;
    if (child) jce_editor_test_run_poll(*child);
}
extern "C" void jce_editor_panel_console_poll(void) { ++ports.console; }
extern "C" void jce_editor_recorder_update(void) { ++ports.recorder; }
void jce_editor_lan_discovery_update(void) { ++ports.lan; }
extern "C" void jce_editor_config_flush_tick(float) { ++ports.config; }
extern "C" void jce_editor_pstate_flush_tick(float) { ++ports.pstate; }
void jce_editor_headless_build_poll() { ++ports.headless; }
bool jce_editor_headless_build_should_quit() { ++ports.headless_quit; return ports.headless_done; }
extern "C" bool jce_editor_layout_is_quit_confirmed() { ++ports.visible_quit; return ports.visible_done; }
extern "C" void jce_editor_layout_request_quit() { ++ports.quit_requests; }
extern "C" bool jce_editor_process_event(const JceEvent *event) {
    ++ports.event_forwards;
    ports.forwarded_event = event;
    ports.requests_at_forward = ports.quit_requests;
    ports.checks_at_forward = ports.visible_quit;
    ports.activity_at_forward = g_state.cadence.last_activity;
    return ports.process_consumed;
}
extern "C" bool jce_window_get_flags(const JceWindow *, uint32_t *flags) {
    *flags = ports.flags; return true;
}
extern "C" void JCE_CALL jce_window_get_size(JceWindow *, uint32_t *w, uint32_t *h) {
    if (w) *w = ports.width;
    if (h) *h = ports.height;
}
#include JCE_TEST_EDITOR_HOST_BODIES

static JceServices services;
static void reset(bool ready = true, bool headless = false) {
    ports = {};
    ports.width = 1280; ports.height = 720;
    services = {};
    services.window = reinterpret_cast<JceWindow *>(uintptr_t(1)); /* counted port only */
    g_state = {&services, ready, 0, {}};
    g_headless_build_mode = headless;
    g_last_update_counter = 0;
    s_editor.initialized = ready; s_editor.active = true; s_editor.last_time = 1;
    s_editor.frame_kpi_index = 19; s_editor.autoplay_frame = 23;
    child = nullptr;
}
static std::string read_log(const std::string &path) {
    uint64_t read = 0, total = 0;
    void *data = jce_fs_host_read_capped(path.c_str(), 64 * 1024, &read, &total);
    if (!data) return {};
    std::string text(static_cast<const char *>(data), static_cast<size_t>(read));
    jce_fs_buffer_free(data); return text;
}
static bool poll_child(JceEditorTestRun &run) {
    const uint64_t begin = jce_time_ticks_ms();
    while (run.process && jce_time_ticks_ms() - begin < 1500) {
        (void)editor_should_quit(nullptr); /* The real pre-render owner, no UI frame. */
        if (run.process) jce_thread_sleep_ms(1);
    }
    return run.process == nullptr;
}

TEST_CASE("actual event callback routes each unconfirmed quit and native close through the same owner") {
    const JceEventType types[] = {JCE_EVENT_QUIT, JCE_EVENT_WINDOW_CLOSE};
    for (const auto type : types) {
        CAPTURE(type);
        reset();
        g_state.cadence.last_activity = UINT64_MAX;
        JceEvent event = {}; event.type = type;
        editor_app_event(&event, nullptr);
        CHECK(ports.visible_quit == 1); CHECK(ports.quit_requests == 1);
        CHECK_FALSE(ports.visible_done);
        CHECK(ports.event_forwards == 1); CHECK(ports.forwarded_event == &event);
        CHECK(ports.checks_at_forward == 1); CHECK(ports.requests_at_forward == 1);
        CHECK(g_state.cadence.last_activity == UINT64_MAX);
        CHECK(ports.activity_at_forward == UINT64_MAX);
    }
}
TEST_CASE("actual event callback forwards confirmed quit and native close without another request") {
    const JceEventType types[] = {JCE_EVENT_QUIT, JCE_EVENT_WINDOW_CLOSE};
    for (const auto type : types) {
        CAPTURE(type);
        reset(); ports.visible_done = true;
        g_state.cadence.last_activity = UINT64_MAX;
        JceEvent event = {}; event.type = type;
        editor_app_event(&event, nullptr);
        CHECK(ports.visible_quit == 1); CHECK(ports.quit_requests == 0);
        CHECK(ports.event_forwards == 1); CHECK(ports.forwarded_event == &event);
        CHECK(ports.checks_at_forward == 1); CHECK(ports.requests_at_forward == 0);
        CHECK(g_state.cadence.last_activity == UINT64_MAX);
    }
}
TEST_CASE("actual headless event callback borrows no cadence quit or input destination") {
    const JceEventType types[] = {JCE_EVENT_QUIT, JCE_EVENT_WINDOW_CLOSE, JCE_EVENT_MOUSE_WHEEL};
    for (const auto type : types) {
        CAPTURE(type);
        reset(true, true);
        g_state.cadence.last_activity = 83;
        JceEvent event = {}; event.type = type;
        editor_app_event(&event, nullptr);
        CHECK(ports.visible_quit == 0); CHECK(ports.quit_requests == 0);
        CHECK(ports.event_forwards == 0); CHECK(ports.forwarded_event == nullptr);
        CHECK(g_state.cadence.last_activity == 83);
    }
}
TEST_CASE("actual ordinary and empty event values only forward without borrowing the quit owner") {
    /* NONE is a valid event value. The callback retains its nonnull event contract. */
    const JceEventType types[] = {JCE_EVENT_NONE, JCE_EVENT_OTHER, JCE_EVENT_WINDOW_FOCUS_LOST};
    for (const auto type : types) {
        CAPTURE(type);
        reset();
        g_state.cadence.last_activity = UINT64_MAX;
        JceEvent event = {}; event.type = type;
        editor_app_event(&event, nullptr);
        CHECK(ports.visible_quit == 0); CHECK(ports.quit_requests == 0);
        CHECK(ports.event_forwards == 1); CHECK(ports.forwarded_event == &event);
        CHECK(ports.checks_at_forward == 0); CHECK(ports.requests_at_forward == 0);
        CHECK(g_state.cadence.last_activity == UINT64_MAX);
    }
}
TEST_CASE("actual input event activity precedes forwarding and preserves the original payload") {
    reset(); ports.process_consumed = true;
    JceEvent event = {}; event.type = JCE_EVENT_MOUSE_WHEEL;
    event.wheel.x = 1.25f; event.wheel.y = -2.0f;
    const uint64_t before = jce_time_perf_counter();
    editor_app_event(&event, nullptr);
    const uint64_t after = jce_time_perf_counter();
    CHECK(g_state.cadence.last_activity >= before);
    CHECK(g_state.cadence.last_activity <= after);
    CHECK(g_state.cadence.last_activity > 0);
    CHECK(ports.activity_at_forward == g_state.cadence.last_activity);
    CHECK(ports.visible_quit == 0); CHECK(ports.quit_requests == 0);
    CHECK(ports.event_forwards == 1); CHECK(ports.forwarded_event == &event);
    CHECK(event.wheel.x == 1.25f); CHECK(event.wheel.y == -2.0f);
}

TEST_CASE("Recorder main wire belongs only to pre-render CPU maintenance") {
    /* Derived from complete actual main source, not a copied app_update loop.
     * The real extracted maintenance body executes in the remaining cases. */
    CHECK(JCE_TEST_RECORDER_MAIN_DIRECT_CALLS == 0);
}
TEST_CASE("before init and after exit no service or quit owner is borrowed") {
    reset(false);
    CHECK_FALSE(editor_should_quit(nullptr));
    CHECK(ports.build == 0); CHECK(ports.runner == 0); CHECK(ports.visible_quit == 0);
    CHECK(ports.console == 0); CHECK(ports.recorder == 0); CHECK(ports.lan == 0);
    g_headless_build_mode = true;
    CHECK_FALSE(editor_should_quit(nullptr));
    CHECK(ports.headless == 0); CHECK(ports.headless_quit == 0);
}
TEST_CASE("visible host callback polls each CPU member once and does not advance UI frames") {
    reset();
    CHECK_FALSE(editor_should_quit(nullptr));
    CHECK(ports.build == 1); CHECK(ports.run == 1); CHECK(ports.cook == 1);
    CHECK(ports.serial == 1); CHECK(ports.index == 1); CHECK(ports.runner == 1);
    CHECK(ports.config == 1); CHECK(ports.pstate == 1); CHECK(ports.cadence == 1);
    CHECK(ports.headless == 0); CHECK(s_editor.last_time == 1);
    CHECK(ports.console == 1); CHECK(ports.recorder == 1); CHECK(ports.lan == 1);
    CHECK(s_editor.frame_kpi_index == 19); CHECK(s_editor.autoplay_frame == 23);
    ports.visible_done = true;
    CHECK(editor_should_quit(nullptr));
    CHECK(ports.build == 2); CHECK(ports.runner == 2); CHECK(ports.config == 2);
    CHECK(ports.visible_quit == 2); CHECK(ports.recorder == 2); CHECK(ports.lan == 2);
}
TEST_CASE("pending Run build delegation remains one Build poll even when Run becomes running") {
    reset(); ports.run_state = JCE_RUN_BUILDING;
    CHECK_FALSE(editor_should_quit(nullptr));
    CHECK(ports.run == 1); CHECK(ports.build == 1);
    CHECK(ports.run_state == JCE_RUN_RUNNING);
    CHECK_FALSE(editor_should_quit(nullptr));
    CHECK(ports.run == 2); CHECK(ports.build == 2);
}
TEST_CASE("minimized hidden and zero-size maintenance refresh only the existing UI clock") {
    reset();
    SUBCASE("native minimized flag port") { ports.flags = JCE_WINDOW_MINIMIZED; }
    SUBCASE("hidden flag port") { ports.flags = JCE_WINDOW_HIDDEN; }
    SUBCASE("zero drawable observation port") { ports.width = 0; }
    CHECK_FALSE(editor_should_quit(nullptr));
    CHECK(ports.runner == 1); CHECK(ports.config == 1); CHECK(ports.pstate == 1);
    CHECK(ports.recorder == 1); CHECK(ports.lan == 1);
    CHECK(s_editor.last_time > 1);
    CHECK(s_editor.frame_kpi_index == 19); CHECK(s_editor.autoplay_frame == 23);
    const uint64_t hidden_clock = s_editor.last_time;
    ports.flags = 0; ports.width = 1280;
    CHECK_FALSE(editor_should_quit(nullptr));
    CHECK(ports.runner == 2); CHECK(s_editor.last_time == hidden_clock); CHECK(ports.recorder == 2); CHECK(ports.lan == 2);
}
TEST_CASE("headless keeps only build and headless driver before its completion predicate") {
    reset(true, true);
    ports.headless_done = true;
    CHECK(editor_should_quit(nullptr));
    CHECK(ports.build == 1); CHECK(ports.headless == 1); CHECK(ports.headless_quit == 1);
    CHECK(ports.console == 0); CHECK(ports.recorder == 0); CHECK(ports.lan == 0);
    CHECK(ports.run == 0); CHECK(ports.runner == 0); CHECK(ports.config == 0);
    CHECK(ports.cadence == 0); CHECK(ports.visible_quit == 0);
}
TEST_CASE("real child success and failure finish through the minimized service owner") {
    reset(); ports.flags = JCE_WINDOW_MINIMIZED;
    const char *mode = "delayed-output";
    int expected_exit = 0;
    JceEditorTestRunStatus expected = JceEditorTestRunStatus::Passed;
    SUBCASE("success") {}
    SUBCASE("ordinary failure") { mode = "fail"; expected_exit = 17; expected = JceEditorTestRunStatus::Failed; }
    JceTestOwnedDirectory directory{"_ut_editor_services"};
    JceEditorTestRun run;
    const std::string arguments = std::string("--jce-test-run-child ") + mode;
    REQUIRE(jce_editor_test_run_start(run, JCE_TEST_RUNNER_CHILD_EXE, arguments.c_str(), nullptr,
        directory.path("child.log").c_str(), 1000));
    child = &run;
    const bool observed = poll_child(run);
    CHECK(observed); CHECK(run.status == expected); CHECK(run.exit_code == expected_exit);
    CHECK(ports.runner > 0); CHECK(ports.build == ports.runner);
    CHECK(s_editor.frame_kpi_index == 19); CHECK(s_editor.autoplay_frame == 23);
    const auto log = read_log(run.log_path);
    CHECK(log.find(expected_exit == 0 ? "PASS exit=0" : "FAIL exit=17") != std::string::npos);
    child = nullptr; jce_editor_test_run_close(run); /* Even the missing-wire mutant closes/reaps. */
}
TEST_CASE("real running child reaches observed timeout while no UI frame is admitted") {
    reset(); ports.flags = JCE_WINDOW_MINIMIZED;
    JceTestOwnedDirectory directory{"_ut_editor_services_timeout"}; JceEditorTestRun run;
    REQUIRE(jce_editor_test_run_start(run, JCE_TEST_RUNNER_CHILD_EXE, "--jce-test-run-child sleep",
        nullptr, directory.path("timeout.log").c_str(), 40));
    child = &run;
    CHECK(poll_child(run)); CHECK(run.status == JceEditorTestRunStatus::TimedOut);
    CHECK(read_log(run.log_path).find("TIMEOUT") != std::string::npos);
    CHECK(ports.runner > 0); CHECK(s_editor.frame_kpi_index == 19);
    child = nullptr; jce_editor_test_run_close(run);
}

TEST_CASE("canonical CPU service scopes preserve parent identity and close after returned polls") {
    reset(); ports.flags = JCE_WINDOW_MINIMIZED;
    jce_trace_set_enabled(false); jce_trace_thread_unregister(); jce_trace_reset();
    jce_trace_set_enabled(true); jce_trace_thread_register("editor_service_fixture");
    const uint64_t parent = jce_trace_next_id();
    jce_trace_task_submit(parent, 0, JCE_TRACE_TASK_SERVICE, "fixture.host_iterate");
    jce_trace_task_begin(parent, parent, "fixture.host_iterate", 0);
    CHECK_FALSE(editor_should_quit(nullptr));
    CHECK(jce_trace_task_current_id() == parent);
    jce_trace_task_end(parent, parent, "fixture.host_iterate", JCE_TRACE_TASK_SUCCEEDED, 0);
    jce_trace_thread_unregister();
    const char *names[] = {
        "editor.cpu_services.poll", "editor.cpu_services.run", "editor.cpu_services.build",
        "editor.cpu_services.cook", "editor.cpu_services.serial", "editor.cpu_services.index",
        "editor.cpu_services.test_runner", "editor.cpu_services.maintenance",
        "editor.cpu_services.recorder", "editor.cpu_services.config_flush", "editor.cpu_services.state_flush",
        "editor.cpu_services.lan_discovery",
        "editor.cpu_services.console"
    };
    uint64_t ids[13] = {}, parents[13] = {};
    unsigned submitted[13] = {}, started[13] = {}, ended[13] = {};
    JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    JceTraceEvent events[128];
    const uint32_t count = jce_trace_read(&cursor, events, 128);
    CHECK(cursor.lost_events == 0);
    for (uint32_t i = 0; i < count; ++i) {
        const auto &event = events[i];
        for (unsigned n = 0; n < 13; ++n) {
            if (std::strcmp(event.name, names[n]) != 0) continue;
            CHECK(event.category == JCE_TRACE_CATEGORY_TASK);
            if (event.type == JCE_TRACE_EVENT_TASK_SUBMIT) {
                ++submitted[n]; ids[n] = event.id; parents[n] = event.parent_id;
                CHECK(event.value_u32[0] == JCE_TRACE_TASK_SERVICE);
            } else if (event.type == JCE_TRACE_EVENT_TASK_BEGIN) {
                ++started[n]; CHECK(event.id == ids[n]); CHECK(event.parent_id == event.id);
            } else if (event.type == JCE_TRACE_EVENT_TASK_END) {
                ++ended[n]; CHECK(event.id == ids[n]); CHECK(event.parent_id == event.id);
                /* SUCCEEDED is a synchronous poll return, independent of child/tool outcome. */
                CHECK(event.value_u32[0] == JCE_TRACE_TASK_SUCCEEDED);
            } else CHECK_MESSAGE(false, "unexpected selected service event type");
        }
    }
    for (unsigned n = 0; n < 13; ++n) {
        CHECK(submitted[n] == 1); CHECK(started[n] == 1); CHECK(ended[n] == 1);
        CHECK(ids[n] != 0);
        CHECK(parents[n] == (n == 0 ? parent : (n == 8 || n == 9 || n == 10 || n == 11) ? ids[7] : ids[0]));
    }
    JceTraceStats stats = {}; jce_trace_get_stats(&stats);
    CHECK(stats.active_work_items == 0); CHECK(stats.active_waits == 0); CHECK(stats.active_threads == 0);
    CHECK(ports.runner == 1); CHECK(s_editor.frame_kpi_index == 19);
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0);
}
TEST_CASE("disabled CPU service tracing records nothing and preserves existing caller identity") {
    reset(); ports.flags = JCE_WINDOW_MINIMIZED;
    jce_trace_set_enabled(false); jce_trace_thread_unregister(); jce_trace_reset();
    const uint64_t parent = UINT64_C(9007199254741017);
    jce_trace_task_restore_id(parent);
    CHECK_FALSE(editor_should_quit(nullptr));
    CHECK(ports.runner == 1); CHECK(ports.config == 1); CHECK(ports.pstate == 1);
    CHECK(ports.recorder == 1); CHECK(ports.lan == 1);
    CHECK(jce_trace_task_current_id() == parent);
    JceTraceStats stats = {}; jce_trace_get_stats(&stats);
    CHECK(stats.recorded_events == 0); CHECK(stats.tasks_submitted == 0);
    CHECK(stats.active_work_items == 0); CHECK(stats.active_threads == 0);
    CHECK(s_editor.frame_kpi_index == 19); CHECK(s_editor.autoplay_frame == 23);
    jce_trace_task_restore_id(0);
}
