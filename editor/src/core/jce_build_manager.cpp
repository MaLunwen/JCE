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
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_toolchain.h>
#include <jce/os/platform/jce_host_shell.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_archive_cook.h>
}

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kGracefulStopMs   = 5000;
constexpr int kToolProbeTimeoutMs = 2000;

#if JCE_PLATFORM_WINDOWS
constexpr char PATH_SEP_CHR_LOCAL = '\\';
#else
constexpr char PATH_SEP_CHR_LOCAL = '/';
#endif

/* The editor owns project builds directly: asset cook/pack/BOM generation
 * runs in-process, then CMake/Ninja/MSVC are driven through jce_process.
 * Engine-workspace builds may still use named CMake presets. */

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
    std::string asset_bom_src;     /* generated project asset BOM */
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

    if (!g_finish.asset_bom_src.empty() &&
        jce_fs_host_exists_file(g_finish.asset_bom_src.c_str())) {
        std::string bom_dst = out + PATH_SEP_CHR_LOCAL + "asset_bom.json";
        if (!jce_fs_host_copy_file(g_finish.asset_bom_src.c_str(),
                                   bom_dst.c_str())) {
            log_line(JCE_CONSOLE_WARNING,
                     "[build] package: failed to stage asset BOM from " +
                     g_finish.asset_bom_src);
        }
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

std::string slash_norm(std::string in)
{
    for (char &c : in)
        if (c == '\\') c = '/';
    return in;
}

bool path_has_segment(const std::string &path, const char *segment)
{
    if (!segment || !segment[0])
        return false;
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t next = path.find('/', pos);
        size_t len = (next == std::string::npos) ? path.size() - pos
                                                 : next - pos;
        if (len == std::strlen(segment) &&
            path.compare(pos, len, segment) == 0) {
            return true;
        }
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return false;
}

bool path_has_hidden_segment(const std::string &path)
{
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t next = path.find('/', pos);
        size_t len = (next == std::string::npos) ? path.size() - pos
                                                 : next - pos;
        if (len > 0 && path[pos] == '.')
            return true;
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return false;
}

std::string make_relative_vpath(const std::string &abs_path,
                                const std::string &root)
{
    std::string a = slash_norm(abs_path);
    std::string r = slash_norm(root);
    while (!r.empty() && r.back() == '/')
        r.pop_back();
    if (!r.empty() && a.size() > r.size() &&
        a.compare(0, r.size(), r) == 0 &&
        (a[r.size()] == '/' || a[r.size()] == '\\')) {
        return a.substr(r.size() + 1);
    }
    size_t slash = a.find_last_of('/');
    return slash == std::string::npos ? a : a.substr(slash + 1);
}

std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char ch : s) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (ch < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)ch);
                    out += buf;
                } else {
                    out.push_back((char)ch);
                }
                break;
        }
    }
    return out;
}

const char *archive_comp_name(uint8_t c)
{
    switch (c) {
        case JCE_ARCHIVE_COMP_NONE:      return "NONE";
        case JCE_ARCHIVE_COMP_ZSTD:      return "ZSTD";
        case JCE_ARCHIVE_COMP_ZSTD_DICT: return "ZSTD_DICT";
        case JCE_ARCHIVE_COMP_LZ4:       return "LZ4";
        default:                         return "UNKNOWN";
    }
}

std::string fourcc_string(uint32_t tag)
{
    char s[5];
    s[0] = (char)(tag & 0xffu);
    s[1] = (char)((tag >> 8) & 0xffu);
    s[2] = (char)((tag >> 16) & 0xffu);
    s[3] = (char)((tag >> 24) & 0xffu);
    s[4] = '\0';
    for (int i = 0; i < 4; ++i) {
        if (s[i] < 0x20 || s[i] > 0x7e)
            s[i] = '?';
    }
    return std::string(s);
}

struct ProjectAssetInput {
    std::string          abs_path;
    std::string          vpath;
    std::vector<uint8_t> bytes;
};

struct CollectAssetCtx {
    std::string root;
    std::vector<ProjectAssetInput> *items = nullptr;
    std::string error;
};

bool collect_asset_walk_cb(const char *path, bool is_dir, void *user)
{
    CollectAssetCtx *ctx = (CollectAssetCtx *)user;
    if (!ctx || !ctx->items || !path)
        return false;
    if (is_dir)
        return true;

    std::string rel = make_relative_vpath(path, ctx->root);
    if (rel.empty() || path_has_segment(rel, "raw_assets") ||
        path_has_hidden_segment(rel)) {
        return true;
    }

    uint64_t sz = 0;
    void *raw = jce_fs_host_read_all(path, &sz);
    if (!raw) {
        ctx->error = "cannot read asset: ";
        ctx->error += path;
        return false;
    }

    ProjectAssetInput item;
    item.abs_path = path;
    item.vpath = rel;
    item.bytes.resize((size_t)sz);
    if (sz)
        std::memcpy(item.bytes.data(), raw, (size_t)sz);
    jce_fs_buffer_free(raw);
    ctx->items->push_back(std::move(item));
    return true;
}

bool collect_assets_from_dir(const std::string &root,
                             std::vector<ProjectAssetInput> &items,
                             std::string &error)
{
    if (root.empty() || !jce_fs_host_exists_dir(root.c_str()))
        return true;

    CollectAssetCtx ctx;
    ctx.root = root;
    ctx.items = &items;
    if (!jce_fs_host_walk(root.c_str(), collect_asset_walk_cb, &ctx)) {
        error = ctx.error.empty() ? ("cannot walk asset dir: " + root)
                                  : ctx.error;
        return false;
    }
    return true;
}

