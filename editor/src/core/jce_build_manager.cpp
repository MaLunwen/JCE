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

#include "jce_binary_embed.h"
#include "jce_editor_project.h"
#include "jce_dist_content_graph.h"
#include "jce_dist_audit.h"
#include "jce_pak_key.h"
#include "jce_project_settings.h"
#include <jce/middleware/physics/jce_physics_layers.h>  /* layer matrix export (Top 4) */
#include <jce/renderer/jce_render_settings.h>            /* quality export (Top 5) */
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
#include <jce/os/core/jce_json.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_archive_cook.h>
#include <jce/resource/jce_bundle_format.h>
#include <jce/resource/jce_bundle_pack.h>
#include <jce/application/jce_project.h>
#include <jce/application/jce_runtime_boot.h>
}

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

/* Defined in jce_dialog_project.cpp (global, external linkage).  Declared at
 * file scope so references inside the anonymous namespace below bind to the
 * external symbol rather than declaring an internal-linkage one (C7631). */
extern char s_current_project_root[512];

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

struct ApprovedGraphAsset {
    std::string address;
    uint64_t content_id = 0;
};

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
    bool        stage_loose = true;/* copy the loose cooked tree (OFF when   *
                                    * the assets ship encrypted in the PAK)  */
    std::string warn_loose_dir;    /* encrypting: if this dir exists post-   *
                                    * build, an old template staged          *
                                    * plaintext next to the exe — warn       */
    bool        dist = false;
    std::string pak_path;
    std::string audit_report;
    std::vector<std::string> protected_paths;
    std::vector<ApprovedGraphAsset> graph_assets;
    std::string graph_snapshot;
    uint8_t     pak_key[32] = {0};
};
FinishPlan g_finish;

void reset_pipeline()
{
    g_queue.clear();
    g_queue_pos = 0;
    std::memset(g_finish.pak_key, 0, sizeof(g_finish.pak_key));
    g_finish = FinishPlan{};
}

/* ---------------------------------------------------------------- *
 * Worker-thread log redirect.                                       *
 *                                                                   *
 * prepare_project_generated_assets() and its helpers call           *
 * log_line()/set_error() directly.  When that cook/pack runs on a   *
 * background build thread, those calls would touch the editor       *
 * console ring buffer and the ImGui toast path, neither of which is *
 * thread-safe.  While a sink is installed on the calling thread we  *
 * capture entries here and replay them on the main thread once the  *
 * worker is joined (poll_asset_prep).                               *
 * ---------------------------------------------------------------- */
struct DeferredLogEntry {
    JceConsoleLevel level;
    std::string     text;
};
struct WorkerLogSink {
    std::vector<DeferredLogEntry> entries;
    std::string                   last_error;
};
thread_local WorkerLogSink *t_log_sink = nullptr;

/* ---------------------------------------------------------------- *
 * Background asset-prepare (cook/pack/embed) job.                    *
 *                                                                   *
 * prepare_project_generated_assets() is the heavy, UI-freezing step *
 * of a project build (ZSTD archive cook + file writes + embed       *
 * object generation).  It is moved onto a dedicated worker thread;  *
 * the main thread polls `done` each frame and, on success, builds   *
 * the cmake configure/compile queue and spawns it.  All inputs the  *
 * cook needs and all context the post-cook pipeline construction    *
 * needs are captured up-front so the worker touches no shared       *
 * editor state besides its own job + log sink.                      *
 * ---------------------------------------------------------------- */
struct PendingProjectBuild {
    bool active = false;

    /* prepare_project_generated_assets inputs */
    std::string project, sdk, cooked, bundles, variant, arch;
    std::string intermediates_dir, reports_dir;
    bool        encrypt_assets = false;   /* packaging.encrypt_assets gate */
    uint8_t     pak_key[32]    = {0};     /* loaded before the worker runs */

    /* prepare_project_generated_assets outputs (filled by the worker) */
    std::string out_assets_obj, out_assets_asm, out_assets_c;
    std::string out_assets_bom, out_bundle_dir, out_pak_key_c;
    std::string out_pak_path;
    std::vector<std::string> protected_paths;
    std::vector<ApprovedGraphAsset> graph_assets;
    std::string out_graph_snapshot;

    /* Context needed to build the cmake queue + finish plan on the main
     * thread once the cook succeeds. */
    std::string build_dir, output_dir, archive_dir, build_type, cmake_dir;
    std::string target, label, exe_name, platform_tag;
    bool        use_cmake_fresh = false;
#if JCE_PLATFORM_WINDOWS
    std::string vcvars, vc_arch, c_compiler;
#endif
    bool        want_package = false;
    std::string package_out_dir, app_name, app_version;

    /* Worker plumbing */
    JceThread    *thread = nullptr;
    JceAtomicI32 *done   = nullptr;   /* 0 running, 1 finished */
    JceAtomicI32 *cancel = nullptr;   /* set by request_stop */
    bool          ok     = false;
    WorkerLogSink sink;
};
PendingProjectBuild g_pending;

void poll_asset_prep();           /* main thread, per-frame */
void asset_prep_shutdown();       /* main thread, on editor exit */
bool asset_prep_active();         /* a cook/pack is in flight */

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
    if (t_log_sink) {
        t_log_sink->entries.push_back({level, line});
        return;
    }
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
    if (t_log_sink) {
        t_log_sink->last_error = msg;
        t_log_sink->entries.push_back({JCE_CONSOLE_ERROR, "[build] " + msg});
        return;
    }
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

