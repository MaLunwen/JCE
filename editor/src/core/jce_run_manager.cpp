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
#include "jce_editor_panels.h"

extern "C" {
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_timer.h>
}

#include <cstdio>
#include <cstring>
#include <string>

namespace {

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

    JceProcessConfig pcfg{};
    pcfg.executable_path   = cfg->executable_path;
    pcfg.working_directory = cfg->working_directory;
    pcfg.arguments         = cfg->arguments;
    pcfg.capture_stdout    = cfg->capture_stdout;
    pcfg.capture_stderr    = cfg->capture_stderr;

    JceProcess *proc = jce_process_spawn(&pcfg);
    if (!proc) {
        set_error(std::string("cannot start game: jce_process_spawn failed: ") +
                  cfg->executable_path);
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
             std::string("[run] started external game: ") + cfg->executable_path);
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