bool write_binary_c_source(const std::string &path, const std::string &symbol,
                           const uint8_t *data, size_t size)
{
    std::string head;
    if (!data || size == 0) {
        head = "#include <stddef.h>\n"
               "const unsigned char " + symbol + "[1] = {0};\n"
               "const size_t " + symbol + "_size = 0;\n";
        return jce_fs_host_write_all(path.c_str(), head.data(),
                                     (uint64_t)head.size());
    }

    head = "/* Auto-generated by JCE Editor -- DO NOT EDIT */\n"
           "#include <stddef.h>\n"
           "const unsigned char " + symbol + "[] = {\n";
    if (!jce_fs_host_write_all(path.c_str(), head.data(),
                               (uint64_t)head.size())) {
        return false;
    }

    std::string chunk;
    chunk.reserve(65536);
    for (size_t i = 0; i < size; ++i) {
        char cell[8];
        if (i % 12 == 0)
            chunk += "    ";
        std::snprintf(cell, sizeof(cell), "0x%02x", (unsigned)data[i]);
        chunk += cell;
        chunk += (i + 1 == size) ? "\n" : ", ";
        if (i % 12 == 11)
            chunk += "\n";
        if (chunk.size() > 60000) {
            if (!jce_fs_host_append(path.c_str(), chunk.data(),
                                    (uint64_t)chunk.size())) {
                return false;
            }
            chunk.clear();
        }
    }
    chunk += "};\nconst size_t " + symbol + "_size = ";
    chunk += std::to_string(size);
    chunk += ";\n";
    return jce_fs_host_append(path.c_str(), chunk.data(),
                              (uint64_t)chunk.size());
}

void byte_push(std::vector<uint8_t> &out, uint8_t v)
{
    out.push_back(v);
}

void byte_append(std::vector<uint8_t> &out, const void *data, size_t size)
{
    if (!data || size == 0)
        return;
    const uint8_t *p = (const uint8_t *)data;
    out.insert(out.end(), p, p + size);
}

void byte_le16(std::vector<uint8_t> &out, uint16_t v)
{
    byte_push(out, (uint8_t)v);
    byte_push(out, (uint8_t)(v >> 8));
}

void byte_le32(std::vector<uint8_t> &out, uint32_t v)
{
    byte_push(out, (uint8_t)v);
    byte_push(out, (uint8_t)(v >> 8));
    byte_push(out, (uint8_t)(v >> 16));
    byte_push(out, (uint8_t)(v >> 24));
}

uint16_t coff_machine_from_arch(const std::string &arch)
{
    if (arch == "x64" || arch == "x86_64" || arch == "amd64")
        return 0x8664u;
    if (arch == "x86" || arch == "i686")
        return 0x014cu;
    if (arch == "arm64" || arch == "aarch64")
        return 0xaa64u;
    if (arch == "arm")
        return 0x01c4u;
    return 0;
}

int coff_pointer_size(uint16_t machine)
{
    switch (machine) {
        case 0x8664u:
        case 0xaa64u:
            return 8;
        default:
            return 4;
    }
}

bool write_binary_coff_object(const std::string &path,
                              const std::string &symbol,
                              const uint8_t *data,
                              size_t size,
                              const std::string &arch)
{
    uint16_t machine = coff_machine_from_arch(arch);
    if (!machine || (size && !data))
        return false;

    const int ptr_size = coff_pointer_size(machine);
    const uint64_t size_offset =
        ((uint64_t)size + (uint64_t)ptr_size - 1u) &
        ~((uint64_t)ptr_size - 1u);
    const uint64_t rdata_size = size_offset + (uint64_t)ptr_size;
    const uint64_t rdata_aligned = (rdata_size + 3u) & ~(uint64_t)3;

    const std::string sym_data = symbol;
    const std::string sym_size = symbol + "_size";
    const uint32_t strtab_off_data = 4;
    const uint32_t strtab_off_size =
        strtab_off_data + (uint32_t)sym_data.size() + 1u;
    const uint32_t strtab_total =
        strtab_off_size + (uint32_t)sym_size.size() + 1u;

    const uint32_t coff_header_size = 20;
    const uint32_t section_hdr_size = 40;
    const uint32_t section_data_off = coff_header_size + section_hdr_size;
    if (rdata_aligned > 0xffffffffull)
        return false;
    const uint64_t symtab_off64 = section_data_off + rdata_aligned;
    if (symtab_off64 > 0xffffffffull || rdata_size > 0xffffffffull)
        return false;
    const uint32_t symtab_off = (uint32_t)symtab_off64;

    std::vector<uint8_t> obj;
    obj.reserve((size_t)symtab_off + 36u + strtab_total);

    byte_le16(obj, machine);
    byte_le16(obj, 1);
    byte_le32(obj, 0);
    byte_le32(obj, symtab_off);
    byte_le32(obj, 2);
    byte_le16(obj, 0);
    byte_le16(obj, 0);

    const char sec_name[8] = {'.', 'r', 'd', 'a', 't', 'a', 0, 0};
    byte_append(obj, sec_name, sizeof(sec_name));
    byte_le32(obj, 0);
    byte_le32(obj, 0);
    byte_le32(obj, (uint32_t)rdata_size);
    byte_le32(obj, section_data_off);
    byte_le32(obj, 0);
    byte_le32(obj, 0);
    byte_le16(obj, 0);
    byte_le16(obj, 0);
    byte_le32(obj, 0x40500040u);

    byte_append(obj, data, size);
    while (obj.size() < (size_t)(section_data_off + size_offset))
        byte_push(obj, 0);
    for (int i = 0; i < ptr_size; ++i)
        byte_push(obj, (uint8_t)((uint64_t)size >> (i * 8)));
    while (obj.size() < (size_t)symtab_off)
        byte_push(obj, 0);

    byte_le32(obj, 0);
    byte_le32(obj, strtab_off_data);
    byte_le32(obj, 0);
    byte_le16(obj, 1);
    byte_le16(obj, 0);
    byte_push(obj, 2);
    byte_push(obj, 0);

    byte_le32(obj, 0);
    byte_le32(obj, strtab_off_size);
    byte_le32(obj, (uint32_t)size_offset);
    byte_le16(obj, 1);
    byte_le16(obj, 0);
    byte_push(obj, 2);
    byte_push(obj, 0);

    byte_le32(obj, strtab_total);
    byte_append(obj, sym_data.c_str(), sym_data.size() + 1u);
    byte_append(obj, sym_size.c_str(), sym_size.size() + 1u);

    return jce_fs_host_write_all(path.c_str(), obj.data(),
                                 (uint64_t)obj.size());
}

