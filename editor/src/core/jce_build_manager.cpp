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
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/platform/jce_host_shell.h>
#include <jce/os/core/jce_filesystem.h>
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kGracefulStopMs   = 5000;
constexpr int kToolProbeTimeoutMs = 2000;

#if defined(_WIN32)
constexpr char PATH_SEP_CHR_LOCAL = '\\';
#else
constexpr char PATH_SEP_CHR_LOCAL = '/';
#endif

/* (Pipeline orchestration removed.  The editor now delegates the full
 * conan→cmake→ninja sequence to scripts/build-*.{bat,sh} via
 * jce_build_manager_run_script() — a single source of truth.) */

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

/* Process-wide fallback working directory.  Used by spawn_tool() when
 * the caller does not supply one — i.e. legacy Configure / Build /
 * Repack paths.  Set by the Build Profiles panel after the user picks
 * a project root. */
std::string g_default_working_dir;

/* Cached tool probe result so we don't re-spawn every frame. */
struct ToolCache {
    bool valid = false;
    JceBuildToolStatus status{};
};
ToolCache g_tools;

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

/* Resolve a bare exe name like "conan" / "cmake" to a full path via the
 * platform PATH search wrapper.  Needed on Windows because SDL3 spawn +
 * a custom working_directory can confuse CreateProcess's implicit PATH
 * lookup.  On POSIX execvp handles it and the wrapper returns false →
 * we leave exe unchanged. */
static bool resolve_exe_on_path(const char *exe, std::string &out)
{
    char buf[1024];
    if (jce_host_resolve_executable(exe, buf, sizeof(buf))) {
        out = buf;
        /* Suppress the trivial "already a path" passthrough so callers
         * can detect "real resolution happened" with strcmp if needed. */
        return out != exe;
    }
    return false;
}

