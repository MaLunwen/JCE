/* Actual Build structs/log/pipe/release/spawn/stop/poll bodies and shared CPU
 * host-services owner. Process/publisher ports are counted, not OS children/UI. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_build_log.h"
#include "core/jce_build_process_outcome.h"
#include "core/jce_editor_host_services.h"
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_log.h>
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#ifndef JCE_TEST_BUILD_COMPLETION_BODIES
#error actual Build completion owner bodies are required
#endif

struct JceProcess { bool owned = true, running = true; unsigned identity = 0; };
namespace {
struct Ports {
    JceProcess first, second;
    bool exited = true, spawn_ok = true, kill_ok = true, exit_on_kill = true;
    bool graceful_ok = true, finish_ok = true;
    int exit_code = 0;
    uint64_t now = 10;
    unsigned destroys = 0, reaps = 0, kills = 0, graceful = 0, spawns = 0;
    unsigned console = 0, flushes = 0, finish = 0, publish = 0, asset_polls = 0;
    unsigned run_polls = 0, cook_polls = 0, serial = 0, index = 0, runner = 0, maintenance = 0;
    std::string stdout_bytes = "current stdout\n", stderr_bytes = "current stderr\n";
    size_t stdout_at = 0, stderr_at = 0;
    std::vector<JceConsoleLevel> output_levels;
    std::vector<std::string> output_lines;
    std::string public_artifact = "previous valid public artifact";
} ports;

size_t read_pipe(bool err, JceProcess *process, char *bytes, size_t cap)
{
    CHECK(process != nullptr); CHECK(process->owned);
    auto &source = err ? ports.stderr_bytes : ports.stdout_bytes;
    auto &offset = err ? ports.stderr_at : ports.stdout_at;
    const size_t got = std::min(cap, source.size() - offset);
    std::memcpy(bytes, source.data() + offset, got); offset += got;
    return got;
}
size_t probe_stdout(JceProcess *p, char *b, size_t c) { return read_pipe(false, p, b, c); }
size_t probe_stderr(JceProcess *p, char *b, size_t c) { return read_pipe(true, p, b, c); }
bool probe_poll(JceProcess *p, int *code)
{
    CHECK((p == &ports.first || p == &ports.second)); CHECK(p->owned);
    if (!ports.exited) return false;
    p->running = false; *code = ports.exit_code; return true;
}
bool probe_kill(JceProcess *p)
{
    CHECK(p->owned); ++ports.kills;
    if (ports.kill_ok && ports.exit_on_kill) { p->running = false; ports.exited = true; }
    return ports.kill_ok;
}
bool probe_graceful(JceProcess *p) { CHECK(p->owned); ++ports.graceful; return ports.graceful_ok; }
void probe_destroy(JceProcess *p)
{
    CHECK(p->owned); ++ports.destroys; ++ports.reaps;
    p->owned = p->running = false;
}
JceProcess *probe_spawn(const JceProcessConfig *cfg)
{
    ++ports.spawns;
    CHECK(std::string(cfg->executable_path) == "/owned/next");
    CHECK(cfg->capture_stdout); CHECK(cfg->capture_stderr);
    CHECK(ports.destroys == 1); /* Old handle is released before the next one. */
    if (!ports.spawn_ok) return nullptr;
    ports.exited = false; ports.second = {}; ports.second.identity = 2;
    return &ports.second;
}
const char *probe_spawn_error() { return "owned spawn failure"; }
uint64_t probe_ticks() { return ports.now; }
void probe_log_flush() { ++ports.flushes; }
bool probe_async_cancel(JceAsyncTask *) { FAIL("process completion must not cancel an unrelated asset task"); return false; }

static_assert(std::is_same_v<decltype(&probe_stdout), decltype(&jce_process_read_stdout)>);
static_assert(std::is_same_v<decltype(&probe_stderr), decltype(&jce_process_read_stderr)>);
static_assert(std::is_same_v<decltype(&probe_poll), decltype(&jce_process_poll_exit)>);
static_assert(std::is_same_v<decltype(&probe_kill), decltype(&jce_process_force_kill)>);
static_assert(std::is_same_v<decltype(&probe_graceful), decltype(&jce_process_request_stop)>);
static_assert(std::is_same_v<decltype(&probe_destroy), decltype(&jce_process_destroy)>);
static_assert(std::is_same_v<decltype(&probe_spawn), decltype(&jce_process_spawn)>);
static_assert(std::is_same_v<decltype(&probe_async_cancel), decltype(&jce_async_task_cancel)>);