std::string asm_escape_path(std::string path)
{
    path = slash_norm(path);
    std::string out;
    out.reserve(path.size() + 8);
    for (char c : path) {
        if (c == '\\' || c == '"')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

bool write_binary_asm_incbin(const std::string &path,
                             const std::string &symbol,
                             const std::string &input_path,
                             size_t size)
{
    std::string text;
    text += "/* Auto-generated by JCE Editor -- DO NOT EDIT */\n";
    text += ".section .rodata\n";
    text += ".global " + symbol + "\n";
    text += ".global " + symbol + "_size\n";
    text += ".p2align 4\n";
    text += symbol + ":\n";
    text += "    .incbin \"" + asm_escape_path(input_path) + "\"\n";
    text += ".p2align 3\n";
    text += symbol + "_size:\n";
    text += "    .quad " + std::to_string(size) + "\n";
    return jce_fs_host_write_all(path.c_str(), text.data(),
                                 (uint64_t)text.size());
}

bool write_asset_bom_json(const std::string &path, const std::string &pak_path,
                          const void *pak_blob, size_t pak_size)
{
    JceArchive *ar = jce_archive_open(pak_blob, pak_size);
    if (!ar)
        return false;

    uint32_t count = jce_archive_count(ar);
    uint64_t total_orig = 0;
    uint64_t total_stored = 0;
    uint32_t stored_count = 0;
    uint32_t compressed_count = 0;
    uint32_t encrypted_count = 0;

    for (uint32_t i = 0; i < count; ++i) {
        const JceArchiveEntry *e = jce_archive_get(ar, i);
        if (!e)
            continue;
        total_orig += e->original_size;
        total_stored += e->stored_size;
        if (e->compression == JCE_ARCHIVE_COMP_NONE)
            ++stored_count;
        else
            ++compressed_count;
        if (e->entry_flags & JCE_ARCHIVE_ENTRY_ENCRYPTED)
            ++encrypted_count;
    }

    std::string json;
    json.reserve(2048 + (size_t)count * 280);
    json += "{\n  \"schema\": \"jce.pakbom.v1\",\n";
    json += "  \"file\": \"" + json_escape(pak_path) + "\",\n";
    json += "  \"file_size\": " + std::to_string(pak_size) + ",\n";
    json += "  \"header\": {\n";
    json += "    \"magic\": \"JPAK\",\n";
    json += "    \"format_version\": 1,\n";
    json += "    \"entry_count\": " + std::to_string(count) + ",\n";
    json += "    \"dict_count\": " +
            std::to_string((unsigned)jce_archive_dict_count(ar)) + ",\n";
    json += "    \"header_verified\": ";
    json += jce_archive_verify_header(ar) ? "true\n" : "false\n";
    json += "  },\n";
    uint16_t dict_count = jce_archive_dict_count(ar);
    json += "  \"dictionaries\": [";
    for (uint16_t d = 0; d < dict_count; ++d) {
        json += d ? ",\n    {" : "\n    {";
        json += "\"id\": " + std::to_string((unsigned)d);
        json += ", \"tag\": \"";
        json += json_escape(fourcc_string(jce_archive_dict_tag(ar, d)));
        json += "\"}";
    }
    json += dict_count ? "\n  ],\n" : "],\n";

    double ratio = total_orig ? (double)total_stored / (double)total_orig : 0.0;
    char ratio_buf[64];
    std::snprintf(ratio_buf, sizeof(ratio_buf), "%.6f", ratio);
    json += "  \"totals\": {\"entries\": " + std::to_string(count);
    json += ", \"original_size\": " + std::to_string(total_orig);
    json += ", \"stored_size\": " + std::to_string(total_stored);
    json += ", \"ratio\": ";
    json += ratio_buf;
    json += ", \"stored_count\": " + std::to_string(stored_count);
    json += ", \"compressed_count\": " + std::to_string(compressed_count);
    json += ", \"encrypted_count\": " + std::to_string(encrypted_count);
    json += ", \"duplicate_groups\": 0, \"duplicate_entries\": 0";
    json += ", \"duplicate_wasted_bytes\": 0";
    json += ", \"duplicate_reclaimed_bytes\": 0, \"verify_ran\": false";
    json += ", \"verified_count\": 0, \"corrupt_count\": 0";
    json += ", \"skipped_count\": 0},\n";
    json += "  \"entries\": [";

    for (uint32_t i = 0; i < count; ++i) {
        const JceArchiveEntry *e = jce_archive_get(ar, i);
        if (!e)
            continue;
        double eratio = e->original_size
            ? (double)e->stored_size / (double)e->original_size : 0.0;
        char hbuf[32];
        char cbuf[16];
        char erbuf[64];
        std::snprintf(hbuf, sizeof(hbuf), "0x%016llx",
                      (unsigned long long)e->path_hash);
        std::snprintf(cbuf, sizeof(cbuf), "0x%08x",
                      (unsigned)e->content_crc);
        std::snprintf(erbuf, sizeof(erbuf), "%.6f", eratio);
        const char *p = jce_archive_debug_path(ar, i);

        json += (i ? ",\n    {" : "\n    {");
        json += "\"index\": " + std::to_string(i);
        json += ", \"path\": ";
        json += p ? ("\"" + json_escape(p) + "\"") : "null";
        json += ", \"path_hash\": \"" + std::string(hbuf) + "\"";
        json += ", \"data_offset\": " + std::to_string(e->data_offset);
        json += ", \"original_size\": " + std::to_string(e->original_size);
        json += ", \"stored_size\": " + std::to_string(e->stored_size);
        json += ", \"ratio\": ";
        json += erbuf;
        json += ", \"compression\": " + std::to_string((unsigned)e->compression);
        json += ", \"compression_name\": \"";
        json += archive_comp_name(e->compression);
        json += "\"";
        if (e->dict_id == 0xFFFFu)
            json += ", \"dict_id\": null";
        else
            json += ", \"dict_id\": " + std::to_string((unsigned)e->dict_id);
        json += ", \"content_crc\": \"" + std::string(cbuf) + "\"";
        json += ", \"flags\": " + std::to_string((unsigned)e->entry_flags);
        json += ", \"page_aligned\": ";
        json += (e->entry_flags & JCE_ARCHIVE_ENTRY_PAGE_ALIGNED) ? "true" : "false";
        json += ", \"encrypted\": ";
        json += (e->entry_flags & JCE_ARCHIVE_ENTRY_ENCRYPTED) ? "true" : "false";
        json += ", \"duplicate\": false, \"verified\": null}";
    }
    json += count ? "\n  ]\n}\n" : "]\n}\n";

    jce_archive_close(ar);
    return jce_fs_host_write_all(path.c_str(), json.data(),
                                 (uint64_t)json.size());
}

std::string sanitize_c_ident(const std::string &name)
{
    std::string out;
    out.reserve(name.size() + 8);
    for (char c : name) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_';
        out.push_back(ok ? c : '_');
    }
    if (out.empty() || (out[0] >= '0' && out[0] <= '9'))
        out.insert(out.begin(), '_');
    return out;
}

std::string basename_no_ext(const std::string &path)
{
    std::string p = slash_norm(path);
    size_t slash = p.find_last_of('/');
    std::string leaf = slash == std::string::npos ? p : p.substr(slash + 1);
    size_t dot = leaf.find_last_of('.');
    return dot == std::string::npos ? leaf : leaf.substr(0, dot);
}

std::vector<std::string> split_cmake_list(const std::string &value)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= value.size()) {
        size_t next = value.find(';', pos);
        std::string item = (next == std::string::npos)
            ? value.substr(pos) : value.substr(pos, next - pos);
        if (!item.empty())
            out.push_back(item);
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return out;
}