bool spawn_tool(JceBuildStage stage, const char *exe,
                const char *preset_label, const char *args,
                const char *working_dir)
{
    if (g_build.process) {
        set_error("a build is already in flight; stop it first");
        return false;
    }
    if (!exe || !exe[0]) {
        set_error("tool name is empty");
        return false;
    }

    std::string resolved;
    const char *exe_for_spawn = exe;
    if (resolve_exe_on_path(exe, resolved))
        exe_for_spawn = resolved.c_str();

    JceProcessConfig pcfg{};
    pcfg.executable_path   = exe_for_spawn;
    const char *wd = (working_dir && working_dir[0]) ? working_dir
                       : (g_default_working_dir.empty() ? nullptr
                                                       : g_default_working_dir.c_str());
    pcfg.working_directory = wd;
    pcfg.arguments         = args;
    pcfg.capture_stdout    = true;
    pcfg.capture_stderr    = true;

    /* Log the attempt BEFORE spawning so failures don't leave the user
     * guessing what we actually tried. */
    {
        std::string line = "[build] spawn: ";
        line += exe_for_spawn;
        if (args && args[0]) { line += " "; line += args; }
        if (wd) { line += "  (cwd="; line += wd; line += ")"; }
        log_line(JCE_CONSOLE_INFO, line);
    }
    LOG_INFO("build", "spawn_tool: about to jce_process_spawn exe=%s args=%s wd=%s",
             exe_for_spawn, args ? args : "(null)", wd ? wd : "(null)");
    jce_log_flush();

    JceProcess *proc = jce_process_spawn(&pcfg);
    LOG_INFO("build", "spawn_tool: jce_process_spawn returned %p",
             (void*)proc);
    jce_log_flush();
    if (!proc) {
        const char *sdl_err = jce_process_get_last_spawn_error();
        set_error(std::string("failed to spawn ") + exe +
                  " (resolved='" + exe_for_spawn + "'): " +
                  (sdl_err && sdl_err[0] ? sdl_err : "unknown error") +
                  " — args: " + (args ? args : ""));
        g_build.state = JCE_BUILD_FAILED;
        return false;
    }

    g_build.process     = proc;
    g_build.state       = JCE_BUILD_RUNNING;
    g_build.stage       = stage;
    g_build.exit_code   = 0;
    g_build.preset      = preset_label ? preset_label : "";
    g_build.last_error.clear();
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
                     "[build] graceful stop timed out; force-killing tool");
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
        snprintf(buf, sizeof(buf), "tool exited with code %d", exit_code);
        g_build.last_error = buf;
    }
    release_process();

    if (exit_code == 0)
        log_line(JCE_CONSOLE_INFO, "[build] step finished OK");
    else
        log_line(JCE_CONSOLE_ERROR,
                 std::string("[build] step failed (exit ") +
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

bool jce_build_manager_repack_game_assets(const char *preset)
{
    /* Project mode: if an editor project is currently open, dispatch
     * scripts/build-project.bat against its root.  This is the one
     * automatic path that keeps the embedded PAK + cooked tree in sync
     * with designer edits.  When no project is open we currently have
     * no way to drive the in-tree CK build — `cmake --build --preset`
     * is broken by a CMakeUserPresets.json Duplicate-preset bug — so
     * we return false and the caller falls back to launching with the
     * existing artefacts. */
    (void)preset;
    extern char s_current_project_root[512]; /* dialog_project.cpp */
    if (s_current_project_root[0] == '\0')
        return false;

    const char *script = jce_build_manager_default_project_script();
    if (!script || !script[0])
        return false;

    JceBuildScriptConfig cfg{};
    cfg.script_path  = script;
    cfg.working_dir  = nullptr;             /* run from repo root */
    cfg.label        = "build-project";
    /* build-project.bat / .sh take the project directory as the first
     * positional argument.  Quote it so spaces survive the cmd.exe /
     * bash splitting layer in run_script(). */
    std::string args = std::string("\"") + s_current_project_root + "\"";
    cfg.script_args  = args.c_str();
    return jce_build_manager_run_script(&cfg);
}

/* ---------------------------------------------------------------- *
 * Script delegation                                                 *
 * ---------------------------------------------------------------- */

const char *jce_build_manager_default_desktop_script(void)
{
#if defined(_WIN32)
    return "scripts/build-desktop.bat";
#elif defined(__APPLE__)
    return "scripts/macos/build-macos-x64.sh";
#elif defined(__linux__)
    return "scripts/linux/build-linux-x64.sh";
#else
    return NULL;
#endif
}

const char *jce_build_manager_default_project_script(void)
{
#if defined(_WIN32)
    return "scripts/build-project.bat";
#else
    return "scripts/build-project.sh";
#endif
}

bool jce_build_manager_run_script(const JceBuildScriptConfig *cfg)
{
    LOG_INFO("build", "run_script: enter cfg=%p", (const void*)cfg);
    jce_log_flush();
    if (!cfg || !cfg->script_path || !cfg->script_path[0]) {
        set_error("run_script: script_path is required");
        return false;
    }
    if (g_build.process) {
        set_error("run_script: a build is already running");
        return false;
    }

    const char *wd = (cfg->working_dir && cfg->working_dir[0])
                         ? cfg->working_dir
                         : (g_default_working_dir.empty()
                                ? nullptr
                                : g_default_working_dir.c_str());

    /* Build the shell + argv.  We use cmd.exe /c on Windows and
     * /bin/bash on POSIX so the script runs through a known-good
     * interpreter regardless of file extension or chmod bits. */
    std::string shell;
    std::string args;
    /* Normalize separators for the host shell.  cmd.exe in particular
     * treats forward slashes as switch prefixes ("/c"), so a path like
     * "scripts/build-desktop.bat" parses as the unknown command
     * "scripts" + switch "/build-desktop.bat".  Backslashes sidestep
     * the entire ambiguity. */
    std::string script_norm = cfg->script_path;
#if defined(_WIN32)
    for (char &c : script_norm) if (c == '/') c = '\\';
#endif

    /* If the script path is relative AND it isn't reachable from the
     * working directory we'll cd into, try to resolve it relative to
     * the editor executable's directory.  Without this, dispatching
     * `scripts\\build-project.bat` with cwd=<user project> always
     * fails (and historically crashed deeper in the spawn path). */
    auto is_absolute_path = [](const std::string &p) {
#if defined(_WIN32)
        return p.size() >= 2 &&
               ((p[1] == ':') || (p[0] == '\\' && p[1] == '\\') ||
                (p[0] == '/'  && p[1] == '/'));
#else
        return !p.empty() && p[0] == '/';
#endif
    };
    if (!is_absolute_path(script_norm)) {
        /* Probe the candidate relative to cwd-we-will-use. */
        std::string probe_a;
        if (wd && wd[0]) {
            probe_a = wd;
            if (!probe_a.empty() &&
                probe_a.back() != '/' && probe_a.back() != '\\')
                probe_a += PATH_SEP_CHR_LOCAL;
            probe_a += script_norm;
        }
        bool found_in_cwd = !probe_a.empty() &&
                            jce_fs_host_exists_file(probe_a.c_str());
        if (!found_in_cwd) {
            char base[1024];
            if (jce_fs_host_get_base_path(base, sizeof(base))) {
                /* Walk up from <exe_dir> looking for the script;
                 * covers in-tree builds (exe at build/.../release/)
                 * and packaged installs (exe at bin/ next to scripts/). */
                std::string dir = base;
                while (!dir.empty() &&
                       (dir.back() == '/' || dir.back() == '\\'))
                    dir.pop_back();
                for (int i = 0; i < 6 && !dir.empty(); ++i) {
                    std::string cand = dir;
                    cand += PATH_SEP_CHR_LOCAL;
                    cand += script_norm;
                    if (jce_fs_host_exists_file(cand.c_str())) {
                        script_norm = cand;
                        break;
                    }
                    size_t pos = dir.find_last_of("/\\");
                    if (pos == std::string::npos) break;
                    dir.resize(pos);
                }
            }
        }
    }

    if (!is_absolute_path(script_norm) &&
        !(wd && wd[0] && jce_fs_host_exists_file(
            (std::string(wd) + PATH_SEP_CHR_LOCAL + script_norm).c_str())) &&
        !jce_fs_host_exists_file(script_norm.c_str())) {
        set_error(("run_script: script not found: " + script_norm).c_str());
        log_line(JCE_CONSOLE_ERROR,
                 "[build] script not found: " + script_norm +
                 " (cwd=" + (wd ? wd : "(default)") +
                 ").  Make sure scripts/ is reachable from either the "
                 "working directory or the editor install dir.");
        return false;
    }

#if defined(_WIN32)
    shell = "cmd.exe";
    args  = "/c ";
    args += "\"";
    args += script_norm;
    args += "\"";
    if (cfg->script_args && cfg->script_args[0]) {
        args += " ";
        args += cfg->script_args;
    }
#else
    shell = "/bin/bash";
    args  = "\"";
    args += script_norm;
    args += "\"";
    if (cfg->script_args && cfg->script_args[0]) {
        args += " ";
        args += cfg->script_args;
    }
#endif

    const char *label = (cfg->label && cfg->label[0]) ? cfg->label
                                                      : cfg->script_path;
    log_line(JCE_CONSOLE_INFO,
             std::string("[build] script: ") + script_norm +
             (cfg->script_args && cfg->script_args[0]
                  ? std::string(" ") + cfg->script_args : std::string()) +
             (wd ? std::string("  (cwd=") + wd + ")" : std::string()));
    LOG_INFO("build", "run_script: resolved script=%s wd=%s shell=%s",
             script_norm.c_str(), wd ? wd : "(null)", shell.c_str());
    jce_log_flush();

    /* Reuse spawn_tool — it logs the resolved command and surfaces SDL
     * errors uniformly.  Stage is COMPILE so the UI status line shows
     * a sensible label. */
    bool ok = spawn_tool(JCE_BUILD_STAGE_COMPILE, shell.c_str(),
                         label, args.c_str(), wd);
    LOG_INFO("build", "run_script: spawn_tool returned %s",
             ok ? "true" : "false");
    jce_log_flush();
    return ok;
}

void jce_build_manager_set_default_working_dir(const char *dir)
{
    g_default_working_dir = (dir && dir[0]) ? std::string(dir) : std::string();
}

void jce_build_manager_request_stop(void)
{
    if (!g_build.process || g_build.state != JCE_BUILD_RUNNING) return;
    if (g_build.stopping) return;
    jce_process_request_stop(g_build.process);
    g_build.stopping         = true;
    g_build.stop_deadline_ms = jce_time_ticks_ms() + kGracefulStopMs;
    log_line(JCE_CONSOLE_INFO, "[build] requested tool stop");
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

/* ---------------------------------------------------------------- *
 * Tool probe                                                        *
 * ---------------------------------------------------------------- */

namespace {

void copy_first_line(char *dst, size_t dst_cap, const std::string &s)
{
    if (!dst || dst_cap == 0) return;
    size_t i = 0;
    for (; i < s.size() && i + 1 < dst_cap; ++i) {
        char c = s[i];
        if (c == '\n' || c == '\r') break;
        dst[i] = c;
    }
    dst[i] = '\0';
}

bool probe_tool(const char *exe, char *version_out, size_t version_cap)
{
    if (version_out && version_cap) version_out[0] = '\0';

    JceProcessConfig pcfg{};
    pcfg.executable_path = exe;
    pcfg.arguments       = "--version";
    pcfg.capture_stdout  = true;
    pcfg.capture_stderr  = true;

    JceProcess *proc = jce_process_spawn(&pcfg);
    if (!proc) return false;

    std::string out;
    uint64_t deadline = jce_time_ticks_ms() + kToolProbeTimeoutMs;
    int exit_code = 0;
    bool exited   = false;
    while (jce_time_ticks_ms() < deadline) {
        char buf[256];
        for (;;) {
            size_t got = jce_process_read_stdout(proc, buf, sizeof(buf));
            if (got == 0) break;
            out.append(buf, got);
        }
        for (;;) {
            size_t got = jce_process_read_stderr(proc, buf, sizeof(buf));
            if (got == 0) break;
            out.append(buf, got);
        }
        if (jce_process_poll_exit(proc, &exit_code)) { exited = true; break; }
        jce_thread_sleep_ms(10);
    }
    if (!exited) {
        jce_process_force_kill(proc);
        jce_process_destroy(proc);
        return false;
    }
    /* Drain any final bytes. */
    {
        char buf[256];
        for (;;) {
            size_t got = jce_process_read_stdout(proc, buf, sizeof(buf));
            if (got == 0) break;
            out.append(buf, got);
        }
    }
    jce_process_destroy(proc);

    if (exit_code != 0) return false;
    copy_first_line(version_out, version_cap, out);
    return true;
}

} /* namespace */

void jce_build_manager_check_tools(JceBuildToolStatus *out)
{
    if (!out) return;

    /* If a build is in flight we cannot probe (single process slot) —
     * return the cached snapshot, or a zeroed struct if never probed. */
    if (g_build.process) {
        *out = g_tools.valid ? g_tools.status : JceBuildToolStatus{};
        return;
    }

    JceBuildToolStatus s{};
    s.cmake_ok = probe_tool("cmake", s.cmake_version, sizeof(s.cmake_version));
    s.conan_ok = probe_tool("conan", s.conan_version, sizeof(s.conan_version));
    s.ninja_ok = probe_tool("ninja", s.ninja_version, sizeof(s.ninja_version));

    g_tools.status = s;
    g_tools.valid  = true;
    *out = s;

    log_line(JCE_CONSOLE_INFO,
             std::string("[build] tools: cmake=") + (s.cmake_ok ? "OK" : "MISSING") +
             " conan=" + (s.conan_ok ? "OK" : "MISSING") +
             " ninja=" + (s.ninja_ok ? "OK" : "MISSING"));
}