#define jce_process_read_stdout probe_stdout
#define jce_process_read_stderr probe_stderr
#define jce_process_poll_exit probe_poll
#define jce_process_force_kill probe_kill
#define jce_process_request_stop probe_graceful
#define jce_process_destroy probe_destroy
#define jce_process_spawn probe_spawn
#define jce_process_get_last_spawn_error probe_spawn_error
#define jce_time_ticks_ms probe_ticks
#define jce_log_flush probe_log_flush
#define jce_async_task_cancel probe_async_cancel
#undef LOG_INFO
#define LOG_INFO(...) ((void)0)

using WorkerLogSink = JceBuildWorkerLog<JceConsoleLevel>;
WorkerLogSink *t_log_sink = nullptr;
std::string g_default_working_dir;
struct PendingPort { bool active = false; JceAsyncTask *task = nullptr; bool stop_requested = false; } g_pending;
std::string jce_build_tool_executable(const char *name) { return std::string("/owned/") + name; }
void poll_asset_prep() { ++ports.asset_polls; }
void run_finish_plan();

#include JCE_TEST_BUILD_COMPLETION_BODIES

void run_finish_plan()
{
    ++ports.finish;
    if (!ports.finish_ok) { g_build.state = JCE_BUILD_FAILED; g_build.last_error = "owned finish failure"; return; }
    ++ports.publish; ports.public_artifact = "current public artifact";
    g_build.artifact_path = "/current/artifact.exe";
    g_build.package_path = "/current/package";
}

void reset(bool tracing = false)
{
    ports = Ports{}; ports.first.identity = 1;
    g_build = Build{}; g_build.process = &ports.first; g_build.state = JCE_BUILD_RUNNING;
    g_build.stage = JCE_BUILD_STAGE_COMPILE; g_build.preset = "current request";
    g_queue.clear(); g_queue_pos = 0; g_finish = FinishPlan{};
    g_finish.verify = g_finish.stage = true;
    std::memset(g_finish.pak_key, 0x57, sizeof(g_finish.pak_key));
    g_pending = {}; t_log_sink = nullptr; g_default_working_dir.clear();
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0); jce_trace_reset();
    jce_trace_set_enabled(tracing);
}
void queue_next()
{
    QueuedStep step; step.stage = JCE_BUILD_STAGE_COMPILE; step.exe = "next";
    step.label = "next current step"; step.args = "--owned";
    g_queue.push_back(step);
}
void check_released()
{
    CHECK(g_build.process == nullptr);
    CHECK(ports.destroys == 1); CHECK(ports.reaps == 1);
    CHECK_FALSE(ports.first.owned); CHECK_FALSE(g_build.stopping);
    CHECK(g_queue.empty()); CHECK(g_queue_pos == 0);
    CHECK_FALSE(g_finish.verify); CHECK_FALSE(g_finish.stage);
    for (auto byte : g_finish.pak_key) CHECK(byte == 0);
}
void check_cancelled(int code)
{
    CHECK(g_build.state == JCE_BUILD_FAILED);
    CHECK(g_build.exit_code == code);
    CHECK(g_build.last_error == "build cancelled (tool exit code " + std::to_string(code) + ")");
    CHECK(g_build.log.text.find(g_build.last_error) != std::string::npos);
    CHECK(g_build.log.text.find("step finished OK") == std::string::npos);
    CHECK(ports.spawns == 0); CHECK(ports.finish == 0); CHECK(ports.publish == 0);
    CHECK(ports.public_artifact == "previous valid public artifact");
    CHECK(g_build.artifact_path.empty()); CHECK(g_build.package_path.empty());
    check_released();
}
}

/* Match the real global declaration. JceConsoleLevel ADL must not find a
 * competing anonymous-namespace function. No production body is altered. */
extern "C" void jce_editor_console_log_level(JceConsoleLevel level, const char *fmt, ...)
{
    ++ports.console;
    char bytes[2048];
    va_list args; va_start(args, fmt);
    std::vsnprintf(bytes, sizeof bytes, fmt, args); va_end(args);
    ports.output_levels.push_back(level);
    ports.output_lines.emplace_back(bytes);
}

/* Consume the same production host-maintenance owner without drawing a panel.
 * Existing host-services controls separately cover observed native flag ports. */
