/* Complete production probe with counted process/clock ports. No child or UI. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#undef jce_process_spawn
#undef jce_process_read_stdout
#undef jce_process_read_stderr
#undef jce_process_poll_exit
#undef jce_process_force_kill
#undef jce_process_destroy
#undef jce_process_get_last_spawn_error
#undef jce_time_ticks_ms
#undef jce_thread_sleep_ms
#include "core/jce_build_tool_probe.h"
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_trace.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

struct JceProcess { bool running = true, owned = true; };
namespace {
struct Ports {
    JceProcess process;
    std::string out = "cmake version 3.31.0\n", err;
    size_t out_at = 0, err_at = 0;
    unsigned spawn = 0, out_reads = 0, err_reads = 0, polls = 0;
    unsigned kills = 0, destroys = 0, reaps = 0, sleeps = 0;
    unsigned exit_after_polls = 1;
    int exit_code = 0;
    uint64_t now = 0, read_ms = 0, cleanup_ms = 0;
    bool spawn_ok = true, kill_ok = true, continuous_stdout = false;
    bool output_after_exit = false;
    std::vector<char> reads;
} ports;

void reset()
{
    ports = Ports{};
    jce_trace_set_enabled(false);
    jce_trace_reset();
}
size_t read_stream(bool stderr_stream, char *dst, size_t cap)
{
    CHECK(ports.process.owned);
    CHECK(cap == 256);
    ports.reads.push_back(stderr_stream ? 'E' : 'O');
    if (stderr_stream) ++ports.err_reads; else ++ports.out_reads;
    ports.now += ports.read_ms;
    if (ports.output_after_exit && ports.process.running) return 0;
    if (!stderr_stream && ports.continuous_stdout && ports.polls < ports.exit_after_polls) {
        std::memset(dst, 'x', cap);
        return cap;
    }
    auto &bytes = stderr_stream ? ports.err : ports.out;
    auto &offset = stderr_stream ? ports.err_at : ports.out_at;
    const size_t got = std::min(cap, bytes.size() - offset);
    std::memcpy(dst, bytes.data() + offset, got);
    offset += got;
    return got;
}
void consumed_once(const JceBuildToolProbeResult &result)
{
    CHECK(ports.spawn == 1);
    CHECK(ports.destroys == 1);
    CHECK(ports.reaps == 1);
    CHECK_FALSE(ports.process.owned);
    CHECK_FALSE(ports.process.running);
    CHECK(std::strlen(result.stdout_prefix) <= 1023);
    CHECK(std::strlen(result.stderr_prefix) <= 1023);
}
}

extern "C" JceProcess *jce_test_tool_probe_spawn(const JceProcessConfig *cfg)
{
    ++ports.spawn;
    CHECK(std::string(cfg->executable_path) == "/owned tools/cmake");
    CHECK(std::string(cfg->arguments) == "--version");
    CHECK(cfg->capture_stdout); CHECK(cfg->capture_stderr);
    CHECK(cfg->working_directory == nullptr);
    return ports.spawn_ok ? &ports.process : nullptr;
}
extern "C" size_t jce_test_tool_probe_stdout(JceProcess *, char *dst, size_t cap)
{ return read_stream(false, dst, cap); }
extern "C" size_t jce_test_tool_probe_stderr(JceProcess *, char *dst, size_t cap)
{ return read_stream(true, dst, cap); }
extern "C" bool jce_test_tool_probe_poll(JceProcess *, int *code)
{
    ++ports.polls;
    if (ports.polls < ports.exit_after_polls) return false;
    ports.process.running = false;
    *code = ports.exit_code;
    return true;
}
extern "C" bool jce_test_tool_probe_kill(JceProcess *)
{
    ++ports.kills;
    if (ports.kill_ok) ports.process.running = false;
    return ports.kill_ok;
}
extern "C" void jce_test_tool_probe_destroy(JceProcess *)
{
    ++ports.destroys;
    /* Public destroy owns fallback kill and blocking reap, even after kill failure. */
    ports.process.running = false;
    ports.process.owned = false;
    ++ports.reaps;
    ports.now += ports.cleanup_ms;
}
extern "C" const char *jce_test_tool_probe_spawn_error()
{ return "owned spawn context: executable refused"; }
extern "C" uint64_t JCE_CALL jce_test_tool_probe_ticks()
{ return ports.now; }
extern "C" void JCE_CALL jce_test_tool_probe_sleep(uint32_t ms)
{ ++ports.sleeps; ports.now += ms; }