std::string project_build_root(const std::string &project,
                               const char *platform_tag,
                               const std::string &arch)
{
    return project + PATH_SEP_CHR_LOCAL + "build" +
           PATH_SEP_CHR_LOCAL + std::string(platform_tag) + "-" + arch;
}

std::string variant_output_dir(const std::string &build_root,
                               const std::string &variant)
{
    return build_root + PATH_SEP_CHR_LOCAL + variant;
}

std::string project_reports_dir(const std::string &build_root)
{
    return build_root + PATH_SEP_CHR_LOCAL + "reports";
}

std::string project_intermediate_dir(const std::string &build_root,
                                     const std::string &variant)
{
    return build_root + PATH_SEP_CHR_LOCAL + "build" +
           PATH_SEP_CHR_LOCAL + variant;
}

int asset_pack_zstd_level_for_variant(const std::string &variant)
{
    return (variant == "release" || variant == "dist") ? 19 : 3;
}

bool write_project_assets_embed_artifact(const std::string &gen_dir,
                                         const std::string &arch,
                                         const std::string &pak_path,
                                         const void *pak_blob,
                                         size_t pak_size,
                                         std::string &out_obj,
                                         std::string &out_asm,
                                         std::string &out_c)
{
    out_obj.clear();
    out_asm.clear();
    out_c.clear();

#if JCE_PLATFORM_WINDOWS
    (void)pak_path;
    out_obj = gen_dir + PATH_SEP_CHR_LOCAL + "project_assets.obj";
    if (write_binary_coff_object(out_obj, "assets_pak_data",
                                 (const uint8_t *)pak_blob, pak_size, arch)) {
        jce_fs_host_remove_file((gen_dir + PATH_SEP_CHR_LOCAL +
                                 "project_assets.c").c_str());
        jce_fs_host_remove_file((gen_dir + PATH_SEP_CHR_LOCAL +
                                 "project_assets.S").c_str());
        return true;
    }
    log_line(JCE_CONSOLE_WARNING,
             "[build] assets: COFF object wrapper failed; using C fallback");
    out_obj.clear();
#else
    out_asm = gen_dir + PATH_SEP_CHR_LOCAL + "project_assets.S";
    if (write_binary_asm_incbin(out_asm, "assets_pak_data",
                                pak_path, pak_size)) {
        jce_fs_host_remove_file((gen_dir + PATH_SEP_CHR_LOCAL +
                                 "project_assets.c").c_str());
        jce_fs_host_remove_file((gen_dir + PATH_SEP_CHR_LOCAL +
                                 "project_assets.obj").c_str());
        return true;
    }
    log_line(JCE_CONSOLE_WARNING,
             "[build] assets: asm incbin wrapper failed; using C fallback");
    out_asm.clear();
#endif

    out_c = gen_dir + PATH_SEP_CHR_LOCAL + "project_assets.c";
    return write_binary_c_source(out_c, "assets_pak_data",
                                 (const uint8_t *)pak_blob, pak_size);
}

