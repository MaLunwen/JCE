#include "jce_build_tool_probe.h"
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_trace.h>
#include <algorithm>
#include <cstring>

namespace {
constexpr size_t kReadCap = 256;

bool budget_expired(uint64_t started)
{
    return jce_time_ticks_ms() - started >= JCE_BUILD_TOOL_PROBE_TIMEOUT_MS;
}

void retain_prefix(char *prefix, size_t &retained, bool &truncated,
                   const char *bytes, size_t count)
{
    const size_t keep = std::min(count, JCE_BUILD_TOOL_PROBE_PREFIX_CAP - 1 - retained);
    std::memcpy(prefix + retained, bytes, keep);
    retained += keep;
    prefix[retained] = '\0';
    truncated = truncated || keep != count;
}

bool first_nonempty_line(const char *bytes, size_t size, char *version)
{
    size_t start = 0;
    while (start < size && (bytes[start] == '\r' || bytes[start] == '\n')) ++start;
    size_t end = start;
    while (end < size && bytes[end] != '\r' && bytes[end] != '\n' && bytes[end] != '\0') ++end;
    const size_t count = end - start;
    if (!count) return false;
    std::memcpy(version, bytes + start, count);
    version[count] = '\0';
    return true;
}

/* Static-name CPU service records use the existing public trace registry.
 * The scope includes process cleanup, not external build or GPU completion. */
struct ProbeTrace {
    JceBuildToolProbeResult &result;
    uint64_t id = 0, parent = 0, started = 0;
    explicit ProbeTrace(JceBuildToolProbeResult &value) : result(value) {
        if (!jce_trace_enabled()) return;
        parent = jce_trace_task_current_id();
        id = jce_trace_next_id();
        started = jce_time_perf_counter();
        jce_trace_task_submit(id, parent, JCE_TRACE_TASK_SERVICE, "editor.build.tool_probe");
        jce_trace_task_begin(id, id, "editor.build.tool_probe", 0);
    }
    ProbeTrace(const ProbeTrace &) = delete;
    ProbeTrace &operator=(const ProbeTrace &) = delete;
    ~ProbeTrace() {
        if (!id) return;
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "editor.build.tool_probe.state", static_cast<int>(result.state));
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "editor.build.tool_probe.observed_bytes", result.observed_bytes);
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "editor.build.tool_probe.elapsed_ms", result.elapsed_ms);
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "editor.build.tool_probe.stdout_truncated", result.stdout_truncated);
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "editor.build.tool_probe.stderr_truncated", result.stderr_truncated);
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "editor.build.tool_probe.kill_accepted", result.kill_accepted);
        const double ms = jce_time_perf_to_ms(started, jce_time_perf_counter());
        jce_trace_task_end(id, id, "editor.build.tool_probe",
            result.state == JceBuildToolProbeState::Ready ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED,
            ms > 0 ? static_cast<uint64_t>(ms * 1000000.0) : 0);
        jce_trace_task_restore_id(parent);
    }
};
}

const char *jce_build_tool_probe_state_name(JceBuildToolProbeState state)
{
    switch (state) {
    case JceBuildToolProbeState::Missing: return "executable unavailable";
    case JceBuildToolProbeState::SpawnFailed: return "spawn failed";
    case JceBuildToolProbeState::Timeout: return "version query timed out";
    case JceBuildToolProbeState::OutputLimit: return "version query output exceeds 64 KiB";
    case JceBuildToolProbeState::ExitFailed: return "version query exited nonzero";
    case JceBuildToolProbeState::EmptyStdout: return "version query has no stdout identity";
    case JceBuildToolProbeState::Ready: return "ready";
    }
    return "invalid probe state";
}

JceBuildToolProbeResult jce_build_tool_probe(const char *resolved_executable)
{
    JceBuildToolProbeResult result;
    ProbeTrace trace(result);
    if (!resolved_executable || !resolved_executable[0]) return result;
    const uint64_t started = jce_time_ticks_ms();
    JceProcessConfig config{};
    config.executable_path = resolved_executable;
    config.arguments = "--version";
    config.capture_stdout = config.capture_stderr = true;
    JceProcess *process = jce_process_spawn(&config);
    if (!process) {
        const char *error = jce_process_get_last_spawn_error();
        if (error) {
            const size_t count = std::min(std::strlen(error), sizeof(result.spawn_error) - 1);
            std::memcpy(result.spawn_error, error, count);
        }
        result.state = JceBuildToolProbeState::SpawnFailed;
        result.elapsed_ms = jce_time_ticks_ms() - started;
        return result;
    }

    size_t stdout_size = 0, stderr_size = 0;
    bool finished = false;
    while (!finished) {
        bool progress = false;
        /* Fair, finite rounds keep stderr draining even under continuous stdout. */
        for (int stream = 0; stream < 2; ++stream) {
            if (budget_expired(started)) {
                result.state = JceBuildToolProbeState::Timeout;
                finished = true;
                break;
            }
            char bytes[kReadCap];
            const size_t got = stream == 0
                ? jce_process_read_stdout(process, bytes, sizeof(bytes))
                : jce_process_read_stderr(process, bytes, sizeof(bytes));
            progress = progress || got != 0;
            result.observed_bytes += got;
            if (result.observed_bytes > JCE_BUILD_TOOL_PROBE_OUTPUT_LIMIT) {
                result.state = JceBuildToolProbeState::OutputLimit;
                finished = true;
                break;
            }
            if (stream == 0)
                retain_prefix(result.stdout_prefix, stdout_size, result.stdout_truncated, bytes, got);
            else
                retain_prefix(result.stderr_prefix, stderr_size, result.stderr_truncated, bytes, got);
        }
        if (finished) break;
        /* Completion admission is also within the same elapsed-time budget. */
        if (budget_expired(started)) {
            result.state = JceBuildToolProbeState::Timeout;
            break;
        }
        const bool was_exited = result.exited;
        if (!result.exited)
            result.exited = jce_process_poll_exit(process, &result.exit_code);
        /* A zero read before observing exit cannot certify the final queued
         * bytes. Require one empty dual-stream round after that observation. */
        if (was_exited && !progress) { result.drained = true; break; }
        if (!progress && !result.exited) jce_thread_sleep_ms(5);
    }
    if (!result.exited) {
        result.kill_requested = true;
        result.kill_accepted = jce_process_force_kill(process);
    }
    jce_process_destroy(process);
    result.elapsed_ms = jce_time_ticks_ms() - started;
    if (result.drained && budget_expired(started)) {
        result.state = JceBuildToolProbeState::Timeout;
        return result;
    }
    if (!result.drained) return result;
    if (result.exit_code != 0) {
        result.state = JceBuildToolProbeState::ExitFailed;
        return result;
    }
    if (!first_nonempty_line(result.stdout_prefix, stdout_size, result.version)) {
        result.state = JceBuildToolProbeState::EmptyStdout;
        return result;
    }
    result.state = JceBuildToolProbeState::Ready;
    return result;
}
