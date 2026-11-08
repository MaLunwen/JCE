/* Private Build tool-version admission through the public process owner. */
#ifndef JCE_BUILD_TOOL_PROBE_H
#define JCE_BUILD_TOOL_PROBE_H

#include <stddef.h>
#include <stdint.h>

constexpr uint64_t JCE_BUILD_TOOL_PROBE_TIMEOUT_MS = 2000;
constexpr size_t JCE_BUILD_TOOL_PROBE_PREFIX_CAP = 1024;
constexpr size_t JCE_BUILD_TOOL_PROBE_OUTPUT_LIMIT = 64 * 1024;

enum class JceBuildToolProbeState {
    Missing, SpawnFailed, Timeout, OutputLimit, ExitFailed, EmptyStdout, Ready
};

struct JceBuildToolProbeResult {
    JceBuildToolProbeState state = JceBuildToolProbeState::Missing;
    int exit_code = -1;
    uint64_t elapsed_ms = 0;
    size_t observed_bytes = 0;
    bool stdout_truncated = false, stderr_truncated = false;
    bool exited = false, drained = false;
    bool kill_requested = false, kill_accepted = false;
    char version[JCE_BUILD_TOOL_PROBE_PREFIX_CAP]{};
    char stdout_prefix[JCE_BUILD_TOOL_PROBE_PREFIX_CAP]{};
    char stderr_prefix[JCE_BUILD_TOOL_PROBE_PREFIX_CAP]{};
    char spawn_error[512]{};
};

/* Each nonblocking pipe read is preceded by a monotonic budget check.
 * Retention and total observed-output admission are independent bounds.
 * The public destroy operation owns final kill/reap; this cooperative timeout
 * cannot bound a synchronous spawn or an OS cleanup call that does not return. */
JceBuildToolProbeResult jce_build_tool_probe(const char *resolved_executable);
const char *jce_build_tool_probe_state_name(JceBuildToolProbeState state);

#endif