bool prepare_project_generated_assets(const std::string &project,
                                      const std::string &sdk,
                                      const std::string &cooked_rel,
                                      const std::string &bundles,
                                      const std::string &variant,
                                      const std::string &generated_root,
                                      const std::string &reports_dir,
                                      const std::string &arch,
                                      std::string &out_assets_obj,
                                      std::string &out_assets_asm,
                                      std::string &out_assets_c,
                                      std::string &out_bom,
                                      std::string &out_bundle_dir)
{
    const std::string gen_dir =
        generated_root + PATH_SEP_CHR_LOCAL + "jce_generated";
    if (!jce_fs_host_create_directory(gen_dir.c_str())) {
        set_error("assets: cannot create generated dir: " + gen_dir);
        return false;
    }
    if (!jce_fs_host_create_directory(reports_dir.c_str())) {
        set_error("assets: cannot create reports dir: " + reports_dir);
        return false;
    }

    std::vector<ProjectAssetInput> items;
    std::string error;
    const std::string engine_res = sdk + PATH_SEP_CHR_LOCAL + "share" +
                                   PATH_SEP_CHR_LOCAL + "jce" +
                                   PATH_SEP_CHR_LOCAL + "engine_resources";
    const std::string engine_ui = sdk + PATH_SEP_CHR_LOCAL + "share" +
                                  PATH_SEP_CHR_LOCAL + "jce" +
                                  PATH_SEP_CHR_LOCAL + "engine_ui";
    const std::string cooked = cooked_rel.empty()
        ? std::string()
        : project + PATH_SEP_CHR_LOCAL + join_norm_sep(cooked_rel);

    if (!collect_assets_from_dir(engine_res, items, error) ||
        !collect_assets_from_dir(engine_ui, items, error) ||
        !collect_assets_from_dir(cooked, items, error)) {
        set_error("assets: " + error);
        return false;
    }

    std::sort(items.begin(), items.end(),
              [](const ProjectAssetInput &a, const ProjectAssetInput &b) {
                  return a.vpath < b.vpath;
              });
    for (size_t i = 1; i < items.size(); ++i) {
        if (items[i - 1].vpath == items[i].vpath) {
            set_error("assets: duplicate virtual path '" + items[i].vpath +
                      "' from " + items[i - 1].abs_path + " and " +
                      items[i].abs_path);
            return false;
        }
    }

    out_assets_obj.clear();
    out_assets_asm.clear();
    out_assets_c.clear();
    out_bom = reports_dir + PATH_SEP_CHR_LOCAL + "project_assets_" +
              variant + ".pak.bom.json";
    const std::string pak_path =
        gen_dir + PATH_SEP_CHR_LOCAL + "project_assets.pak";

    std::vector<JceCookInput> inputs(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        inputs[i].vpath = items[i].vpath.c_str();
        inputs[i].data = items[i].bytes.data();
        inputs[i].size = items[i].bytes.size();
    }

    JceCookInput empty_input{};
    const JceCookInput *cook_inputs =
        inputs.empty() ? &empty_input : inputs.data();

    JceCookConfig cfg{};
    cfg.zstd_level = asset_pack_zstd_level_for_variant(variant);
    cfg.alignment_log2 = 4;
    cfg.emit_debug_paths = true;
    cfg.compress_index = true;
    cfg.use_dict = true;
    cfg.dedup_content = true;

    void *pak_blob = nullptr;
    size_t pak_size = 0;
    uint16_t dict_count = 0;
    if (!jce_archive_cook(cook_inputs, inputs.size(), &cfg,
                          &pak_blob, &pak_size, &dict_count)) {
        set_error("assets: archive cook failed");
        return false;
    }

    bool ok = jce_fs_host_write_all(pak_path.c_str(), pak_blob,
                                    (uint64_t)pak_size) &&
              write_project_assets_embed_artifact(gen_dir, arch, pak_path,
                                                  pak_blob, pak_size,
                                                  out_assets_obj,
                                                  out_assets_asm,
                                                  out_assets_c) &&
              write_asset_bom_json(out_bom, pak_path, pak_blob, pak_size);
    jce_free(pak_blob);
    if (!ok) {
        set_error("assets: failed to write generated PAK/embed/BOM outputs");
        return false;
    }

    if (items.empty()) {
        log_line(JCE_CONSOLE_WARNING,
                 "[build] assets: no resource files found; linked empty JPAK");
    }
    log_line(JCE_CONSOLE_INFO,
             "[build] assets: packed " + std::to_string(items.size()) +
             " file(s) -> " + pak_path + "  bom=" + out_bom +
             "  zstd=" + std::to_string(cfg.zstd_level) +
             (dict_count ? ("  dicts=" + std::to_string(dict_count)) : ""));

    out_bundle_dir = gen_dir + PATH_SEP_CHR_LOCAL + "bundles";
    if (!jce_fs_host_create_directory(out_bundle_dir.c_str())) {
        set_error("bundles: cannot create generated bundle dir: " + out_bundle_dir);
        return false;
    }

    for (const std::string &bundle_in : split_cmake_list(bundles)) {
        std::string bundle = bundle_in;
        if (!bundle.empty() && !jce_fs_host_exists_file(bundle.c_str()))
            bundle = project + PATH_SEP_CHR_LOCAL + join_norm_sep(bundle);
        if (!jce_fs_host_exists_file(bundle.c_str())) {
            log_line(JCE_CONSOLE_WARNING,
                     "[build] bundle embed skipped; file missing: " + bundle_in);
            continue;
        }

        uint64_t sz = 0;
        void *raw = jce_fs_host_read_all(bundle.c_str(), &sz);
        if (!raw) {
            set_error("bundles: cannot read " + bundle);
            return false;
        }

        std::string sym = "bundle_" + sanitize_c_ident(basename_no_ext(bundle));
#if JCE_PLATFORM_WINDOWS
        std::string src = out_bundle_dir + PATH_SEP_CHR_LOCAL +
                          "_embed_bundle_" + sym + ".obj";
        bool embed_ok = write_binary_coff_object(src, sym,
                                                 (const uint8_t *)raw,
                                                 (size_t)sz, arch);
        jce_fs_host_remove_file((out_bundle_dir + PATH_SEP_CHR_LOCAL +
                                 "_embed_bundle_" + sym + ".c").c_str());
#else
        std::string src = out_bundle_dir + PATH_SEP_CHR_LOCAL +
                          "_embed_bundle_" + sym + ".S";
        bool embed_ok = write_binary_asm_incbin(src, sym, bundle, (size_t)sz);
#endif
        jce_fs_buffer_free(raw);
        if (!embed_ok) {
            set_error("bundles: cannot write generated embed object: " + src);
            return false;
        }
        log_line(JCE_CONSOLE_INFO,
                 "[build] bundle embed object: " + bundle + " -> " + src);
    }

    return true;
}