extern "C" void jce_build_manager_poll() { probe_manager_poll(); }
extern "C" void jce_build_manager_request_stop() { probe_request_stop(); }
extern "C" JceRunState jce_run_manager_state() { return JCE_RUN_IDLE; }
extern "C" void jce_run_manager_poll() { ++ports.run_polls; }
extern "C" void jce_cook_manager_poll() { ++ports.cook_polls; }
extern "C" void jce_state_scene_serial_poll() { ++ports.serial; }
extern "C" void jce_asset_path_index_poll() { ++ports.index; }
extern "C" void jce_editor_panel_test_runner_tick() { ++ports.runner; }
extern "C" void jce_editor_panel_console_poll(void) {}
extern "C" void jce_editor_tick_cpu_services(float, bool) { ++ports.maintenance; }
void jce_editor_headless_build_poll() { }

TEST_CASE("actual pipe diagnostics separate severity from stdout and stderr origin")
{
    reset();
    ports.stdout_bytes = "LINK : fatal error LNK1104: cannot open file 'busy.dll'\n"
                         "[1/2] Building error_demo.cpp.obj\n"
                         "source.cpp(12): warning C4100: unused parameter\n";
    ports.stderr_bytes = "CMake Warning:\n  manually specified variables were not used\n"
                         "unclassified stderr note\n";
    drain_pipes();
    REQUIRE(ports.output_levels.size() == 6);
    CHECK_MESSAGE(ports.output_levels[0] == JCE_CONSOLE_ERROR,
                  "fatal linker output on stdout must be an error");
    CHECK(ports.output_levels[1] == JCE_CONSOLE_INFO);
    CHECK(ports.output_levels[2] == JCE_CONSOLE_WARNING);
    CHECK_MESSAGE(ports.output_levels[3] == JCE_CONSOLE_WARNING,
                  "CMake warning on stderr must not become an error");
    CHECK(ports.output_levels[4] == JCE_CONSOLE_INFO);
    CHECK(ports.output_levels[5] == JCE_CONSOLE_INFO);
    CHECK(ports.output_lines[0].find("LINK : fatal error LNK1104") != std::string::npos);
    CHECK(ports.output_lines[3].find("[stderr]") != std::string::npos);
    CHECK(ports.output_lines[5].find("unclassified stderr note") != std::string::npos);
    const JceBuildLogView transcript = g_build.log.view();
    CHECK(transcript.size > 0);
}

TEST_CASE("diagnostics survive chunk boundaries and EOF without changing completion policy")
{
    reset(); g_build.log.reset();
    const char *first = "file.cpp:9:4: err";
    const char *last = "or: missing symbol\r\n";
    emit_lines(g_build.stdout_partial, first, std::strlen(first), JCE_CONSOLE_INFO);
    CHECK(ports.output_lines.empty());
    emit_lines(g_build.stdout_partial, last, std::strlen(last), JCE_CONSOLE_INFO);
    REQUIRE(ports.output_lines.size() == 1);
    CHECK(ports.output_levels[0] == JCE_CONSOLE_ERROR);
    CHECK(ports.output_lines[0] == "[build] file.cpp:9:4: error: missing symbol");
    const char *tail = "CMake Warning (dev) at project.cmake:3";
    emit_lines(g_build.stderr_partial, tail, std::strlen(tail), JCE_CONSOLE_ERROR);
    CHECK_FALSE(g_build.log.incomplete);
    release_process();
    REQUIRE(ports.output_lines.size() == 2);
    CHECK(ports.output_levels[1] == JCE_CONSOLE_WARNING);
    CHECK(ports.output_lines[1] == std::string("[build] [stderr] ") + tail);
    CHECK(g_build.log.incomplete); CHECK(g_build.stderr_partial.empty());
    CHECK(g_build.state == JCE_BUILD_RUNNING); // Classification is not tool completion.
    reset(); ports.stdout_bytes = "source.cpp: error: diagnostic text\n";
    ports.stderr_bytes.clear(); jce_build_manager_poll();
    CHECK(g_build.state == JCE_BUILD_SUCCEEDED); CHECK(ports.publish == 1);
}

