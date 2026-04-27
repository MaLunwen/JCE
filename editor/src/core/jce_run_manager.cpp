/*
 * jce_run_manager.cpp  External game subprocess lifecycle.
 *
 * Thin wrapper around the engine's jce_process_* API.  All process /
 * pipe / kill plumbing lives in <jce/core/jce_process.h>; this file
 * keeps editor-specific concerns: console line buffering, graceful-
 * stop deadline, error-message strings.
 *
 * Threading: all public entry points are expected to be called from
 * the editor main thread.
 */

#include "jce_run_manager.h"

#include "ui/jce_editor_panels.h"

extern "C" {
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_timer.h>
}

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include <jce/os/core/jce_defs.h>

namespace {

#if JCE_PLATFORM_WINDOWS
static constexpr const char *kExeSuffix = ".exe";
#else
static constexpr const char *kExeSuffix = "";
#endif

static bool path_exists(const std::string &p)
{
    if (p.empty()) return false;
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

/* Try `candidate` as-is and (on platforms with an exe suffix) with the
 * suffix appended.  Returns the matching path, or empty string. */
static std::string try_with_suffix(const std::string &candidate)
{
    if (path_exists(candidate)) return candidate;
    if (kExeSuffix[0]) {
        size_t slen = std::strlen(kExeSuffix);
        if (candidate.size() < slen ||
            candidate.compare(candidate.size() - slen, slen, kExeSuffix) != 0) {
            std::string with = candidate + kExeSuffix;
            if (path_exists(with)) return with;
        }
    }
    return {};
}

/* Resolve user-configured executable path.  Tries (in order):
 *   1. path as-is (then with platform exe suffix on Windows)
 *   2. for relative paths, prepend a few cwd ancestors ("..", "../..", …)
 *      so launching the editor from build/desktop/.../release still finds
 *      siblings declared with repo-relative paths
 *   3. known CMake preset output dirs for the host platform — keeps the
 *      out-of-the-box defaults working without the user touching anything
 * Returns the first match, or the original string for a useful diagnostic. */
static std::string resolve_executable(const std::string &configured)
{
    if (configured.empty()) return configured;

    std::string hit = try_with_suffix(configured);
    if (!hit.empty()) return hit;

    bool is_absolute = false;
    if (!configured.empty() && (configured[0] == '/' || configured[0] == '\\'))
        is_absolute = true;
    if (configured.size() > 1 && configured[1] == ':')
        is_absolute = true;

    if (!is_absolute) {
        const char *parents[] = {
            ".", "..", "../..", "../../..", "../../../..",
        };
        for (const char *par : parents) {
            std::string p = std::string(par) + "/" + configured;
            hit = try_with_suffix(p);
            if (!hit.empty()) return hit;
        }
    }

    /* Per-platform "well-known build-preset output" candidates so the
     * default path (which targets one specific arch/variant) still finds
     * the binary when the user actually built a different variant. */
    static const char *kCandidates[] = {
#if JCE_PLATFORM_WINDOWS
        "build/desktop/windows-x64/release/caged_kingdom",
        "build/desktop/windows-x64/dist/caged_kingdom",
        "build/desktop/windows-x64/debug/caged_kingdom",
        "build/desktop/windows-arm64/release/caged_kingdom",
#elif JCE_PLATFORM_MACOS
        "build/desktop/macos-arm64/CagedKingdom",
        "build/desktop/macos-x64/CagedKingdom",
#elif JCE_PLATFORM_LINUX
        "build/desktop/linux-x64/CagedKingdom",
        "build/desktop/linux-arm64/CagedKingdom",
#endif
        nullptr,
    };
    const char *parents[] = { ".", "..", "../..", "../../..", "../../../..",
                              nullptr };
    for (int ci = 0; kCandidates[ci]; ++ci) {
        for (int pi = 0; parents[pi]; ++pi) {
            std::string p = std::string(parents[pi]) + "/" + kCandidates[ci];
            hit = try_with_suffix(p);
            if (!hit.empty()) return hit;
        }
    }
    return configured;
}

/* 2-second graceful-stop window before forcing termination. */
constexpr uint64_t STOP_TIMEOUT_MS = 2000;

struct RunManager {
    JceRunState state = JCE_RUN_IDLE;
    int         exit_code = 0;
    std::string last_error;
    uint64_t    stop_deadline_ms = 0;
    bool        force_stop_logged = false;

    JceProcess *process = nullptr;
    bool        capture_stdout = false;
    bool        capture_stderr = false;

    std::string stdout_partial;
    std::string stderr_partial;
};

RunManager g_run;

void copy_cstr(char *dst, size_t dst_size, const std::string &src)
{
    if (!dst || dst_size == 0) return;
    std::snprintf(dst, dst_size, "%s", src.c_str());
}

void log_line(JceConsoleLevel level, const std::string &text)
{
    jce_editor_console_log_level(level, "%s", text.c_str());
}

void set_error(const std::string &text)
{
    g_run.state = JCE_RUN_FAILED;
    g_run.last_error = text;
    log_line(JCE_CONSOLE_ERROR, "[run] " + text);
}

bool is_empty(const char *s) { return !s || s[0] == '\0'; }

void emit_lines(std::string &partial, const char *data, size_t len,
                JceConsoleLevel level)
{
    for (size_t i = 0; i < len; ++i) {
        char c = data[i];
        if (c == '\r') continue;
        if (c == '\n') {
            if (!partial.empty()) {
                log_line(level, "[run] " + partial);
                partial.clear();
            }
        } else {
            partial.push_back(c);
        }
    }
}

void drain_stdout()
{
    if (!g_run.process || !g_run.capture_stdout) return;
    char buf[1024];
    for (;;) {
        size_t got = jce_process_read_stdout(g_run.process, buf, sizeof(buf));
        if (got == 0) break;
        emit_lines(g_run.stdout_partial, buf, got, JCE_CONSOLE_INFO);
    }
}

void drain_stderr()
{
    if (!g_run.process || !g_run.capture_stderr) return;
    char buf[1024];
    for (;;) {
        size_t got = jce_process_read_stderr(g_run.process, buf, sizeof(buf));
        if (got == 0) break;
        emit_lines(g_run.stderr_partial, buf, got, JCE_CONSOLE_ERROR);
    }
}

void flush_partial(std::string &partial, JceConsoleLevel level)
{
    if (!partial.empty()) {
        log_line(level, "[run] " + partial);
        partial.clear();
    }
}

void release_process()
{
    if (g_run.process) {
        jce_process_destroy(g_run.process);
        g_run.process = nullptr;
    }
    g_run.capture_stdout = false;
    g_run.capture_stderr = false;
    flush_partial(g_run.stdout_partial, JCE_CONSOLE_INFO);
    flush_partial(g_run.stderr_partial, JCE_CONSOLE_ERROR);
    g_run.force_stop_logged = false;
}

void poll_process_state()
{
    if (g_run.state != JCE_RUN_RUNNING && g_run.state != JCE_RUN_STOPPING)
        return;

    drain_stdout();
    drain_stderr();

    if (g_run.state == JCE_RUN_STOPPING && g_run.process &&
        jce_time_ticks_ms() >= g_run.stop_deadline_ms) {
        if (!g_run.force_stop_logged) {
            log_line(JCE_CONSOLE_WARNING,
                     "[run] graceful stop timed out; forcing process termination");
            g_run.force_stop_logged = true;
        }
        jce_process_force_kill(g_run.process);
    }

    if (!g_run.process) return;

    int exit_code = 0;
    if (!jce_process_poll_exit(g_run.process, &exit_code))
        return;

    drain_stdout();
    drain_stderr();

    g_run.exit_code = exit_code;
    g_run.state = JCE_RUN_EXITED;
    g_run.last_error.clear();
    release_process();

    if (exit_code == 0)
        log_line(JCE_CONSOLE_INFO, "[run] game exited with code 0");
    else
        log_line(JCE_CONSOLE_ERROR,
                 "[run] game exited with code " + std::to_string(exit_code));
}

void cleanup_running_process()
{
    /* jce_process_destroy force-kills if still running. */
    release_process();
    g_run.state = JCE_RUN_IDLE;
    g_run.last_error.clear();
}

} // namespace

void jce_run_manager_init(void)
{
    g_run.state = JCE_RUN_IDLE;
    g_run.exit_code = 0;
    g_run.last_error.clear();
}

void jce_run_manager_shutdown(void)
{
    poll_process_state();
    cleanup_running_process();
}

void jce_run_manager_poll(void)
{
    poll_process_state();
}

bool jce_run_manager_start(const JceRunConfig *cfg)
{
    poll_process_state();

    if (!cfg) {
        set_error("cannot start game: missing run configuration");
        return false;
    }

    if (is_empty(cfg->executable_path)) {
        set_error("cannot start game: executable path is empty");
        return false;
    }

    if (jce_run_manager_is_running()) {
        log_line(JCE_CONSOLE_WARNING, "[run] game is already running");
        return false;
    }

    release_process();

    std::string resolved_exe = resolve_executable(cfg->executable_path);
    std::string resolved_cwd = cfg->working_directory ? cfg->working_directory : "";
    if (!path_exists(resolved_exe)) {
        std::string msg = std::string("game executable not found: ") +
                          cfg->executable_path;
        if (resolved_exe != cfg->executable_path)
            msg += " (also tried: " + resolved_exe + ")";
        msg += " — set Preferences > Game > Executable Path";
        set_error(msg);
        return false;
    }
    /* If working dir is empty/missing, default to the directory containing
     * the resolved executable so the game can find its assets. */
    if (resolved_cwd.empty() || !path_exists(resolved_cwd)) {
        size_t slash = resolved_exe.find_last_of("/\\");
        if (slash != std::string::npos)
            resolved_cwd = resolved_exe.substr(0, slash);
    }

    JceProcessConfig pcfg{};
    pcfg.executable_path   = resolved_exe.c_str();
    pcfg.working_directory = resolved_cwd.empty() ? nullptr : resolved_cwd.c_str();
    pcfg.arguments         = cfg->arguments;
    pcfg.capture_stdout    = cfg->capture_stdout;
    pcfg.capture_stderr    = cfg->capture_stderr;

    JceProcess *proc = jce_process_spawn(&pcfg);
    if (!proc) {
        set_error(std::string("cannot start game: jce_process_spawn failed: ") +
                  resolved_exe);
        return false;
    }

    g_run.process        = proc;
    g_run.capture_stdout = cfg->capture_stdout;
    g_run.capture_stderr = cfg->capture_stderr;
    g_run.state          = JCE_RUN_RUNNING;
    g_run.exit_code      = 0;
    g_run.last_error.clear();
    g_run.force_stop_logged = false;

    log_line(JCE_CONSOLE_INFO,
             std::string("[run] started external game: ") + resolved_exe +
             (resolved_cwd.empty() ? std::string()
                                   : (std::string(" (cwd: ") + resolved_cwd + ")")));
    return true;
}

void jce_run_manager_request_stop(void)
{
    if (!jce_run_manager_is_running()) {
        log_line(JCE_CONSOLE_WARNING, "[run] no external game process is running");
        return;
    }

    bool ok = g_run.process && jce_process_request_stop(g_run.process);
    g_run.state = JCE_RUN_STOPPING;
    g_run.stop_deadline_ms = jce_time_ticks_ms() + STOP_TIMEOUT_MS;
    g_run.force_stop_logged = false;

    if (ok)
        log_line(JCE_CONSOLE_INFO, "[run] requested graceful game shutdown");
    else
        log_line(JCE_CONSOLE_WARNING,
                 "[run] graceful stop request failed; will force termination after timeout");
}

bool jce_run_manager_is_running(void)
{
    return g_run.state == JCE_RUN_RUNNING || g_run.state == JCE_RUN_STOPPING;
}

JceRunState jce_run_manager_state(void)
{
    return g_run.state;
}

void jce_run_manager_get_status(JceRunStatus *out_status)
{
    if (!out_status) return;
    out_status->state = g_run.state;
    out_status->running = jce_run_manager_is_running();
    out_status->exit_code = g_run.exit_code;
    copy_cstr(out_status->last_error, sizeof(out_status->last_error),
              g_run.last_error);
}
