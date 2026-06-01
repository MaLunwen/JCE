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

#include "jce_editor_project.h"
#include "ui/jce_editor_panels.h"

extern "C" {
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_toolchain.h>
#include <jce/os/platform/jce_host_shell.h>
#include <jce/os/core/jce_filesystem.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kGracefulStopMs   = 5000;
constexpr int kToolProbeTimeoutMs = 2000;

#if JCE_PLATFORM_WINDOWS
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

/* ---------------------------------------------------------------- *
 * Native project-build pipeline state (start_project_build).         *
 * A small command queue lets one logical build run as several child  *
 * processes (e.g. POSIX configure then compile) through the existing *
 * single-process poll loop.  When the queue drains, an optional       *
 * "finish" plan verifies the artifact and stages a package.          *
 * ---------------------------------------------------------------- */
struct QueuedStep {
    JceBuildStage stage = JCE_BUILD_STAGE_COMPILE;
    std::string   exe;
    std::string   args;
    std::string   wd;
    std::string   label;
};
std::vector<QueuedStep> g_queue;   /* steps after the one in flight */
size_t                  g_queue_pos = 0;

struct FinishPlan {
    bool        verify = false;    /* confirm the artifact exists */
    std::string artifact_a;        /* <build>/<exe> */
    std::string artifact_b;        /* <build>/<config>/<exe> fallback */
    std::string exe_name;
    bool        stage = false;     /* package staging (replaces package-game) */
    std::string out_dir;
    std::string cooked_src;        /* <project>/<cooked_rel> */
    std::string cooked_rel;        /* relative, for the staged layout */
    std::string version_text;      /* full VERSION.txt body */
};
FinishPlan g_finish;

void reset_pipeline()
{
    g_queue.clear();
    g_queue_pos = 0;
    g_finish = FinishPlan{};
}

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

/* Resolve which artifact candidate actually exists (a preferred, b
 * fallback).  Returns empty when neither is present. */
std::string resolve_artifact()
{
    if (!g_finish.artifact_a.empty() &&
        jce_fs_host_exists_file(g_finish.artifact_a.c_str()))
        return g_finish.artifact_a;
    if (!g_finish.artifact_b.empty() &&
        jce_fs_host_exists_file(g_finish.artifact_b.c_str()))
        return g_finish.artifact_b;
    return std::string();
}

/* Post-pipeline finish: verify the build artifact and, when requested,
 * stage a redistributable package directory (exe + cooked assets +
 * VERSION.txt).  Runs natively via jce_fs — no scripts.  On any hard
 * failure it flips g_build to FAILED with a descriptive error. */
void run_finish_plan()
{
    std::string exe = resolve_artifact();

    if (g_finish.verify) {
        if (exe.empty()) {
            g_build.state = JCE_BUILD_FAILED;
            set_error("build succeeded but artifact missing: " +
                      g_finish.exe_name);
            return;
        }
        log_line(JCE_CONSOLE_INFO, "[build] artifact: " + exe);
    }

    if (!g_finish.stage)
        return;

    if (exe.empty()) {
        g_build.state = JCE_BUILD_FAILED;
        set_error("package: built artifact not found for staging");
        return;
    }

    const std::string &out = g_finish.out_dir;
    if (jce_fs_host_exists_dir(out.c_str()))
        jce_fs_host_remove_recursive(out.c_str());
    if (!jce_fs_host_create_directory(out.c_str())) {
        g_build.state = JCE_BUILD_FAILED;
        set_error("package: cannot create output dir: " + out);
        return;
    }

    std::string dst_exe = out + PATH_SEP_CHR_LOCAL + g_finish.exe_name;
    if (!jce_fs_host_copy_file(exe.c_str(), dst_exe.c_str())) {
        g_build.state = JCE_BUILD_FAILED;
        set_error("package: failed to copy exe to " + dst_exe);
        return;
    }

    /* Stage cooked assets so the packaged game has its PhysFS mount
     * root alongside the exe (mirrors package-game.bat). */
    if (!g_finish.cooked_rel.empty() && !g_finish.cooked_src.empty() &&
        jce_fs_host_exists_dir(g_finish.cooked_src.c_str())) {
        std::string cooked_dst = out + PATH_SEP_CHR_LOCAL + g_finish.cooked_rel;
        if (!jce_fs_host_copy_recursive(g_finish.cooked_src.c_str(),
                                        cooked_dst.c_str())) {
            log_line(JCE_CONSOLE_WARNING,
                     "[build] package: failed to stage cooked assets from " +
                     g_finish.cooked_src);
        }
    } else {
        log_line(JCE_CONSOLE_WARNING,
                 "[build] package: cooked assets dir missing (packaged game "
                 "may have no content): " + g_finish.cooked_src);
    }

    if (!g_finish.version_text.empty()) {
        std::string ver_path = out + PATH_SEP_CHR_LOCAL + "VERSION.txt";
        jce_fs_host_write_all(ver_path.c_str(),
                              g_finish.version_text.data(),
                              g_finish.version_text.size());
    }

    log_line(JCE_CONSOLE_INFO, "[build] package staged at: " + out);
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

    /* Pipeline advance: if this step succeeded and more steps are
     * queued (and we are not stopping), launch the next one and stay
     * RUNNING.  This drives multi-process native builds (POSIX
     * configure -> compile) through the same poll loop. */
    if (exit_code == 0 && !g_build.stopping && g_queue_pos < g_queue.size()) {
        log_line(JCE_CONSOLE_INFO, "[build] step finished OK");
        QueuedStep step = g_queue[g_queue_pos++];
        release_process();   /* frees handle, keeps state RUNNING */
        if (!spawn_tool(step.stage, step.exe.c_str(), step.label.c_str(),
                        step.args.empty() ? nullptr : step.args.c_str(),
                        step.wd.empty() ? nullptr : step.wd.c_str())) {
            /* spawn_tool already set FAILED + last_error. */
            reset_pipeline();
        }
        return;
    }

    g_build.state = (exit_code == 0) ? JCE_BUILD_SUCCEEDED
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

    /* Finish plan: artifact verification + optional package staging.
     * Only runs on a fully successful pipeline. */
    if (g_build.state == JCE_BUILD_SUCCEEDED &&
        (g_finish.verify || g_finish.stage)) {
        run_finish_plan();
    }
    reset_pipeline();
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
    /* Project mode: if an editor project is currently open, rebuild it
     * natively (no scripts) so the embedded PAK + cooked tree stay in
     * sync with designer edits.  Requires a resolvable SDK (manifest
     * sdk_path or JCE_SDK_DIR).  When no project / SDK is available we
     * return false and the caller falls back to launching with the
     * existing artefacts. */
    (void)preset;
    extern char s_current_project_root[512]; /* dialog_project.cpp */
    if (s_current_project_root[0] == '\0')
        return false;

    const JceProject *jp = jce_editor_project_get();
    if (!jp || jce_editor_project_is_engine_workspace())
        return false;

    const char *sdk = (jp->sdk_path && jp->sdk_path[0]) ? jp->sdk_path
                                                        : nullptr;
    if (!sdk) {
        const char *env_sdk = std::getenv("JCE_SDK_DIR");
        if (env_sdk && env_sdk[0]) sdk = env_sdk;
    }
    if (!sdk || !sdk[0])
        return false;

    std::string bundles_joined;
    if (jp->bundles && jp->bundles_count > 0) {
        for (int i = 0; i < jp->bundles_count; ++i) {
            if (!jp->bundles[i] || !jp->bundles[i][0]) continue;
            if (!bundles_joined.empty()) bundles_joined += ";";
            bundles_joined += jp->bundles[i];
        }
    }

    JceBuildProjectConfig pcfg{};
    pcfg.label         = "repack";
    pcfg.project_dir   = (s_current_project_root[0] ? s_current_project_root
                                                    : jp->project_root);
    pcfg.sdk_dir       = sdk;
    pcfg.target        = (jp->target_name && jp->target_name[0])
                             ? jp->target_name : jp->name;
    pcfg.exe_name      = (jp->output_exe && jp->output_exe[0])
                             ? jp->output_exe : nullptr;
    pcfg.variant       = "release";
    pcfg.arch          = nullptr;   /* host default */
    pcfg.cooked_assets = (jp->cooked_assets && jp->cooked_assets[0])
                             ? jp->cooked_assets : nullptr;
    pcfg.bundles       = bundles_joined.empty() ? nullptr
                                                : bundles_joined.c_str();
    pcfg.clean         = false;
    pcfg.package_out_dir = nullptr;
    pcfg.app_name      = jp->name;
    pcfg.app_version   = jp->version;
    return jce_build_manager_start_project_build(&pcfg);
}

/* ---------------------------------------------------------------- *
 * Script delegation                                                 *
 * ---------------------------------------------------------------- */

const char *jce_build_manager_default_desktop_script(void)
{
#if JCE_PLATFORM_WINDOWS
    return "scripts/build-desktop.bat";
#elif JCE_PLATFORM_APPLE
    return "scripts/macos/build-macos-x64.sh";
#elif JCE_PLATFORM_LINUX
    return "scripts/linux/build-linux-x64.sh";
#else
    return NULL;
#endif
}

const char *jce_build_manager_default_project_script(void)
{
#if JCE_PLATFORM_WINDOWS
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
    reset_pipeline();   /* drop any stale native-pipeline queue/finish plan */

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
#if JCE_PLATFORM_WINDOWS
    for (char &c : script_norm) if (c == '/') c = '\\';
#endif

    /* If the script path is relative AND it isn't reachable from the
     * working directory we'll cd into, try to resolve it relative to
     * the editor executable's directory.  Without this, dispatching
     * `scripts\\build-project.bat` with cwd=<user project> always
     * fails (and historically crashed deeper in the spawn path). */
    auto is_absolute_path = [](const std::string &p) {
#if JCE_PLATFORM_WINDOWS
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

#if JCE_PLATFORM_WINDOWS
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

/* ---------------------------------------------------------------- *
 * Native project build (single-executable path — no scripts)        *
 * ---------------------------------------------------------------- */

namespace {

/* Wrap a token in ONE outer quote pair when it contains whitespace.
 * jce_process's split_args consumes the outer quotes and keeps the
 * inner bytes as a single argv token (even with embedded spaces); SDL
 * then re-quotes space-bearing tokens when it rebuilds the Windows
 * command line, so cmd.exe / cmake receive each path intact.  Tokens
 * without spaces are emitted bare so cmd operators like "&&" stay
 * operators rather than literals. */
std::string qtok(const std::string &s)
{
    bool has_ws = false;
    for (char c : s)
        if (c == ' ' || c == '\t') { has_ws = true; break; }
    if (!has_ws) return s;
    return "\"" + s + "\"";
}

std::string join_norm_sep(const std::string &in)
{
    std::string out = in;
#if JCE_PLATFORM_WINDOWS
    for (char &c : out) if (c == '/') c = '\\';
#endif
    return out;
}

} // namespace

bool jce_build_manager_start_project_build(const JceBuildProjectConfig *cfg)
{
    if (!cfg) { set_error("start_project_build: cfg is null"); return false; }
    if (g_build.process) {
        set_error("start_project_build: a build is already running");
        return false;
    }
    if (!cfg->project_dir || !cfg->project_dir[0]) {
        set_error("start_project_build: project_dir is required");
        return false;
    }
    if (!cfg->target || !cfg->target[0]) {
        set_error("start_project_build: target is required");
        return false;
    }
    if (!cfg->sdk_dir || !cfg->sdk_dir[0]) {
        set_error("start_project_build: sdk_dir is required");
        return false;
    }

    reset_pipeline();

    const std::string project = join_norm_sep(cfg->project_dir);
    const std::string sdk     = join_norm_sep(cfg->sdk_dir);
    const std::string target  = cfg->target;
    const std::string variant = (cfg->variant && cfg->variant[0])
                                    ? std::string(cfg->variant) : "release";
    const std::string arch    = (cfg->arch && cfg->arch[0])
                                    ? std::string(cfg->arch) : "x86_64";
    const std::string label   = (cfg->label && cfg->label[0])
                                    ? std::string(cfg->label) : "build-project";

#if JCE_PLATFORM_WINDOWS
    const char *exe_default_suffix = ".exe";
    const char *platform_tag       = "win32";
#elif JCE_PLATFORM_MACOS
    const char *exe_default_suffix = "";
    const char *platform_tag       = "darwin";
#else
    const char *exe_default_suffix = "";
    const char *platform_tag       = "linux";
#endif
    const std::string exe_name = (cfg->exe_name && cfg->exe_name[0])
                                     ? std::string(cfg->exe_name)
                                     : (target + exe_default_suffix);

    /* Resolve the SDK's CMake package dir: prefer the installed
     * <sdk>/lib/cmake/JCE layout, fall back to <sdk>/cmake. */
    std::string cmake_dir;
    {
        const std::string a = sdk + PATH_SEP_CHR_LOCAL + "lib" +
                              PATH_SEP_CHR_LOCAL + "cmake" +
                              PATH_SEP_CHR_LOCAL + "JCE";
        const std::string b = sdk + PATH_SEP_CHR_LOCAL + "cmake";
        if (jce_fs_host_exists_file(
                (a + PATH_SEP_CHR_LOCAL + "JCEConfig.cmake").c_str()) ||
            jce_fs_host_exists_dir(a.c_str()))
            cmake_dir = a;
        else
            cmake_dir = b;
    }

    const std::string build_dir = project + PATH_SEP_CHR_LOCAL + "build" +
                                  PATH_SEP_CHR_LOCAL +
                                  std::string(platform_tag) + "-" + arch +
                                  "-" + variant;
    const std::string build_type =
        (variant == "debug") ? std::string("Debug") : std::string("Release");

    if (cfg->clean && jce_fs_host_exists_dir(build_dir.c_str())) {
        log_line(JCE_CONSOLE_INFO, "[build] clean: removing " + build_dir);
        jce_fs_host_remove_recursive(build_dir.c_str());
    }

    const std::string cooked = (cfg->cooked_assets && cfg->cooked_assets[0])
                                   ? std::string(cfg->cooked_assets) : "";
    const std::string bundles = (cfg->bundles && cfg->bundles[0])
                                    ? std::string(cfg->bundles) : "";

    /* Shared configure flags (identical across platforms). */
    auto append_configure = [&](std::string &a) {
        a += " -S " + qtok(project);
        a += " -B " + qtok(build_dir);
        a += " -G Ninja";
        a += " -DCMAKE_BUILD_TYPE=" + build_type;
        a += " -DJCE_PROJECT_COOKED_ASSETS=" + qtok(cooked);
        a += " -DJCE_PROJECT_BUNDLES=" + qtok(bundles);
        a += " -DJCE_DIR=" + qtok(cmake_dir);
    };

#if JCE_PLATFORM_WINDOWS
    /* The Windows SDK ships MSVC-built static libs, so force cl and
     * activate the MSVC environment inline via Microsoft's own
     * vcvarsall.bat (NOT a first-party script).  Both configure and
     * build run in one cmd.exe /c chain so the env survives between
     * them. */
    jce_toolchain_refresh();
    const JceToolchain *msvc = jce_toolchain_get(JCE_TOOLCHAIN_MSVC);
    std::string vcvars;
    if (msvc && msvc->present && msvc->path[0]) {
        std::string cand = std::string(msvc->path) +
                           "\\VC\\Auxiliary\\Build\\vcvarsall.bat";
        if (jce_fs_host_exists_file(cand.c_str()))
            vcvars = cand;
    }
    std::string vc_arch = "x64";
    if (arch == "i686")    vc_arch = "x64_x86";
    if (arch == "aarch64") vc_arch = "x64_arm64";

    std::string args = "/c ";
    if (!vcvars.empty()) {
        args += "call " + qtok(vcvars) + " " + vc_arch + " && ";
    } else {
        log_line(JCE_CONSOLE_WARNING,
                 "[build] MSVC vcvarsall.bat not found; assuming cl.exe is "
                 "already on PATH");
    }
    args += "cmake";
    append_configure(args);
    args += " -DCMAKE_C_COMPILER=cl";
    args += " && cmake --build " + qtok(build_dir) +
            " --target " + qtok(target);

    QueuedStep step;
    step.stage = JCE_BUILD_STAGE_COMPILE;
    step.exe   = "cmd.exe";
    step.args  = args;
    step.wd    = project;
    step.label = label;
    g_queue.push_back(step);
#else
    /* POSIX: two direct cmake invocations (configure, then build).
     * cmake picks the default system compiler. */
    {
        QueuedStep configure;
        configure.stage = JCE_BUILD_STAGE_CONFIGURE;
        configure.exe   = "cmake";
        std::string a;
        append_configure(a);
        /* Leading space from append_configure is harmless to split. */
        configure.args  = a;
        configure.wd    = project;
        configure.label = label;
        g_queue.push_back(configure);

        QueuedStep compile;
        compile.stage = JCE_BUILD_STAGE_COMPILE;
        compile.exe   = "cmake";
        compile.args  = "--build " + qtok(build_dir) +
                        " --target " + qtok(target);
        compile.wd    = project;
        compile.label = label;
        g_queue.push_back(compile);
    }
#endif

    /* Finish plan: always verify the artifact; stage a package when an
     * output directory was requested. */
    g_finish.verify     = true;
    g_finish.exe_name   = exe_name;
    g_finish.artifact_a = build_dir + PATH_SEP_CHR_LOCAL + exe_name;
    g_finish.artifact_b = build_dir + PATH_SEP_CHR_LOCAL + build_type +
                          PATH_SEP_CHR_LOCAL + exe_name;

    if (cfg->package_out_dir && cfg->package_out_dir[0]) {
        g_finish.stage      = true;
        g_finish.out_dir    = join_norm_sep(cfg->package_out_dir);
        g_finish.cooked_rel = cooked;
        if (!cooked.empty())
            g_finish.cooked_src = project + PATH_SEP_CHR_LOCAL +
                                  join_norm_sep(cooked);

        std::string vt;
        vt += "name:     ";
        vt += (cfg->app_name && cfg->app_name[0]) ? cfg->app_name
                                                  : target.c_str();
        vt += "\n";
        if (cfg->app_version && cfg->app_version[0]) {
            vt += "version:  "; vt += cfg->app_version; vt += "\n";
        }
        vt += "platform: "; vt += platform_tag; vt += "\n";
        vt += "arch:     "; vt += arch;         vt += "\n";
        vt += "variant:  "; vt += variant;      vt += "\n";
        g_finish.version_text = vt;
    }

    log_line(JCE_CONSOLE_INFO,
             "[build] native project build: " + target +
             " (" + std::string(platform_tag) + "/" + arch + "/" + variant +
             ")  sdk=" + sdk);

    /* Spawn the first queued step; poll_state() advances the rest. */
    QueuedStep first = g_queue.front();
    g_queue_pos = 1;
    if (!spawn_tool(first.stage, first.exe.c_str(), first.label.c_str(),
                    first.args.empty() ? nullptr : first.args.c_str(),
                    first.wd.empty() ? nullptr : first.wd.c_str())) {
        reset_pipeline();
        return false;
    }
    return true;
}