TEST_CASE("diagnostic forms do not mistake source paths for compiler messages")
{
    const struct { const char *line; JceConsoleLevel expected; } cases[] = {
        {"source.cpp(9): error C2065: unknown identifier", JCE_CONSOLE_ERROR},
        {"build.proj: error MSB3073: tool returned 1", JCE_CONSOLE_ERROR},
        {"CMake Error at CMakeLists.txt:4 (add_library):", JCE_CONSOLE_ERROR},
        {"ninja: error: rebuilding build.ninja", JCE_CONSOLE_ERROR},
        {"ninja: build stopped: subcommand failed.", JCE_CONSOLE_ERROR},
        {"FAILED: target.exe", JCE_CONSOLE_ERROR},
        {"file.c:3: warning: variable unused", JCE_CONSOLE_WARNING},
        {"warning C4996: message includes error: as quoted text", JCE_CONSOLE_WARNING},
        {"[3/4] Building obj/error.cpp.obj", JCE_CONSOLE_INFO},
        {"[3/4] Building obj/warning.cpp.obj", JCE_CONSOLE_INFO},
        {"[3/4] Building error_handler.cpp.obj", JCE_CONSOLE_INFO},
        {"everything finished", JCE_CONSOLE_INFO},
    };
    reset();
    for (const auto &sample : cases) {
        std::string partial;
        const std::string bytes = std::string(sample.line) + '\n';
        emit_lines(partial, bytes.data(), bytes.size(), JCE_CONSOLE_INFO);
        REQUIRE_FALSE(ports.output_levels.empty());
        CHECK_MESSAGE(ports.output_levels.back() == sample.expected, sample.line);
        CHECK(ports.output_lines.back() == std::string("[build] ") + sample.line);
    }
}