bool has_exe_suffix(const std::string &name)
{
    if (name.size() < 4)
        return false;

    const size_t pos = name.size() - 4;
    const char suffix[5] = ".exe";
    for (size_t i = 0; i < 4; ++i) {
        char c = name[pos + i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != suffix[i])
            return false;
    }
    return true;
}

std::string project_exe_name(const std::string &target,
                             const char *configured)
{
    std::string name = (configured && configured[0])
                           ? std::string(configured)
                           : target;
#if JCE_PLATFORM_WINDOWS
    if (!has_exe_suffix(name))
        name += ".exe";
#endif
    return name;
}

std::string cmake_cache_norm(std::string s)
{
    for (char &c : s) {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}

std::string cmake_cli_path(std::string s)
{
    for (char &c : s) {
        if (c == '\\') c = '/';
    }
    return s;
}

bool cmake_cache_value(const std::string &text,
                       const char *key,
                       std::string *out)
{
    if (!key || !out) return false;
    std::string prefix = key;
    prefix += ":";

    size_t pos = 0;
    while ((pos = text.find(prefix, pos)) != std::string::npos) {
        if (pos != 0 && text[pos - 1] != '\n' && text[pos - 1] != '\r') {
            pos += prefix.size();
            continue;
        }

        size_t eq = text.find('=', pos + prefix.size());
        if (eq == std::string::npos)
            return false;
        size_t eol = text.find_first_of("\r\n", eq + 1);
        *out = text.substr(eq + 1,
                           eol == std::string::npos ? std::string::npos
                                                     : eol - eq - 1);
        return true;
    }
    return false;
}

#if JCE_PLATFORM_WINDOWS
std::string msvc_target_bin_arch(const std::string &arch)
{
    if (arch == "i686")    return "x86";
    if (arch == "aarch64") return "arm64";
    return "x64";
}

struct MsvcToolsPick {
    std::string root;
    std::string target_arch;
    std::string best_version;
};

bool pick_msvc_tools_dir(const char *name, bool is_dir, void *user)
{
    if (!name || !is_dir || !user)
        return true;

    MsvcToolsPick *pick = (MsvcToolsPick *)user;
    std::string cl = pick->root + PATH_SEP_CHR_LOCAL + name +
                     "\\bin\\Hostx64\\" + pick->target_arch + "\\cl.exe";
    if (!jce_fs_host_exists_file(cl.c_str()))
        return true;

    if (pick->best_version.empty() ||
        std::string(name) > pick->best_version)
        pick->best_version = name;
    return true;
}

std::string find_msvc_c_compiler(const std::string &vs_root,
                                 const std::string &arch)
{
    const std::string tools_root = vs_root + "\\VC\\Tools\\MSVC";
    if (!jce_fs_host_exists_dir(tools_root.c_str()))
        return "";

    MsvcToolsPick pick;
    pick.root        = tools_root;
    pick.target_arch = msvc_target_bin_arch(arch);
    jce_fs_host_list_dir(tools_root.c_str(), pick_msvc_tools_dir, &pick);
    if (pick.best_version.empty())
        return "";

    std::string cl = tools_root + PATH_SEP_CHR_LOCAL + pick.best_version +
                     "\\bin\\Hostx64\\" + pick.target_arch + "\\cl.exe";
    return jce_fs_host_exists_file(cl.c_str()) ? cmake_cli_path(cl) : "";
}
#endif

bool is_generated_project_build_dir(const std::string &project,
                                    const std::string &build_dir)
{
    std::string prefix = project;
    if (!prefix.empty() &&
        prefix.back() != PATH_SEP_CHR_LOCAL)
        prefix += PATH_SEP_CHR_LOCAL;
    prefix += "build";
    prefix += PATH_SEP_CHR_LOCAL;

    return build_dir.rfind(prefix, 0) == 0 &&
           build_dir.size() > prefix.size();
}

bool remove_project_build_cache(const std::string &project,
                                const std::string &build_dir,
                                const std::string &reason)
{
    if (!jce_fs_host_exists_dir(build_dir.c_str()))
        return true;

    if (!is_generated_project_build_dir(project, build_dir)) {
        set_error("refusing to remove non-generated build dir: " + build_dir);
        return false;
    }

    log_line(JCE_CONSOLE_INFO,
             "[build] removing project build cache (" + reason + "): " +
             build_dir);
    if (!jce_fs_host_remove_recursive(build_dir.c_str())) {
        log_line(JCE_CONSOLE_WARNING,
                 "[build] failed to remove project build cache: " +
                 build_dir);
        return false;
    }
    return true;
}

bool project_cache_needs_reset(const std::string &cache_path,
                               const std::string &cmake_dir,
                               const std::string &build_type,
                               const std::string &expected_c_compiler,
                               std::string *reason)
{
#if !JCE_PLATFORM_WINDOWS
    (void)expected_c_compiler;
#endif

    if (!jce_fs_host_exists_file(cache_path.c_str()))
        return false;

    uint64_t size = 0;
    char *raw = (char *)jce_fs_host_read_all(cache_path.c_str(), &size);
    if (!raw) {
        if (reason) *reason = "unreadable CMakeCache.txt";
        return true;
    }
    std::string text(raw, raw + size);
    jce_fs_buffer_free(raw);

    std::string val;
    if (!cmake_cache_value(text, "JCE_DIR", &val) ||
        val.find("NOTFOUND") != std::string::npos ||
        cmake_cache_norm(val) != cmake_cache_norm(cmake_dir)) {
        if (reason) *reason = "JCE_DIR changed or was not found";
        return true;
    }

    if (cmake_cache_value(text, "CMAKE_BUILD_TYPE", &val) &&
        cmake_cache_norm(val) != cmake_cache_norm(build_type)) {
        if (reason) *reason = "CMAKE_BUILD_TYPE changed";
        return true;
    }

#if JCE_PLATFORM_WINDOWS
    if (!cmake_cache_value(text, "CMAKE_C_COMPILER", &val)) {
        if (reason) *reason = "CMAKE_C_COMPILER missing";
        return true;
    }
    if (!expected_c_compiler.empty() &&
        cmake_cache_norm(val) != cmake_cache_norm(expected_c_compiler)) {
        if (reason) *reason = "CMAKE_C_COMPILER changed";
        return true;
    }
#endif

    return false;
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
    const char *platform_tag       = "win32";
#elif JCE_PLATFORM_MACOS
    const char *platform_tag       = "darwin";
#else
    const char *platform_tag       = "linux";
#endif
    const std::string exe_name = project_exe_name(target, cfg->exe_name);

    /* Resolve the SDK's CMake package dir: prefer the installed
     * <sdk>/lib/cmake/JCE layout, fall back to <sdk>/cmake. */
    std::string cmake_dir;
    {
        const std::string a = sdk + PATH_SEP_CHR_LOCAL + "lib" +
                              PATH_SEP_CHR_LOCAL + "cmake" +
                              PATH_SEP_CHR_LOCAL + "JCE";
        const std::string b = sdk + PATH_SEP_CHR_LOCAL + "cmake";
        if (jce_fs_host_exists_file(
                (a + PATH_SEP_CHR_LOCAL + "JCEConfig.cmake").c_str())) {
            cmake_dir = a;
        } else if (jce_fs_host_exists_file(
                (b + PATH_SEP_CHR_LOCAL + "JCEConfig.cmake").c_str())) {
            cmake_dir = b;
        } else {
            set_error("SDK CMake package not found under " + sdk +
                      " (expected lib/cmake/JCE/JCEConfig.cmake)");
            return false;
        }
    }

    const std::string build_dir = project_build_root(project,
                                                     platform_tag,
                                                     arch);
    const std::string output_dir = variant_output_dir(build_dir, variant);
    const std::string reports_dir = project_reports_dir(build_dir);
    const std::string intermediates_dir =
        project_intermediate_dir(build_dir, variant);
    const std::string archive_dir =
        intermediates_dir + PATH_SEP_CHR_LOCAL + "lib";
    const std::string build_type =
        (variant == "debug") ? std::string("Debug") : std::string("Release");
    bool use_cmake_fresh = false;

    std::string c_compiler;
#if JCE_PLATFORM_WINDOWS
    jce_toolchain_refresh();
    const JceToolchain *msvc = jce_toolchain_get(JCE_TOOLCHAIN_MSVC);
    std::string vcvars;
    if (msvc && msvc->present && msvc->path[0]) {
        const std::string vs_root = join_norm_sep(msvc->path);
        const std::string cand = vs_root + PATH_SEP_CHR_LOCAL + "VC" +
                                 PATH_SEP_CHR_LOCAL + "Auxiliary" +
                                 PATH_SEP_CHR_LOCAL + "Build" +
                                 PATH_SEP_CHR_LOCAL + "vcvarsall.bat";
        if (jce_fs_host_exists_file(cand.c_str()))
            vcvars = cand;
        c_compiler = find_msvc_c_compiler(vs_root, arch);
    }
    std::string vc_arch = "x64";
    if (arch == "i686")    vc_arch = "x64_x86";
    if (arch == "aarch64") vc_arch = "x64_arm64";
#endif

    if (cfg->clean) {
        if (!remove_project_build_cache(project, build_dir, "clean requested")) {
            set_error("failed to remove project build cache: " + build_dir);
            return false;
        }
    } else {
        const std::string cache = build_dir + PATH_SEP_CHR_LOCAL +
                                  "CMakeCache.txt";
        std::string reset_reason;
        if (project_cache_needs_reset(cache, cmake_dir, build_type,
                                      c_compiler,
                                      &reset_reason) &&
            !remove_project_build_cache(project, build_dir, reset_reason)) {
            if (!jce_fs_host_remove_file(cache.c_str())) {
                set_error("failed to reset project build cache: " +
                          build_dir);
                return false;
            }
            use_cmake_fresh = true;
            log_line(JCE_CONSOLE_WARNING,
                     "[build] project build dir was locked; removed "
                     "CMakeCache.txt and will configure with cmake --fresh");
        }
    }

    const std::string cooked = (cfg->cooked_assets && cfg->cooked_assets[0])
                                   ? std::string(cfg->cooked_assets)
                                   : "resources/_cooked";
    const std::string bundles = (cfg->bundles && cfg->bundles[0])
                                    ? std::string(cfg->bundles) : "";

    std::string prebuilt_assets_obj;
    std::string prebuilt_assets_asm;
    std::string prebuilt_assets_c;
    std::string prebuilt_assets_bom;
    std::string prebuilt_bundle_dir;
    if (!prepare_project_generated_assets(project, sdk, cooked, bundles,
                                          variant, intermediates_dir,
                                          reports_dir, arch,
                                          prebuilt_assets_obj,
                                          prebuilt_assets_asm,
                                          prebuilt_assets_c,
                                          prebuilt_assets_bom,
                                          prebuilt_bundle_dir)) {
        return false;
    }

    /* Shared configure flags (identical across platforms). */
    auto append_configure = [&](std::string &a) {
        if (use_cmake_fresh)
            a += " --fresh";
        a += " -S " + qtok(project);
        a += " -B " + qtok(build_dir);
        a += " -G Ninja";
        a += " -DCMAKE_BUILD_TYPE=" + build_type;
        a += " -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=" + qtok(output_dir);
        a += " -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=" + qtok(output_dir);
        a += " -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=" + qtok(archive_dir);
        a += " -DJCE_PROJECT_COOKED_ASSETS=" + qtok(cooked);
        a += " -DJCE_PROJECT_BUNDLES=" + qtok(bundles);
        a += " -U JCE_PROJECT_PREBUILT_ASSETS_OBJ";
        a += " -U JCE_PROJECT_PREBUILT_ASSETS_ASM";
        a += " -U JCE_PROJECT_PREBUILT_ASSETS_C";
        if (!prebuilt_assets_obj.empty())
            a += " -DJCE_PROJECT_PREBUILT_ASSETS_OBJ=" + qtok(prebuilt_assets_obj);
        if (!prebuilt_assets_asm.empty())
            a += " -DJCE_PROJECT_PREBUILT_ASSETS_ASM=" + qtok(prebuilt_assets_asm);
        if (!prebuilt_assets_c.empty())
            a += " -DJCE_PROJECT_PREBUILT_ASSETS_C=" + qtok(prebuilt_assets_c);
        a += " -DJCE_PROJECT_PREBUILT_BUNDLE_DIR=" + qtok(prebuilt_bundle_dir);
        a += " -DJCE_DIR=" + qtok(cmake_dir);
    };

#if JCE_PLATFORM_WINDOWS
    /* The Windows SDK ships MSVC-built static libs, so force cl and
     * activate the MSVC environment inline via Microsoft's own
     * vcvarsall.bat (NOT a first-party script).  Both configure and
     * build run in one cmd.exe /c chain so the env survives between
     * them. */
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
    if (!c_compiler.empty())
        args += " -DCMAKE_C_COMPILER=" + qtok(c_compiler);
    else
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
    g_finish.artifact_a = output_dir + PATH_SEP_CHR_LOCAL + exe_name;
    g_finish.artifact_b = build_dir + PATH_SEP_CHR_LOCAL + exe_name;
    g_finish.asset_bom_src = prebuilt_assets_bom;

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
