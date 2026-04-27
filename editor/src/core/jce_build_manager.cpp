/*
 * jce_build_manager.cpp  CMake-driven build orchestration.
 *
 * Spawns `cmake --preset <p>` (configure) and `cmake --build --preset
 * <p>` (compile) child processes.  Mirrors the structure of
 * jce_run_manager but is intentionally a separate process slot so a
 * build can run while the user is also playing the game.
 *
 * Stop policy: a 5-second graceful window (SIGINT-equivalent) before
 * jce_process_force_kill is invoked.  CMake/ninja flush their child
 * compilers on Ctrl+C so partial outputs are not catastrophic.
 *
 * Threading: all public entry points are called from the editor main
 * thread.  Pipes are drained synchronously inside poll().
 */

#include "jce_build_manager.h"

#include "ui/jce_editor_panels.h"

extern "C" {
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_timer.h>
}

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr int kGracefulStopMs = 5000;

struct Build {
    JceProcess   *process         = nullptr;
    JceBuildState state           = JCE_BUILD_IDLE;
    JceBuildStage stage           = JCE_BUILD_STAGE_NONE;
    int           exit_code       = 0;
    std::string   preset;
    std::string   last_error;
    std::string   stdout_partial;
    std::string   stderr_partial;
    uint64_t      stop_deadline_ms = 0;
    bool          stopping         = false;
    bool          force_logged     = false;
};

Build g_build;

void log_line(JceConsoleLevel level, const std::string &line)
{
    jce_editor_console_log_level(level, "%s", line.c_str());
}

void emit_lines(std::string &partial, const char *data, size_t len,
                JceConsoleLevel level)
{
    for (size_t i = 0; i < len; ++i) {
        char c = data[i];
        if (c == '\r') continue;
        if (c == '\n') {
            if (!partial.empty()) {
                log_line(level, "[build] " + partial);
                partial.clear();
            }
        } else {
            partial.push_back(c);
        }
    }
}

void drain_pipes()
{
    if (!g_build.process) return;
    char buf[1024];
    for (;;) {
        size_t got = jce_process_read_stdout(g_build.process, buf, sizeof(buf));
        if (got == 0) break;
        emit_lines(g_build.stdout_partial, buf, got, JCE_CONSOLE_INFO);
    }
    for (;;) {
        size_t got = jce_process_read_stderr(g_build.process, buf, sizeof(buf));
        if (got == 0) break;
        emit_lines(g_build.stderr_partial, buf, got, JCE_CONSOLE_ERROR);
    }
}

void flush_partial(std::string &partial, JceConsoleLevel level)
{
    if (!partial.empty()) {
        log_line(level, "[build] " + partial);
        partial.clear();
    }
}

void release_process()
{
    if (g_build.process) {
        jce_process_destroy(g_build.process);
        g_build.process = nullptr;
    }
    flush_partial(g_build.stdout_partial, JCE_CONSOLE_INFO);
    flush_partial(g_build.stderr_partial, JCE_CONSOLE_ERROR);
    g_build.stopping     = false;
    g_build.force_logged = false;
}

void set_error(const std::string &msg)
{
    g_build.last_error = msg;
    log_line(JCE_CONSOLE_ERROR, "[build] " + msg);
}

bool spawn_cmake(JceBuildStage stage, const char *preset, const char *args)
{
    if (g_build.process) {
        set_error("a build is already in flight; stop it first");
        return false;
    }
    if (!preset || !preset[0]) {
        set_error("preset name is empty");
        return false;
    }

    JceProcessConfig pcfg{};
    pcfg.executable_path   = "cmake";
    pcfg.working_directory = nullptr; /* inherit editor cwd */
    pcfg.arguments         = args;
    pcfg.capture_stdout    = true;
    pcfg.capture_stderr    = true;

    JceProcess *proc = jce_process_spawn(&pcfg);
    if (!proc) {
        set_error(std::string("failed to spawn cmake (is it on PATH?): ") +
                  args);
        g_build.state = JCE_BUILD_FAILED;
        return false;
    }

    g_build.process     = proc;
    g_build.state       = JCE_BUILD_RUNNING;
    g_build.stage       = stage;
    g_build.exit_code   = 0;
    g_build.preset      = preset;
    g_build.last_error.clear();

    log_line(JCE_CONSOLE_INFO,
             std::string("[build] cmake ") + args);
    return true;
}

void poll_state()
{
    if (g_build.state != JCE_BUILD_RUNNING || !g_build.process) return;

    drain_pipes();

    if (g_build.stopping &&
        jce_time_ticks_ms() >= g_build.stop_deadline_ms) {
        if (!g_build.force_logged) {
            log_line(JCE_CONSOLE_WARNING,
                     "[build] graceful stop timed out; force-killing cmake");
            g_build.force_logged = true;
        }
        jce_process_force_kill(g_build.process);
    }

    int exit_code = 0;
    if (!jce_process_poll_exit(g_build.process, &exit_code)) return;

    drain_pipes();
    g_build.exit_code = exit_code;
    g_build.state     = (exit_code == 0) ? JCE_BUILD_SUCCEEDED
                                         : JCE_BUILD_FAILED;
    if (exit_code != 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "cmake exited with code %d", exit_code);
        g_build.last_error = buf;
    }
    release_process();

    if (exit_code == 0)
        log_line(JCE_CONSOLE_INFO, "[build] cmake finished OK");
    else
        log_line(JCE_CONSOLE_ERROR,
                 std::string("[build] cmake failed (exit ") +
                 std::to_string(exit_code) + ")");
}

} // namespace

void jce_build_manager_init(void)
{
    g_build = Build{};
}

void jce_build_manager_shutdown(void)
{
    if (g_build.process) {
        jce_process_force_kill(g_build.process);
        release_process();
    }
    g_build = Build{};
}

void jce_build_manager_poll(void)
{
    poll_state();
}

bool jce_build_manager_configure(const char *preset)
{
    if (!preset) return false;
    std::string args = std::string("--preset ") + preset;
    return spawn_cmake(JCE_BUILD_STAGE_CONFIGURE, preset, args.c_str());
}

bool jce_build_manager_build(const char *preset)
{
    if (!preset) return false;
    std::string args = std::string("--build --preset ") + preset;
    return spawn_cmake(JCE_BUILD_STAGE_COMPILE, preset, args.c_str());
}

void jce_build_manager_request_stop(void)
{
    if (!g_build.process || g_build.state != JCE_BUILD_RUNNING) return;
    if (g_build.stopping) return;
    jce_process_request_stop(g_build.process);
    g_build.stopping         = true;
    g_build.stop_deadline_ms = jce_time_ticks_ms() + kGracefulStopMs;
    log_line(JCE_CONSOLE_INFO, "[build] requested cmake stop");
}

bool jce_build_manager_is_running(void)
{
    return g_build.state == JCE_BUILD_RUNNING;
}

void jce_build_manager_get_status(JceBuildStatus *out)
{
    if (!out) return;
    out->state     = g_build.state;
    out->stage     = g_build.stage;
    out->exit_code = g_build.exit_code;
    snprintf(out->preset, sizeof(out->preset), "%s", g_build.preset.c_str());
    snprintf(out->last_error, sizeof(out->last_error), "%s",
             g_build.last_error.c_str());
}