TEST_CASE("graceful zero and nonzero Stop remain cancelled and never consume a queued step or publisher")
{
    for (int code : {0, 23}) for (bool queued : {false, true}) {
        reset(); ports.exit_code = code;
        if (queued) queue_next();
        jce_build_manager_request_stop();
        CHECK(ports.graceful == 1); CHECK(g_build.stopping);
        jce_build_manager_poll();
        check_cancelled(code);
        CHECK(g_build.log.text.find("current stdout") != std::string::npos);
        CHECK(g_build.log.text.find("current stderr") != std::string::npos);
    }
}
TEST_CASE("ordinary zero exit admits the actual finish seam and nonzero never publishes")
{
    reset(); jce_build_manager_poll();
    CHECK(g_build.state == JCE_BUILD_SUCCEEDED); CHECK(g_build.exit_code == 0);
    CHECK(g_build.last_error.empty()); CHECK(ports.finish == 1); CHECK(ports.publish == 1);
    CHECK(ports.public_artifact == "current public artifact"); check_released();
    reset(); ports.exit_code = 23; queue_next(); jce_build_manager_poll();
    CHECK(g_build.state == JCE_BUILD_FAILED); CHECK(g_build.exit_code == 23);
    CHECK(g_build.last_error == "tool exited with code 23");
    CHECK(ports.spawns == 0); CHECK(ports.finish == 0); CHECK(ports.publish == 0);
    CHECK(ports.public_artifact == "previous valid public artifact"); check_released();
}
TEST_CASE("successful next step and actual failed spawn preserve their distinct results")
{
    for (bool spawn_ok : {false, true}) {
        reset(); ports.spawn_ok = spawn_ok; queue_next(); jce_build_manager_poll();
        CHECK(ports.spawns == 1); CHECK(ports.destroys == 1); CHECK(ports.reaps == 1);
        CHECK(ports.finish == 0); CHECK(ports.publish == 0);
        if (spawn_ok) {
            CHECK(g_build.state == JCE_BUILD_RUNNING); CHECK(g_build.process == &ports.second);
            CHECK(g_build.last_error.empty()); CHECK(g_queue_pos == 1); CHECK(g_finish.verify);
            /* Complete through the same owner; never leak the second handle. */
            ports.exited = true; jce_build_manager_poll();
            CHECK(g_build.state == JCE_BUILD_SUCCEEDED); CHECK(ports.destroys == 2); CHECK(ports.publish == 1);
        } else {
            CHECK(g_build.state == JCE_BUILD_FAILED); CHECK(g_build.process == nullptr);
            CHECK(g_build.last_error.find("failed to spawn next") != std::string::npos);
            CHECK(g_build.last_error.find("owned spawn failure") != std::string::npos);
            CHECK(g_queue.empty()); CHECK_FALSE(g_finish.verify);
        }
    }
}
TEST_CASE("a live child remains owned and progresses from CPU maintenance without any drawn panel")
{
    reset(); ports.exited = false;
    jce_editor_host_services_poll(false, 0.02f, true);
    CHECK(g_build.state == JCE_BUILD_RUNNING); CHECK(g_build.process == &ports.first);
    CHECK(ports.asset_polls == 1); CHECK(ports.run_polls == 1); CHECK(ports.runner == 1);
    CHECK(ports.destroys == 0); CHECK(ports.finish == 0); CHECK(ports.publish == 0);
    jce_build_manager_request_stop();
    jce_editor_host_services_poll(false, 0.02f, true);
    CHECK(g_build.state == JCE_BUILD_RUNNING); CHECK(g_build.stopping);
    CHECK(ports.destroys == 0); CHECK(ports.kills == 0);
    ports.exited = true;
    jce_editor_host_services_poll(false, 0.02f, true);
    CHECK(ports.asset_polls == 3); CHECK(ports.maintenance == 3); check_cancelled(0);
}
TEST_CASE("graceful refusal and overdue kill preserve the Stop disposition")
{
    for (bool kill_ok : {false, true}) {
        reset(); ports.exited = false; ports.graceful_ok = false; ports.kill_ok = kill_ok;
        queue_next(); jce_build_manager_request_stop();
        const uint64_t deadline = g_build.stop_deadline_ms;
        ports.now = deadline - 1; jce_build_manager_poll();
        CHECK(ports.kills == 0); CHECK(g_build.process == &ports.first);
        ports.now = deadline; jce_build_manager_poll();
        CHECK(ports.kills == 1);
        if (!kill_ok) {
            CHECK(g_build.state == JCE_BUILD_RUNNING); CHECK(ports.destroys == 0);
            ports.exited = true; ports.exit_code = 23; jce_build_manager_poll();
            check_cancelled(23);
        } else check_cancelled(0);
        CHECK(g_build.log.text.find("force-killing tool") != std::string::npos);
    }
}
TEST_CASE("completion trace records actual success failure cancellation and transition failure")
{
    for (int mode = 0; mode < 5; ++mode) {
        reset(true);
        if (mode == 1) ports.exit_code = 23;
        if (mode == 2) jce_build_manager_request_stop();
        if (mode == 3) { ports.spawn_ok = false; queue_next(); }
        if (mode == 4) ports.finish_ok = false;
        const uint64_t parent = jce_trace_next_id();
        jce_trace_task_submit(parent, 0, JCE_TRACE_TASK_SERVICE, "test.build.parent");
        jce_trace_task_begin(parent, parent, "test.build.parent", 0);
        jce_build_manager_poll();
        CHECK(jce_trace_task_current_id() == parent);
        jce_trace_task_end(parent, parent, "test.build.parent", JCE_TRACE_TASK_SUCCEEDED, 0);
        jce_trace_task_restore_id(0);
        JceTraceCursor cursor{}; jce_trace_cursor_init(&cursor);
        JceTraceEvent events[64]{}; const auto count = jce_trace_read(&cursor, events, 64);
        unsigned submit = 0, begin = 0, end = 0; uint64_t id = 0;
        const auto expected = mode == 2 ? JCE_TRACE_TASK_CANCELLED :
            mode == 0 ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED;
        for (uint32_t i = 0; i < count; ++i) {
            const auto &event = events[i];
            if (std::strcmp(event.name, "editor.build.tool_completion")) continue;
            if (event.type == JCE_TRACE_EVENT_TASK_SUBMIT) {
                ++submit; id = event.id; CHECK(event.parent_id == parent);
                CHECK(event.value_u32[0] == JCE_TRACE_TASK_SERVICE);
            } else if (event.type == JCE_TRACE_EVENT_TASK_BEGIN) {
                ++begin; CHECK(event.id == id); CHECK(event.parent_id == id);
            } else if (event.type == JCE_TRACE_EVENT_TASK_END) {
                ++end; CHECK(event.id == id); CHECK(event.parent_id == id);
                CHECK(event.value_u32[0] == expected);
            }
        }
        CHECK(submit == 1); CHECK(begin == 1); CHECK(end == 1); CHECK(cursor.lost_events == 0);
        JceTraceStats stats{}; jce_trace_get_stats(&stats); CHECK(stats.active_work_items == 0);
        jce_trace_set_enabled(false);
    }
}
TEST_CASE("trace-off completion keeps the caller identity and emits no observation")
{
    reset(); const uint64_t parent = UINT64_C(9007199254741017); jce_trace_task_restore_id(parent);
    jce_build_manager_request_stop(); jce_build_manager_poll(); check_cancelled(0);
    CHECK(jce_trace_task_current_id() == parent);
    JceTraceStats stats{}; jce_trace_get_stats(&stats);
    CHECK(stats.recorded_events == 0); CHECK(stats.active_work_items == 0);
    jce_trace_task_restore_id(0);
}