TEST_CASE("exit-zero dual pipe drain admits first nonempty stdout without stderr identity")
{
    reset();
    ports.out = "\r\n\ncmake version 3.31.0\r\nignored next line\n";
    ports.err = "warning: optional feature unavailable\n";
    auto result = jce_build_tool_probe("/owned tools/cmake");
    CHECK(result.state == JceBuildToolProbeState::Ready);
    CHECK(std::string(result.version) == "cmake version 3.31.0");
    CHECK(std::string(result.stderr_prefix) == ports.err);
    CHECK(result.observed_bytes == ports.out.size() + ports.err.size());
    CHECK(result.exited); CHECK(result.drained);
    CHECK(ports.kills == 0); CHECK(ports.sleeps == 0);
    consumed_once(result);
}
TEST_CASE("queued bytes observed only after exit are drained fairly and independently bounded")
{
    reset(); ports.output_after_exit = true;
    ports.out += std::string(2048, 'o'); ports.err = std::string(3072, 'e');
    auto result = jce_build_tool_probe("/owned tools/cmake");
    CHECK(result.state == JceBuildToolProbeState::Ready);
    CHECK(result.stdout_truncated); CHECK(result.stderr_truncated);
    CHECK(std::strlen(result.stdout_prefix) == 1023);
    CHECK(std::strlen(result.stderr_prefix) == 1023);
    CHECK(ports.out_at == ports.out.size()); CHECK(ports.err_at == ports.err.size());
    REQUIRE(ports.reads.size() % 2 == 0);
    for (size_t i = 0; i < ports.reads.size(); i += 2) {
        CHECK(ports.reads[i] == 'O'); CHECK(ports.reads[i + 1] == 'E');
    }
    consumed_once(result);
}
TEST_CASE("stderr banner and empty stdout never authorize an available version")
{
    for (const char *empty : {"", "\r\n\n"}) {
        reset(); ports.out = empty; ports.err = "cmake version 99.0.0\n";
        auto result = jce_build_tool_probe("/owned tools/cmake");
        CHECK(result.state == JceBuildToolProbeState::EmptyStdout);
        CHECK(result.version[0] == '\0');
        CHECK(std::string(result.stderr_prefix) == ports.err);
        consumed_once(result);
    }
}
TEST_CASE("nonzero exit retains actionable stderr and refuses a plausible stdout banner")
{
    reset(); ports.exit_code = 23; ports.err = "required runtime missing\n";
    auto result = jce_build_tool_probe("/owned tools/cmake");
    CHECK(result.state == JceBuildToolProbeState::ExitFailed);
    CHECK(result.exit_code == 23); CHECK(result.version[0] == '\0');
    CHECK(std::string(result.stderr_prefix) == ports.err);
    CHECK(ports.kills == 0); consumed_once(result);
}
TEST_CASE("silent child deadline uses sleep progress and always invokes cleanup")
{
    reset(); ports.out.clear(); ports.exit_after_polls = 450;
    auto result = jce_build_tool_probe("/owned tools/cmake");
    CHECK(result.state == JceBuildToolProbeState::Timeout);
    CHECK(result.elapsed_ms == 2000);
    CHECK(ports.polls == 400); CHECK(ports.sleeps == 400);
    CHECK(result.kill_requested); CHECK(result.kill_accepted);
    CHECK(ports.kills == 1); consumed_once(result);
}
TEST_CASE("continuous stdout cannot starve stderr or bypass a deadline during draining")
{
    reset(); ports.continuous_stdout = true; ports.exit_after_polls = 20;
    ports.read_ms = 200; ports.err = "deadline diagnostic\n";
    auto result = jce_build_tool_probe("/owned tools/cmake");
    CHECK(result.state == JceBuildToolProbeState::Timeout);
    CHECK(result.elapsed_ms == 2000);
    CHECK(ports.out_reads == 5); CHECK(ports.err_reads == 5);
    CHECK(std::string(result.stderr_prefix) == ports.err);
    CHECK(ports.sleeps == 0); CHECK(ports.kills == 1);
    consumed_once(result);
}
TEST_CASE("a read crossing the deadline prevents the next pipe read and late admission")
{
    reset(); ports.read_ms = 2000;
    auto result = jce_build_tool_probe("/owned tools/cmake");
    CHECK(result.state == JceBuildToolProbeState::Timeout);
    CHECK(ports.out_reads == 1); CHECK(ports.err_reads == 0);
    CHECK(ports.polls == 0); CHECK(result.version[0] == '\0');
    consumed_once(result);
    reset(); ports.cleanup_ms = 2000;
    result = jce_build_tool_probe("/owned tools/cmake");
    CHECK(result.state == JceBuildToolProbeState::Timeout);
    CHECK(result.exited); CHECK(result.drained);
    CHECK(result.version[0] == '\0'); consumed_once(result);
}
TEST_CASE("64 KiB total output is admitted but excess refuses both channel combinations")
{
    for (size_t err_size : {size_t(0), size_t(32768), size_t(65536)}) {
        for (size_t extra : {size_t(0), size_t(1)}) {
            reset();
            ports.out = std::string(JCE_BUILD_TOOL_PROBE_OUTPUT_LIMIT - err_size + extra, 'o');
            ports.err = std::string(err_size, 'e');
            auto result = jce_build_tool_probe("/owned tools/cmake");
            CHECK(result.observed_bytes == JCE_BUILD_TOOL_PROBE_OUTPUT_LIMIT + extra);
            const auto expected = extra ? JceBuildToolProbeState::OutputLimit :
                (ports.out.empty() ? JceBuildToolProbeState::EmptyStdout : JceBuildToolProbeState::Ready);
            CHECK(result.state == expected);
            if (extra) CHECK_FALSE(result.drained);
            consumed_once(result);
        }
    }
}
TEST_CASE("failed kill still delegates cleanup and spawn failure preserves current context")
{
    reset(); ports.kill_ok = false; ports.read_ms = 2000;
    auto result = jce_build_tool_probe("/owned tools/cmake");
    CHECK(result.state == JceBuildToolProbeState::Timeout);
    CHECK(result.kill_requested); CHECK_FALSE(result.kill_accepted);
    consumed_once(result);
    reset(); ports.spawn_ok = false;
    result = jce_build_tool_probe("/owned tools/cmake");
    CHECK(result.state == JceBuildToolProbeState::SpawnFailed);
    CHECK(std::string(result.spawn_error) == "owned spawn context: executable refused");
    CHECK(ports.destroys == 0); CHECK(ports.kills == 0); CHECK(ports.out_reads == 0);
    reset(); result = jce_build_tool_probe(nullptr);
    CHECK(result.state == JceBuildToolProbeState::Missing); CHECK(ports.spawn == 0);
}
TEST_CASE("running output overflow kills once and elapsed subtraction survives tick wrap")
{
    reset(); ports.exit_after_polls = 512;
    ports.out += std::string(JCE_BUILD_TOOL_PROBE_OUTPUT_LIMIT, 'x');
    auto result = jce_build_tool_probe("/owned tools/cmake");
    CHECK(result.state == JceBuildToolProbeState::OutputLimit);
    CHECK(result.observed_bytes > JCE_BUILD_TOOL_PROBE_OUTPUT_LIMIT);
    CHECK(result.observed_bytes <= JCE_BUILD_TOOL_PROBE_OUTPUT_LIMIT + 256);
    CHECK(result.kill_requested); CHECK(result.kill_accepted); CHECK(ports.kills == 1);
    consumed_once(result);
    reset(); ports.now = UINT64_MAX - 1000; ports.read_ms = 1000;
    result = jce_build_tool_probe("/owned tools/cmake");
    CHECK(result.state == JceBuildToolProbeState::Timeout);
    CHECK(result.elapsed_ms == 2000); CHECK(ports.kills == 1);
    consumed_once(result);
}
TEST_CASE("canonical trace service closes with observed outcome and restores its parent")
{
    for (bool success : {false, true}) {
        reset(); ports.exit_code = success ? 0 : 23;
        jce_trace_set_enabled(true);
        const uint64_t parent = jce_trace_next_id();
        jce_trace_task_submit(parent, 0, JCE_TRACE_TASK_SERVICE, "test.probe.parent");
        jce_trace_task_begin(parent, parent, "test.probe.parent", 0);
        auto result = jce_build_tool_probe("/owned tools/cmake");
        CHECK(jce_trace_task_current_id() == parent);
        jce_trace_task_end(parent, parent, "test.probe.parent", JCE_TRACE_TASK_SUCCEEDED, 0);
        jce_trace_task_restore_id(0);
        JceTraceCursor cursor{}; jce_trace_cursor_init(&cursor);
        JceTraceEvent events[64]{};
        const auto count = jce_trace_read(&cursor, events, 64);
        unsigned submit = 0, begin = 0, end = 0;
        uint64_t child = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const auto &event = events[i];
            if (std::strcmp(event.name, "editor.build.tool_probe")) continue;
            if (event.type == JCE_TRACE_EVENT_TASK_SUBMIT) {
                ++submit; child = event.id; CHECK(event.parent_id == parent);
                CHECK(event.value_u32[0] == JCE_TRACE_TASK_SERVICE);
            }
            if (event.type == JCE_TRACE_EVENT_TASK_BEGIN) { ++begin; CHECK(event.id == child); CHECK(event.parent_id == child); }
            if (event.type == JCE_TRACE_EVENT_TASK_END) {
                ++end; CHECK(event.id == child); CHECK(event.parent_id == child);
                CHECK(event.value_u32[0] == (success ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED));
            }
        }
        CHECK(submit == 1); CHECK(begin == 1); CHECK(end == 1);
        JceTraceStats stats{}; jce_trace_get_stats(&stats);
        CHECK(stats.active_work_items == 0);
        consumed_once(result); jce_trace_set_enabled(false);
    }
}
TEST_CASE("trace-off probe records no service and preserves the caller identity")
{
    reset();
    const uint64_t parent = UINT64_C(9007199254741017);
    jce_trace_task_restore_id(parent);
    auto result = jce_build_tool_probe("/owned tools/cmake");
    CHECK(result.state == JceBuildToolProbeState::Ready);
    CHECK(jce_trace_task_current_id() == parent);
    JceTraceStats stats{}; jce_trace_get_stats(&stats);
    CHECK(stats.recorded_events == 0); CHECK(stats.tasks_submitted == 0);
    CHECK(stats.active_work_items == 0);
    consumed_once(result); jce_trace_task_restore_id(0);
}