bool run_dist_audit(const std::string &exe, const char *package_dir)
{
    std::vector<const char *> path_ptrs;
    path_ptrs.reserve(g_finish.protected_paths.size());
    for (const std::string &path : g_finish.protected_paths)
        path_ptrs.push_back(path.c_str());
    std::vector<JceDistGraphAsset> graph_assets;
    graph_assets.reserve(g_finish.graph_assets.size());
    for (const ApprovedGraphAsset &asset : g_finish.graph_assets)
        graph_assets.push_back({asset.address.c_str(), asset.content_id});

    JceDistAuditInput input{};
    input.executable_path = exe.c_str();
    input.pak_path = g_finish.pak_path.c_str();
    input.report_path = g_finish.audit_report.c_str();
    input.package_dir = package_dir;
    input.expected_exe_name = g_finish.exe_name.c_str();
    input.graph_snapshot_path = g_finish.graph_snapshot.c_str();
    input.protected_paths = path_ptrs.empty() ? nullptr : path_ptrs.data();
    input.protected_path_count = path_ptrs.size();
    input.graph_assets = graph_assets.empty() ? nullptr : graph_assets.data();
    input.graph_asset_count = graph_assets.size();
    input.key = g_finish.pak_key;
    input.require_secure = true;
#if JCE_PLATFORM_WINDOWS
    input.require_gui = true;
#endif
    input.require_single_file = package_dir && package_dir[0];
    input.require_graph_parity = true;

    JceDistAuditResult result{};
    if (!jce_dist_audit_run(input, &result)) {
        g_build.state = JCE_BUILD_FAILED;
        set_error(std::string("dist audit failed: ") +
                  (result.error[0] ? result.error : "unknown error") +
                  " (report: " + g_finish.audit_report + ")");
        return false;
    }
    log_line(JCE_CONSOLE_INFO,
             "[build] dist audit passed: entries=" +
             std::to_string(result.archive_entries) +
             " verified=" + std::to_string(result.verified_entries) +
             " graph=" + std::to_string(result.graph_verified) + "/" +
             std::to_string(result.graph_expected) +
             " path_leaks=" + std::to_string(result.leaked_path_count) +
             " report=" + g_finish.audit_report);
    return true;
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

    /* Plaintext side-channel check: when assets are encrypted, the project
     * CMake is told not to stage the loose cooked tree next to the exe
     * (JCE_PROJECT_STAGE_LOOSE_ASSETS=OFF).  An old project template that
     * predates the option will have copied it anyway — we can't suppress
     * that from here, so warn loudly. */
    if (!g_finish.warn_loose_dir.empty() &&
        jce_fs_host_exists_dir(g_finish.warn_loose_dir.c_str())) {
        log_line(JCE_CONSOLE_WARNING,
                 "[build] encrypted build still staged a loose plaintext "
                 "asset tree (project CMakeLists predates "
                 "JCE_PROJECT_STAGE_LOOSE_ASSETS?): " +
                 g_finish.warn_loose_dir);
    }

    if (!g_finish.stage) {
        if (g_finish.dist)
            run_dist_audit(exe, nullptr);
        return;
    }

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

    /* Dist is a one-file public package.  Authoring configs, symbols, BOMs,
     * and audit data remain under the private build/reports tree. */
    if (g_finish.dist) {
        if (run_dist_audit(dst_exe, out.c_str()))
            log_line(JCE_CONSOLE_INFO, "[build] dist package staged at: " + out);
        return;
    }

    /* Optional authored runtime config, staged beside the exe.  Copied only
     * when the project authored it; silent when absent (the runtime seeds
     * built-in defaults).
     *  - Settings/audio_mixer.json — jce_default_main resolves it host-first
     *    (CWD, then exe dir), so the packaged layout mirrors the project's
     *    Settings/ next to the exe.
     *  - .jce/input_actions.json — jce_select_input_actions_path probes
     *    ".jce/input_actions.json" relative to the CWD (then HOME), so the
     *    packaged layout mirrors the project's .jce/ next to the exe. */
    {
        std::string proj = s_current_project_root[0]
                               ? std::string(s_current_project_root)
                               : std::string(".");
        static const struct { const char *dir; const char *file;
                              const char *label; } kRuntimeCfg[] = {
            { "Settings", "audio_mixer.json",   "audio mixer config" },
            { ".jce",     "input_actions.json", "input actions"      },
        };
        for (const auto &c : kRuntimeCfg) {
            std::string src = proj + PATH_SEP_CHR_LOCAL + c.dir +
                              PATH_SEP_CHR_LOCAL + c.file;
            if (!jce_fs_host_exists_file(src.c_str()))
                continue;
            std::string dst_dir = out + PATH_SEP_CHR_LOCAL + c.dir;
            jce_fs_host_create_directory(dst_dir.c_str());
            std::string dst = dst_dir + PATH_SEP_CHR_LOCAL + c.file;
            if (jce_fs_host_copy_file(src.c_str(), dst.c_str()))
                log_line(JCE_CONSOLE_INFO,
                         std::string("[build] ") + c.label + " -> " + dst);
            else
                log_line(JCE_CONSOLE_WARNING,
                         std::string("[build] failed to stage ") + c.label +
                         ": " + src);
        }
    }

    /* Top 4 — export the project's authored physics layer collision matrix into
     * the cooked tree so the shipped game's app_init loads the SAME collision
     * filtering editor Play uses.  ps is the authoritative source: editor Play
     * (jce_editor_play.cpp) and this build both drive the engine matrix from it,
     * so the standalone physics-layers panel's file is already overridden by
     * Play.  Written before the cooked-tree staging below so the copy picks it
     * up.  NOTE: loose-tree only — an encrypted/embedded-PAK package
     * (stage_loose=false) does not carry it yet (needs a PAK-aware matrix
     * loader; tracked as follow-up). */
    if (!g_finish.cooked_src.empty() &&
        jce_fs_host_exists_dir(g_finish.cooked_src.c_str())) {
        JceProjectSettings ps_local;
        const JceProjectSettings *ps = jce_project_settings_current();
        if (!ps) { jce_project_settings_load(&ps_local); ps = &ps_local; }
        for (uint32_t i = 0; i < JCE_PS_LAYER_COUNT; ++i) {
            jce_physics_layer_set_name(i, ps->tags_layers.layers[i]);
            for (uint32_t j = i; j < JCE_PS_LAYER_COUNT; ++j)
                jce_physics_set_layer_collides(
                    i, j, (ps->physics.layer_collision_matrix[i] >> j) & 1u);
        }
        std::string lp = g_finish.cooked_src + PATH_SEP_CHR_LOCAL +
                         "physics_layers.json";
        if (jce_physics_layer_matrix_save_json(lp.c_str()))
            log_line(JCE_CONSOLE_INFO, "[build] physics layer matrix -> " + lp);
        else
            log_line(JCE_CONSOLE_WARNING,
                     "[build] failed to write physics layer matrix: " + lp);

        /* Top 5 — export the active quality level's render settings so the
         * shipped game's app_init applies the authored shadow tier + lod bias
         * (per-scene JceSceneRenderingSettings still override at render time). */
        JceRenderSettings rs = jce_render_settings_default();
        int ql = ps->quality.current_level;
        if (ql < 0) ql = 0;
        if (ql >= JCE_PS_MAX_QUALITY_LEVELS) ql = JCE_PS_MAX_QUALITY_LEVELS - 1;
        const JceProjectQualityLevel *lvl = &ps->quality.levels[ql];
        rs.shadow_quality = lvl->shadow_quality;
        {
            static const int kShadowRes[4] = { 512, 1024, 2048, 4096 };
            int sr = lvl->shadow_resolution;
            if (sr < 0) sr = 0; if (sr > 3) sr = 3;
            rs.shadow_map_size = kShadowRes[sr];
        }
        rs.shadow_cascades = lvl->shadow_cascades;
        rs.shadow_distance = lvl->shadow_distance;
        rs.lod_bias        = lvl->lod_bias;
        rs.vsync           = lvl->vsync_count > 0 ? 1 : 0;
        rs.msaa            = ps->graphics.default_msaa;   /* 0/2/4/8 (Graphics block) */
        std::string rp = g_finish.cooked_src + PATH_SEP_CHR_LOCAL +
                         "render_settings.json";
        if (jce_render_settings_save_json(rp.c_str(), &rs))
            log_line(JCE_CONSOLE_INFO, "[build] render settings -> " + rp);
        else
            log_line(JCE_CONSOLE_WARNING,
                     "[build] failed to write render settings: " + rp);

        /* Settings S2 — export the authored render-pipeline asset into the
         * cooked tree (PAK key settings/render_pipeline.rp.json); the runtime
         * re-resolves it after bundle mount (apply_boot_mounted).  Without
         * this, shipped single-exe games silently fell back to the tier
         * auto-preset. */
        {
            std::string proj = s_current_project_root[0]
                                   ? std::string(s_current_project_root)
                                   : std::string(".");
            std::string rp_src = proj + PATH_SEP_CHR_LOCAL + "Settings" +
                                 PATH_SEP_CHR_LOCAL + "RenderPipeline.rp.json";
            if (jce_fs_host_exists_file(rp_src.c_str())) {
                std::string rp_dir = g_finish.cooked_src + PATH_SEP_CHR_LOCAL +
                                     "settings";
                jce_fs_host_create_directory(rp_dir.c_str());
                std::string rp_dst = rp_dir + PATH_SEP_CHR_LOCAL +
                                     "render_pipeline.rp.json";
                if (jce_fs_host_copy_file(rp_src.c_str(), rp_dst.c_str()))
                    log_line(JCE_CONSOLE_INFO,
                             "[build] render pipeline asset -> " + rp_dst);
                else
                    log_line(JCE_CONSOLE_WARNING,
                             "[build] failed to stage render pipeline asset: " +
                             rp_src);
            }

            /* Mirror the authored audio-mixer routing into the same cooked
             * settings/ channel (PAK key settings/audio_mixer.json).  NOTE:
             * the runtime mixer read is still host-only (rt_mixer_read_config
             * uses jce_fs_host_read_all; no apply_boot_mounted analogue for
             * audio yet), so a pure single-exe build does not consume this
             * copy — it travels with the cooked tree for parity with the
             * render pipeline asset and for a future PAK-aware read. */
            std::string mx_src = proj + PATH_SEP_CHR_LOCAL + "Settings" +
                                 PATH_SEP_CHR_LOCAL + "audio_mixer.json";
            if (jce_fs_host_exists_file(mx_src.c_str())) {
                std::string mx_dir = g_finish.cooked_src + PATH_SEP_CHR_LOCAL +
                                     "settings";
                jce_fs_host_create_directory(mx_dir.c_str());
                std::string mx_dst = mx_dir + PATH_SEP_CHR_LOCAL +
                                     "audio_mixer.json";
                if (jce_fs_host_copy_file(mx_src.c_str(), mx_dst.c_str()))
                    log_line(JCE_CONSOLE_INFO,
                             "[build] audio mixer config (cooked) -> " + mx_dst);
                else
                    log_line(JCE_CONSOLE_WARNING,
                             "[build] failed to stage audio mixer config: " +
                             mx_src);
            }
        }
    }

    /* Stage cooked assets so the packaged game has its PhysFS mount
     * root alongside the exe (mirrors package-game.bat).  Suppressed when
     * the assets ship encrypted inside the embedded PAK — staging the
     * plaintext tree would defeat the encryption. */
    if (!g_finish.stage_loose) {
        log_line(JCE_CONSOLE_INFO,
                 "[build] package: loose cooked assets NOT staged "
                 "(assets are encrypted inside the embedded PAK)");
    } else if (!g_finish.cooked_rel.empty() && !g_finish.cooked_src.empty() &&
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
    asset_prep_shutdown();   /* join any in-flight cook/pack worker */
    if (g_build.process) {
        jce_process_force_kill(g_build.process);
        release_process();
    }
    g_build = Build{};
}

void jce_build_manager_poll(void)
{
    poll_asset_prep();   /* advance a background cook/pack into compile */
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
    if (g_build.process || asset_prep_active()) {
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
    /* If the cook/pack worker is in flight there is no child process yet;
     * flag the pending build so poll_asset_prep() aborts before the
     * (heavy) compile is spawned.  The cook itself is a single call and
     * cannot be interrupted mid-flight. */
    if (g_pending.active && g_pending.cancel) {
        jce_atomic_i32_store(g_pending.cancel, 1);
        log_line(JCE_CONSOLE_INFO,
                 "[build] stop requested; will abort after cook completes");
        return;
    }
    if (!g_build.process || g_build.state != JCE_BUILD_RUNNING) return;
    if (g_build.stopping) return;
    jce_process_request_stop(g_build.process);
    g_build.stopping         = true;
    g_build.stop_deadline_ms = jce_time_ticks_ms() + kGracefulStopMs;
    log_line(JCE_CONSOLE_INFO, "[build] requested tool stop");
}

bool jce_build_manager_is_running(void)
{
    return g_build.state == JCE_BUILD_RUNNING || g_pending.active;
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
    bool                 protect_path = false;
};

struct CollectAssetCtx {
    std::string root;
    std::vector<ProjectAssetInput> *items = nullptr;
    std::string error;
    bool protect_paths = false;
};

bool ascii_ends_with(std::string value, const char *suffix)
{
    if (!suffix) return false;
    for (char &c : value) {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    const size_t suffix_len = std::strlen(suffix);
    return value.size() >= suffix_len &&
           value.compare(value.size() - suffix_len, suffix_len, suffix) == 0;
}

bool shader_profile_unreachable(const std::string &vpath)
{
    const bool dx11  = ascii_ends_with(vpath, "_dx11.bin");
    const bool spv   = ascii_ends_with(vpath, "_spv.bin");
    const bool glsl  = ascii_ends_with(vpath, "_glsl.bin");
    const bool essl  = ascii_ends_with(vpath, "_essl.bin");
    const bool essl1 = ascii_ends_with(vpath, "_essl1.bin");
    const bool mtl   = ascii_ends_with(vpath, "_mtl.bin");
    if (!(dx11 || spv || glsl || essl || essl1 || mtl))
        return false;
#if JCE_PLATFORM_WINDOWS
    return essl || essl1 || mtl;
#elif JCE_PLATFORM_MACOS
    return dx11 || glsl || essl || essl1;
#elif JCE_PLATFORM_WEB
    return dx11 || spv || glsl || essl1 || mtl;
#elif JCE_PLATFORM_ANDROID
    return dx11 || glsl || essl1 || mtl;
#else
    return dx11 || essl || essl1 || mtl;
#endif
}

bool collect_asset_walk_cb(const char *path, bool is_dir, void *user)
{
    CollectAssetCtx *ctx = (CollectAssetCtx *)user;
    if (!ctx || !ctx->items || !path)
        return false;
    if (is_dir)
        return true;

    std::string rel = make_relative_vpath(path, ctx->root);
    if (rel.empty() || path_has_segment(rel, "raw_assets") ||
        path_has_hidden_segment(rel) || shader_profile_unreachable(rel)) {
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
    item.protect_path = ctx->protect_paths;
    item.bytes.resize((size_t)sz);
    if (sz)
        std::memcpy(item.bytes.data(), raw, (size_t)sz);
    jce_fs_buffer_free(raw);
    ctx->items->push_back(std::move(item));
    return true;
}

bool collect_assets_from_dir(const std::string &root,
                             std::vector<ProjectAssetInput> &items,
                             std::string &error,
                             bool protect_paths)
{
    if (root.empty() || !jce_fs_host_exists_dir(root.c_str()))
        return true;

    CollectAssetCtx ctx;
    ctx.root = root;
    ctx.items = &items;
    ctx.protect_paths = protect_paths;
    if (!jce_fs_host_walk(root.c_str(), collect_asset_walk_cb, &ctx)) {
        error = ctx.error.empty() ? ("cannot walk asset dir: " + root)
                                  : ctx.error;
        return false;
    }
    return true;
}

bool append_runtime_boot_asset(const std::string &project,
                               std::vector<ProjectAssetInput> &items,
                               std::string &error)
{
    JceProject *manifest = jce_project_load(project.c_str());
    if (!manifest) {
        error = "cannot load project manifest: " + project;
        return false;
    }

    const char *startup_scene = manifest->startup_scene;

    JceJson *boot_json = jce_json_object();
    if (!boot_json) {
        jce_project_free(manifest);
        error = "cannot allocate runtime boot manifest";
        return false;
    }
    jce_json_set_string(boot_json, "contract", JCE_RUNTIME_BOOT_CONTRACT);
    jce_json_set_int(boot_json, "schema", JCE_RUNTIME_BOOT_SCHEMA_VERSION);
    jce_json_set_string(boot_json, "startup_scene",
                        startup_scene ? startup_scene : "");
    char *text = jce_json_print(boot_json, false);
    jce_json_free(boot_json);
    jce_project_free(manifest);
    if (!text) {
        error = "cannot serialize runtime boot manifest";
        return false;
    }

    JceRuntimeBootManifest parsed{};
    const size_t text_size = std::strlen(text);
    const bool valid = jce_runtime_boot_manifest_parse(text, text_size,
                                                       &parsed);
    if (!valid) {
        jce_json_free_string(text);
        error = "project startup_scene is not a valid runtime virtual path";
        return false;
    }

    ProjectAssetInput item;
    item.abs_path = project + PATH_SEP_CHR_LOCAL + "jce_project.json";
    item.vpath = JCE_RUNTIME_BOOT_MANIFEST_PATH;
    item.bytes.assign((const uint8_t *)text,
                      (const uint8_t *)text + text_size);
    jce_json_free_string(text);
    items.push_back(std::move(item));
    return true;
}

void dist_content_graph_log(int level, const char *message, void *)
{
    JceConsoleLevel console_level = JCE_CONSOLE_INFO;
    if (level == JCE_BUNDLE_PACK_LOG_WARNING)
        console_level = JCE_CONSOLE_WARNING;
    else if (level == JCE_BUNDLE_PACK_LOG_ERROR)
        console_level = JCE_CONSOLE_ERROR;
    log_line(console_level,
             std::string("[build] graph: ") + (message ? message : ""));
}

bool append_dist_bundle_graph(const std::string &project,
                              const std::string &gen_dir,
                              const std::string &reports_dir,
                              std::vector<ProjectAssetInput> &items,
                              std::vector<ApprovedGraphAsset> &graph_assets,
                              std::string &graph_snapshot,
                              std::string &error)
{
    JceDistContentGraph graph;
    if (!jce_dist_content_graph_build(
            project, gen_dir, reports_dir,
            jce_dist_content_graph_host_platform(), dist_content_graph_log,
            nullptr, &graph, &error)) {
        return false;
    }

    graph_assets.clear();
    for (JceDistContentGraphAsset &source : graph.assets) {
        ProjectAssetInput item;
        item.abs_path = std::move(source.source_label);
        item.vpath = source.address;
        item.bytes = std::move(source.bytes);
        item.protect_path = true;
        items.push_back(std::move(item));
        graph_assets.push_back({std::move(source.address), source.content_id});
    }
    graph_snapshot = std::move(graph.snapshot_path);
    log_line(JCE_CONSOLE_INFO,
             "[build] Dist consumes Bundle graph: " +
             std::to_string(graph_assets.size()) +
             " cooked asset(s), snapshot=" + graph_snapshot);
    return true;
}

bool write_asset_bom_json(const std::string &path, const std::string &pak_path,
                          const void *pak_blob, size_t pak_size,
                          const std::vector<ProjectAssetInput> &source_items,
                          const uint8_t *verify_key)
{
    JceArchive *ar = jce_archive_open(pak_blob, pak_size);
    if (!ar)
        return false;

    if (verify_key)
        jce_archive_set_decryption_key(ar, verify_key);
    if (jce_archive_is_authenticated(ar) &&
        jce_archive_auth_status(ar) != JCE_ARCHIVE_AUTH_VALID) {
        jce_archive_close(ar);
        return false;
    }

    uint32_t count = jce_archive_count(ar);
    std::vector<const char *> report_paths(count, nullptr);
    for (const ProjectAssetInput &item : source_items) {
        const JceArchiveEntry *entry = jce_archive_find(ar, item.vpath.c_str());
        if (!entry)
            continue;
        for (uint32_t i = 0; i < count; ++i) {
            if (jce_archive_get(ar, i) == entry) {
                report_paths[i] = item.vpath.c_str();
                break;
            }
        }
    }

    const bool header_verified = jce_archive_verify_header(ar) != 0;
    const bool secure_index = jce_archive_is_secure(ar) != 0;
    const bool authenticated = jce_archive_is_authenticated(ar) != 0;
    const bool auth_verified = !authenticated ||
        jce_archive_auth_status(ar) == JCE_ARCHIVE_AUTH_VALID;
    bool has_debug_paths = false;
    std::vector<int8_t> entry_verified(count, -1);
    std::vector<uint64_t> entry_content_ids(count, 0);
    uint32_t verified_count = 0;
    uint32_t corrupt_count = 0;
    uint32_t skipped_count = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const JceArchiveEntry *entry = jce_archive_get(ar, i);
        if (!entry || entry->original_size > (uint64_t)SIZE_MAX) {
            entry_verified[i] = 0;
            ++corrupt_count;
            continue;
        }
        if ((entry->entry_flags & JCE_ARCHIVE_ENTRY_ENCRYPTED) &&
            !verify_key) {
            entry_verified[i] = -1;
            ++skipped_count;
            continue;
        }
        std::vector<uint8_t> decoded((size_t)entry->original_size);
        const size_t got = jce_archive_read(ar, entry, decoded.data(),
                                            decoded.size());
        if (got == decoded.size() &&
            jce_archive_verify_entry(entry, decoded.data(), got)) {
            entry_verified[i] = 1;
            entry_content_ids[i] = jce_archive_content_hash(decoded.data(),
                                                            got);
            ++verified_count;
        } else {
            entry_verified[i] = 0;
            ++corrupt_count;
        }
    }
    uint64_t total_orig = 0;
    uint64_t total_stored = 0;
    uint32_t stored_count = 0;
    uint32_t compressed_count = 0;
    uint32_t encrypted_count = 0;
    uint32_t authenticated_count = 0;

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
        if (e->entry_flags & JCE_ARCHIVE_ENTRY_AUTHENTICATED)
            ++authenticated_count;
        if (jce_archive_debug_path(ar, i))
            has_debug_paths = true;
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
    json += header_verified ? "true,\n" : "false,\n";
    json += "    \"secure_index\": ";
    json += secure_index ? "true,\n" : "false,\n";
    json += "    \"authenticated\": ";
    json += authenticated ? "true,\n" : "false,\n";
    json += "    \"auth_verified\": ";
    json += auth_verified ? "true,\n" : "false,\n";
    json += "    \"has_debug_paths\": ";
    json += has_debug_paths ? "true\n" : "false\n";
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
    json += ", \"authenticated_count\": " +
            std::to_string(authenticated_count);
    json += ", \"plain_count\": " +
            std::to_string(count - encrypted_count);
    json += ", \"duplicate_groups\": 0, \"duplicate_entries\": 0";
    json += ", \"duplicate_wasted_bytes\": 0";
    json += ", \"duplicate_reclaimed_bytes\": 0, \"verify_ran\": true";
    json += ", \"verified_count\": " + std::to_string(verified_count);
    json += ", \"corrupt_count\": " + std::to_string(corrupt_count);
    json += ", \"skipped_count\": " + std::to_string(skipped_count) + "},\n";
    json += "  \"entries\": [";

    for (uint32_t i = 0; i < count; ++i) {
        const JceArchiveEntry *e = jce_archive_get(ar, i);
        if (!e)
            continue;
        double eratio = e->original_size
            ? (double)e->stored_size / (double)e->original_size : 0.0;
        char hbuf[32];
        char cbuf[16];
        char content_id_buf[17];
        char erbuf[64];
        std::snprintf(hbuf, sizeof(hbuf), "0x%016llx",
                      (unsigned long long)e->path_hash);
        std::snprintf(cbuf, sizeof(cbuf), "0x%08x",
                      (unsigned)e->content_crc);
        std::snprintf(content_id_buf, sizeof(content_id_buf), "%016llx",
                      (unsigned long long)entry_content_ids[i]);
        std::snprintf(erbuf, sizeof(erbuf), "%.6f", eratio);
        const char *p = jce_archive_debug_path(ar, i);
        if (!p && i < report_paths.size())
            p = report_paths[i];

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
        json += ", \"content_id\": \"" +
                std::string(content_id_buf) + "\"";
        json += ", \"flags\": " + std::to_string((unsigned)e->entry_flags);
        json += ", \"page_aligned\": ";
        json += (e->entry_flags & JCE_ARCHIVE_ENTRY_PAGE_ALIGNED) ? "true" : "false";
        json += ", \"encrypted\": ";
        json += (e->entry_flags & JCE_ARCHIVE_ENTRY_ENCRYPTED) ? "true" : "false";
        json += ", \"authenticated\": ";
        json += (e->entry_flags & JCE_ARCHIVE_ENTRY_AUTHENTICATED)
                    ? "true" : "false";
        json += ", \"duplicate\": false, \"verified\": ";
        if (entry_verified[i] > 0)
            json += "true}";
        else if (entry_verified[i] == 0)
            json += "false}";
        else
            json += "null}";
    }
    json += count ? "\n  ]\n}\n" : "]\n}\n";

    jce_archive_close(ar);
    const bool written = jce_fs_host_write_all(path.c_str(), json.data(),
                                               (uint64_t)json.size());
    const bool secure_verified = !verify_key ||
        (secure_index && authenticated && auth_verified &&
         encrypted_count == count && authenticated_count == count &&
         !has_debug_paths);
    return written && header_verified && secure_verified &&
           corrupt_count == 0 && skipped_count == 0;
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
    if (jce_binary_embed_write_coff(out_obj, "assets_pak_data",
                                    (const uint8_t *)pak_blob, pak_size,
                                    arch)) {
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
    if (jce_binary_embed_write_incbin(out_asm, "assets_pak_data",
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
    return jce_binary_embed_write_c_source(
        out_c, "assets_pak_data", (const uint8_t *)pak_blob, pak_size);
}

bool prepare_project_generated_assets(const std::string &project,
                                      const std::string &sdk,
                                      const std::string &cooked_rel,
                                      const std::string &bundles,
                                      const std::string &variant,
                                      const std::string &generated_root,
                                      const std::string &reports_dir,
                                      const std::string &arch,
                                      bool encrypt_assets,
                                      const uint8_t pak_key[32],
                                      std::string &out_assets_obj,
                                      std::string &out_assets_asm,
                                      std::string &out_assets_c,
                                      std::string &out_bom,
                                      std::string &out_pak_path,
                                      std::vector<std::string> &out_protected_paths,
                                      std::vector<ApprovedGraphAsset> &out_graph_assets,
                                      std::string &out_graph_snapshot,
                                      std::string &out_bundle_dir,
                                      std::string &out_pak_key_c)
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
    out_graph_assets.clear();
    out_graph_snapshot.clear();
    const std::string engine_res = sdk + PATH_SEP_CHR_LOCAL + "share" +
                                   PATH_SEP_CHR_LOCAL + "jce" +
                                   PATH_SEP_CHR_LOCAL + "engine_resources";
    const std::string engine_ui = sdk + PATH_SEP_CHR_LOCAL + "share" +
                                  PATH_SEP_CHR_LOCAL + "jce" +
                                  PATH_SEP_CHR_LOCAL + "engine_ui";
    const std::string cooked = cooked_rel.empty()
        ? std::string()
        : project + PATH_SEP_CHR_LOCAL + join_norm_sep(cooked_rel);

    if (!collect_assets_from_dir(engine_res, items, error, false) ||
        !collect_assets_from_dir(engine_ui, items, error, false)) {
        set_error("assets: " + error);
        return false;
    }
    if (variant == "dist") {
        if (!append_dist_bundle_graph(project, gen_dir, reports_dir, items,
                                      out_graph_assets,
                                      out_graph_snapshot, error)) {
            set_error("assets: " + error);
            return false;
        }
    } else if (!collect_assets_from_dir(cooked, items, error, true)) {
        set_error("assets: " + error);
        return false;
    }
    if (!append_runtime_boot_asset(project, items, error)) {
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
    out_pak_path = pak_path;
    out_protected_paths.clear();
    for (const ProjectAssetInput &item : items) {
        if (item.protect_path)
            out_protected_paths.push_back(item.vpath);
    }

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
    /* Release PAKs retain only normalized XXH3 path hashes. The external
     * BOM receives source paths directly, so auditability does not require
     * shipping a recoverable directory table in the executable. */
    cfg.emit_debug_paths = variant == "debug";
    cfg.compress_index = true;
    cfg.use_dict = true;
    cfg.dedup_content = true;
    /* Packaging > Encrypt Assets: compress-then-ChaCha20 every entry of the
     * embedded PAK ("project_assets" seeds the nonce salt).  NOTE this makes
     * the build depend on .jce/pak_key.hex — deterministic rebuilds require
     * the same key file. */
    if (encrypt_assets && pak_key) {
        cfg.encrypt        = true;
        cfg.encryption_key = pak_key;
        cfg.encrypt_label  = "project_assets";
    }

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
              write_asset_bom_json(out_bom, pak_path, pak_blob, pak_size,
                                   items, encrypt_assets ? pak_key : nullptr);
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
             (dict_count ? ("  dicts=" + std::to_string(dict_count)) : "") +
             (cfg.encrypt ? "  encrypted=yes" : ""));

    /* Asset-key delivery: emit jce_generated/jce_pak_key.c with two XOR
     * shares of the key (regenerated per build) so the runtime can
     * reconstruct + install it before the first PAK open.  When not
     * encrypting, remove any stale TU so an old key never ships. */
    out_pak_key_c.clear();
    {
        const std::string key_c = gen_dir + PATH_SEP_CHR_LOCAL + "jce_pak_key.c";
        if (encrypt_assets && pak_key) {
            std::string kerr;
            if (!jce_pak_key_write_shares_c(project, key_c, &kerr)) {
                set_error("assets: " + kerr);
                return false;
            }
            out_pak_key_c = key_c;
            log_line(JCE_CONSOLE_INFO,
                     "[build] assets: embedded key shares -> " + key_c);
        } else {
            jce_fs_host_remove_file(key_c.c_str());
        }
    }

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

        if (encrypt_assets) {
            JceArchive *archive = jce_archive_open(raw, (size_t)sz);
            if (archive)
                jce_archive_set_decryption_key(archive, pak_key);

            bool secure = archive && jce_archive_is_secure(archive) &&
                          jce_archive_is_authenticated(archive) &&
                          jce_archive_auth_status(archive) ==
                              JCE_ARCHIVE_AUTH_VALID &&
                          jce_archive_verify_header(archive);
            if (secure) {
                const uint32_t count = jce_archive_count(archive);
                for (uint32_t i = 0; i < count; ++i) {
                    const JceArchiveEntry *entry =
                        jce_archive_get(archive, i);
                    if (!entry ||
                        !(entry->entry_flags & JCE_ARCHIVE_ENTRY_ENCRYPTED) ||
                        !(entry->entry_flags &
                          JCE_ARCHIVE_ENTRY_AUTHENTICATED) ||
                        jce_archive_debug_path(archive, i)) {
                        secure = false;
                        break;
                    }
                }
            }
            if (archive)
                jce_archive_close(archive);
            if (!secure) {
                jce_fs_buffer_free(raw);
                set_error("bundles: Dist requires a keyed, authenticated "
                          "bundle built with this project's current PAK key: " +
                          bundle);
                return false;
            }
        }

        std::string sym = "bundle_" + sanitize_c_ident(basename_no_ext(bundle));
#if JCE_PLATFORM_WINDOWS
        std::string src = out_bundle_dir + PATH_SEP_CHR_LOCAL +
                          "_embed_bundle_" + sym + ".obj";
        bool embed_ok = jce_binary_embed_write_coff(
            src, sym, (const uint8_t *)raw, (size_t)sz, arch);
        jce_fs_host_remove_file((out_bundle_dir + PATH_SEP_CHR_LOCAL +
                                 "_embed_bundle_" + sym + ".c").c_str());
#else
        std::string src = out_bundle_dir + PATH_SEP_CHR_LOCAL +
                          "_embed_bundle_" + sym + ".S";
        bool embed_ok = jce_binary_embed_write_incbin(
            src, sym, bundle, (size_t)sz);
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

/* ---------------------------------------------------------------- *
 * Background cook/pack worker + completion handling.                 *
 * ---------------------------------------------------------------- */

bool asset_prep_active() { return g_pending.active; }

/* WORKER thread: run the whole cook/pack/embed.  All log/error output is
 * captured into the job's sink (see t_log_sink) and replayed on the main
 * thread by poll_asset_prep(). */
void asset_prep_worker(void *arg)
{
    PendingProjectBuild *p = (PendingProjectBuild *)arg;
    t_log_sink = &p->sink;
    p->ok = prepare_project_generated_assets(
        p->project, p->sdk, p->cooked, p->bundles, p->variant,
        p->intermediates_dir, p->reports_dir, p->arch,
        p->encrypt_assets, p->encrypt_assets ? p->pak_key : nullptr,
        p->out_assets_obj, p->out_assets_asm, p->out_assets_c,
        p->out_assets_bom, p->out_pak_path, p->protected_paths,
        p->graph_assets, p->out_graph_snapshot,
        p->out_bundle_dir, p->out_pak_key_c);
    t_log_sink = nullptr;
    jce_atomic_i32_store(p->done, 1);
}

/* MAIN thread: the cook succeeded — build the cmake configure/compile
 * queue + finish plan from the captured context and spawn the first step.
 * State stays RUNNING (set when the worker launched). */
void finalize_project_build_pipeline(PendingProjectBuild &p)
{
    auto append_configure = [&](std::string &a) {
        if (p.use_cmake_fresh)
            a += " --fresh";
        a += " -S " + qtok(p.project);
        a += " -B " + qtok(p.build_dir);
        a += " -G Ninja";
        a += " -DCMAKE_BUILD_TYPE=" + p.build_type;
        a += " -DJCE_BUILD_VARIANT=" + p.variant;
        a += " -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=" + qtok(p.output_dir);
        a += " -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=" + qtok(p.output_dir);
        a += " -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=" + qtok(p.archive_dir);
        a += " -DJCE_PROJECT_COOKED_ASSETS=" + qtok(p.cooked);
        a += " -DJCE_PROJECT_BUNDLES=" + qtok(p.bundles);
        a += " -U JCE_PROJECT_PREBUILT_ASSETS_OBJ";
        a += " -U JCE_PROJECT_PREBUILT_ASSETS_ASM";
        a += " -U JCE_PROJECT_PREBUILT_ASSETS_C";
        if (!p.out_assets_obj.empty())
            a += " -DJCE_PROJECT_PREBUILT_ASSETS_OBJ=" + qtok(p.out_assets_obj);
        if (!p.out_assets_asm.empty())
            a += " -DJCE_PROJECT_PREBUILT_ASSETS_ASM=" + qtok(p.out_assets_asm);
        if (!p.out_assets_c.empty())
            a += " -DJCE_PROJECT_PREBUILT_ASSETS_C=" + qtok(p.out_assets_c);
        a += " -DJCE_PROJECT_PREBUILT_BUNDLE_DIR=" + qtok(p.out_bundle_dir);
        /* Asset-encryption wiring: the generated key-shares TU (when
         * encrypting) and the loose-asset staging gate.  Both are pinned
         * with -D/-U every configure — populated cache vars otherwise leak
         * between variants (the build-dir contamination gotcha). */
        a += " -U JCE_PROJECT_PREBUILT_PAK_KEY_C";
        if (!p.out_pak_key_c.empty())
            a += " -DJCE_PROJECT_PREBUILT_PAK_KEY_C=" + qtok(p.out_pak_key_c);
        a += std::string(" -DJCE_PROJECT_STAGE_LOOSE_ASSETS=") +
             (p.encrypt_assets ? "OFF" : "ON");
        a += " -DJCE_DIR=" + qtok(p.cmake_dir);
    };

#if JCE_PLATFORM_WINDOWS
    /* The Windows SDK ships MSVC-built static libs, so force cl and
     * activate the MSVC environment inline via Microsoft's own
     * vcvarsall.bat.  Configure and build run in one cmd.exe /c chain so
     * the env survives between them. */
    std::string args = "/c ";
    if (!p.vcvars.empty()) {
        args += "call " + qtok(p.vcvars) + " " + p.vc_arch + " && ";
    } else {
        log_line(JCE_CONSOLE_WARNING,
                 "[build] MSVC vcvarsall.bat not found; assuming cl.exe is "
                 "already on PATH");
    }
    args += "cmake";
    append_configure(args);
    if (!p.c_compiler.empty())
        args += " -DCMAKE_C_COMPILER=" + qtok(p.c_compiler);
    else
        args += " -DCMAKE_C_COMPILER=cl";
    args += " && cmake --build " + qtok(p.build_dir) +
            " --target " + qtok(p.target);

    QueuedStep step;
    step.stage = JCE_BUILD_STAGE_COMPILE;
    step.exe   = "cmd.exe";
    step.args  = args;
    step.wd    = p.project;
    step.label = p.label;
    g_queue.push_back(step);
#else
    /* POSIX: two direct cmake invocations (configure, then build). */
    {
        QueuedStep configure;
        configure.stage = JCE_BUILD_STAGE_CONFIGURE;
        configure.exe   = "cmake";
        std::string a;
        append_configure(a);
        configure.args  = a;
        configure.wd    = p.project;
        configure.label = p.label;
        g_queue.push_back(configure);

        QueuedStep compile;
        compile.stage = JCE_BUILD_STAGE_COMPILE;
        compile.exe   = "cmake";
        compile.args  = "--build " + qtok(p.build_dir) +
                        " --target " + qtok(p.target);
        compile.wd    = p.project;
        compile.label = p.label;
        g_queue.push_back(compile);
    }
#endif

    /* Finish plan: always verify the artifact; stage a package when an
     * output directory was requested. */
    g_finish.verify        = true;
    g_finish.exe_name      = p.exe_name;
    g_finish.artifact_a    = p.output_dir + PATH_SEP_CHR_LOCAL + p.exe_name;
    g_finish.artifact_b    = p.build_dir + PATH_SEP_CHR_LOCAL + p.exe_name;
    g_finish.asset_bom_src = p.out_assets_bom;
    g_finish.dist          = p.variant == "dist";
    g_finish.pak_path      = p.out_pak_path;
    g_finish.protected_paths = p.protected_paths;
    g_finish.graph_assets = p.graph_assets;
    g_finish.graph_snapshot = p.out_graph_snapshot;
    if (p.encrypt_assets)
        std::memcpy(g_finish.pak_key, p.pak_key, sizeof(g_finish.pak_key));
    if (g_finish.dist) {
        g_finish.audit_report = p.reports_dir + PATH_SEP_CHR_LOCAL +
                                p.target + "_dist_audit.json";
    }
    if (p.encrypt_assets && !p.cooked.empty())
        g_finish.warn_loose_dir = p.output_dir + PATH_SEP_CHR_LOCAL +
                                  join_norm_sep(p.cooked);

    if (p.want_package) {
        g_finish.stage       = true;
        g_finish.out_dir     = p.package_out_dir;
        g_finish.cooked_rel  = p.cooked;
        /* Single self-contained exe by default (desktop): the project build
         * always embeds its PAK into the executable, and the runtime mounts
         * only that embedded blob — so the loose cooked tree beside the exe is
         * redundant (and, unencrypted, leaks plaintext assets).  Never stage
         * it on desktop; web/android use their own external-asset path. */
        g_finish.stage_loose = false;
        if (!p.cooked.empty())
            g_finish.cooked_src = p.project + PATH_SEP_CHR_LOCAL +
                                  join_norm_sep(p.cooked);

        std::string vt;
        vt += "name:     ";
        vt += (!p.app_name.empty()) ? p.app_name : p.target;
        vt += "\n";
        if (!p.app_version.empty()) {
            vt += "version:  "; vt += p.app_version; vt += "\n";
        }
        vt += "platform: "; vt += p.platform_tag; vt += "\n";
        vt += "arch:     "; vt += p.arch;         vt += "\n";
        vt += "variant:  "; vt += p.variant;      vt += "\n";
        if (p.encrypt_assets)
            vt += "assets:   encrypted (deterministic rebuilds require the "
                  "same .jce/pak_key.hex; key shares re-randomized per "
                  "build)\n";
        g_finish.version_text = vt;
    }

    log_line(JCE_CONSOLE_INFO,
             "[build] native project build: " + p.target +
             " (" + p.platform_tag + "/" + p.arch + "/" + p.variant +
             ")  sdk=" + p.sdk);

    /* Spawn the first queued step; poll_state() advances the rest. */
    QueuedStep first = g_queue.front();
    g_queue_pos = 1;
    if (!spawn_tool(first.stage, first.exe.c_str(), first.label.c_str(),
                    first.args.empty() ? nullptr : first.args.c_str(),
                    first.wd.empty() ? nullptr : first.wd.c_str())) {
        /* spawn_tool already set FAILED + last_error. */
        reset_pipeline();
    }
}

/* MAIN thread, per-frame: pick up a finished cook/pack and continue. */
void poll_asset_prep()
{
    if (!g_pending.active) return;
    if (!g_pending.done || jce_atomic_i32_load(g_pending.done) == 0)
        return;   /* worker still running */

    if (g_pending.thread) {
        jce_thread_join(g_pending.thread);
        g_pending.thread = nullptr;
    }

    /* Replay the worker's captured log/error lines now, on the thread
     * where the console + toast path is safe. */
    for (const DeferredLogEntry &e : g_pending.sink.entries)
        jce_editor_console_log_level(e.level, "%s", e.text.c_str());
    g_pending.sink.entries.clear();

    const bool cancelled =
        g_pending.cancel && jce_atomic_i32_load(g_pending.cancel) != 0;
    const bool ok = g_pending.ok;
    const std::string sink_error = g_pending.sink.last_error;

    if (g_pending.done)   { jce_atomic_i32_destroy(g_pending.done);   g_pending.done = nullptr; }
    if (g_pending.cancel) { jce_atomic_i32_destroy(g_pending.cancel); g_pending.cancel = nullptr; }

    if (cancelled) {
        g_pending.active   = false;
        g_build.state      = JCE_BUILD_FAILED;
        g_build.last_error = "build stopped before compile";
        log_line(JCE_CONSOLE_WARNING,
                 "[build] cook/pack finished; build stopped before compile");
        reset_pipeline();
        std::memset(g_pending.pak_key, 0, sizeof(g_pending.pak_key));
        return;
    }
    if (!ok) {
        g_pending.active   = false;
        g_build.state      = JCE_BUILD_FAILED;
        g_build.last_error = sink_error.empty()
            ? std::string("asset cook/pack failed") : sink_error;
        reset_pipeline();
        std::memset(g_pending.pak_key, 0, sizeof(g_pending.pak_key));
        return;
    }

    /* Success: build the cmake queue + spawn it (state stays RUNNING). */
    g_pending.active = false;
    finalize_project_build_pipeline(g_pending);
    std::memset(g_pending.pak_key, 0, sizeof(g_pending.pak_key));
}

/* MAIN thread, on editor shutdown: join any in-flight worker so it does
 * not write into freed state after teardown. */
void asset_prep_shutdown()
{
    if (!g_pending.active) return;
    if (g_pending.thread) {
        jce_thread_join(g_pending.thread);
        g_pending.thread = nullptr;
    }
    if (g_pending.done)   { jce_atomic_i32_destroy(g_pending.done);   g_pending.done = nullptr; }
    if (g_pending.cancel) { jce_atomic_i32_destroy(g_pending.cancel); g_pending.cancel = nullptr; }
    g_pending.sink.entries.clear();
    std::memset(g_pending.pak_key, 0, sizeof(g_pending.pak_key));
    g_pending.active = false;
}

} // namespace

bool jce_build_manager_start_project_build(const JceBuildProjectConfig *cfg)
{
    if (!cfg) { set_error("start_project_build: cfg is null"); return false; }
    if (g_build.process || asset_prep_active()) {
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

    /* Dist is the fail-closed shipping profile: authenticated encryption is
     * mandatory.  Project Settings may additionally enable it for release or
     * debug; debug remains inspectable unless explicitly opted in.  Resolve
     * the key on the main thread so the worker never touches editor state. */
    bool    encrypt_assets = false;
    uint8_t pak_key[32] = {0};
    {
        JceProjectSettings ps_local;
        const JceProjectSettings *ps = jce_project_settings_current();
        if (!ps) { jce_project_settings_load(&ps_local); ps = &ps_local; }
        const bool protection_required = variant == "dist" ||
            (ps->packaging.encrypt_assets &&
             (variant != "debug" || ps->packaging.encrypt_debug_builds));
        if (protection_required) {
            if (jce_pak_key_load(project, pak_key)) {
                encrypt_assets = true;
            } else {
                std::string kerr;
                if (jce_pak_key_generate(project, false, &kerr) &&
                    jce_pak_key_load(project, pak_key)) {
                    encrypt_assets = true;
                    log_line(JCE_CONSOLE_WARNING,
                             "[build] no asset key found — generated " +
                             jce_pak_key_path(project));
                } else {
                    set_error("packaging: secure assets are required but no "
                              "key is available (" + kerr + ")");
                    return false;
                }
            }
        }
    }

    /* The cook/pack/embed step (jce_archive_cook + file writes + embed
     * object generation) is the slow, UI-freezing part of a build, so it
     * runs on a worker thread.  Capture everything the cook needs AND
     * everything the post-cook cmake pipeline construction needs, then
     * launch the worker; poll_asset_prep() picks up the result on the
     * main thread and spawns cmake/ninja. */
    PendingProjectBuild &p = g_pending;
    p = PendingProjectBuild{};   /* clears any stale fields/pointers */
    p.project           = project;
    p.sdk               = sdk;
    p.cooked            = cooked;
    p.bundles           = bundles;
    p.variant           = variant;
    p.arch              = arch;
    p.intermediates_dir = intermediates_dir;
    p.reports_dir       = reports_dir;
    p.encrypt_assets    = encrypt_assets;
    if (encrypt_assets)
        memcpy(p.pak_key, pak_key, sizeof(p.pak_key));
    memset(pak_key, 0, sizeof(pak_key));
    p.build_dir         = build_dir;
    p.output_dir        = output_dir;
    p.archive_dir       = archive_dir;
    p.build_type        = build_type;
    p.cmake_dir         = cmake_dir;
    p.target            = target;
    p.label             = label;
    p.exe_name          = exe_name;
    p.platform_tag      = platform_tag;
    p.use_cmake_fresh   = use_cmake_fresh;
#if JCE_PLATFORM_WINDOWS
    p.vcvars     = vcvars;
    p.vc_arch    = vc_arch;
    p.c_compiler = c_compiler;
#endif
    p.want_package = (cfg->package_out_dir && cfg->package_out_dir[0]);
    if (p.want_package)
        p.package_out_dir = join_norm_sep(cfg->package_out_dir);
    p.app_name    = (cfg->app_name && cfg->app_name[0]) ? cfg->app_name : "";
    p.app_version = (cfg->app_version && cfg->app_version[0])
                        ? cfg->app_version : "";

    p.done   = jce_atomic_i32_create(0);
    p.cancel = jce_atomic_i32_create(0);
    p.active = true;

    /* Flip status to RUNNING up-front so the UI shows progress and any
     * re-entrant start_project_build / run_script call is refused while
     * the cook is in flight. */
    g_build.state     = JCE_BUILD_RUNNING;
    g_build.stage     = JCE_BUILD_STAGE_PREPARE_ASSETS;
    g_build.exit_code = 0;
    g_build.preset    = label;
    g_build.last_error.clear();

    log_line(JCE_CONSOLE_INFO,
             "[build] cooking + packing project assets in background…");

    p.thread = jce_thread_create(asset_prep_worker, &p, "jce_build_cook");
    if (!p.thread) {
        /* No worker thread available: run the cook inline then drive the
         * same completion path synchronously. */
        asset_prep_worker(&p);
        poll_asset_prep();
        return g_build.state != JCE_BUILD_FAILED;
    }
    return true;
}
