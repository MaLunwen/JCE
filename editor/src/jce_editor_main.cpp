/*
 * editor_main.cpp  JCE_Editor application entry point.
 *
 * Mirrors caged_kingdom/src/main.c: the engine owns SDL via the
 * Phase A jce_main.h firewall.  This TU only assembles the editor's
 * JceAppDesc; the actual SDL_App* callbacks live in jce_main_sdl.c
 * (compiled into the jce_application static library).
 */
#include <jce/middleware/world/jce_environment.h>
#include "io/jce_editor_mesh_predecode.h"
#include "shadergraph/jce_shadergraph_shaderc.h"   /* one shaderc driver, one
                                                     * backend matrix */
#include "ui/jce_editor_panels.h"                  /* console log */
#include <jce/application/jce_main.h>
#include <jce/os/core/jce_defs.h>   /* JCE_PLATFORM_WINDOWS */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#if JCE_PLATFORM_WINDOWS && defined(_DEBUG)
#include <crtdbg.h>   /* CRT-heap leak dump (debug builds only) */
#endif

extern "C" {
#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_engine.h>
#include <jce/application/jce_runtime.h>
#include "core/jce_editor_assert_bridge.h"
#include "core/jce_editor_scene_file_watch.h"
#include "core/jce_editor_automation.h"

#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_sysinfo.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_trace.h>
#include <jce/os/platform/jce_host_dialog.h>
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_window.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/renderer/jce_scene_renderer.h>
}

#include "core/jce_editor.h"
#include "core/jce_editor_config.h"
#include "core/jce_editor_game_input_bridge.h"
#include "core/jce_editor_headless_build.h"
#include "core/jce_editor_state.h"
#include "core/jce_run_manager.h"
#include "core/jce_build_manager.h"
#include "core/jce_cook_manager.h"
#include "dialogs/jce_editor_dialogs.h"
#include "scene/jce_editor_scene_render.h"
#include "scene/jce_editor_game_render.h"
#include "scene/jce_asset_path_index.h"

extern "C" void jce_editor_register_builtin_modules(void);
#include "ui/jce_editor_layout.h"
#include "ui/jce_editor_panels.h"

/* ── Editor state ──────────────────────────────────────────────────── */

struct EditorState {
    const JceServices *svc;
};
static EditorState g_state;

static uint64_t g_startup_t0;
static bool g_startup_reported;
static uint64_t g_last_update_counter;
static bool g_headless_build_mode;

static void editor_shutdown_diag(const char *phase)
{
    const char *diag = getenv("JCE_SHUTDOWN_DIAG");
    if (!diag || !diag[0] || diag[0] == '0')
        return;
    LOG_INFO("editor-main", "shutdown: %s", phase);
    jce_log_flush();
}

static void play_commit_pointer(void *, float dx, float dy, float wheel,
                                uint32_t buttons)
{
    jce_editor_play_set_pointer_input(dx, dy, wheel, buttons);
}

static void play_commit_touches(void *, const JceRuntimeTouch *touches,
                                int count)
{
    jce_editor_play_set_touch_input(touches, count);
}

static void play_commit_actions(void *, const JceInputActions *actions)
{
    jce_editor_play_set_actions(actions);
}

static void play_commit_step(void *, float dt)
{
    jce_state_play_mode_tick(dt);
}

/* The engine's hardware-fed action map.  g_state is file-static here, so
 * panels that need pad input reach it through this accessor rather than a
 * second copy of the services pointer. */
const JceInputActions *jce_editor_engine_actions(void)
{
    return (g_state.svc && g_state.svc->actions) ? g_state.svc->actions
                                                 : nullptr;
}

/* The engine's live input system, same lifetime rule as the map above: `svc`
 * is stamped in editor_app_init(), so this answers from editor startup onward
 * and in EDIT mode -- it is NOT gated on Play.  The Input Manager's device
 * strip is the caller, and it must see a pad while the owner is authoring.
 * See the contract at the declaration for why this one is not const. */
JceInput *jce_editor_engine_input(void)
{
    return (g_state.svc && g_state.svc->input) ? g_state.svc->input
                                               : nullptr;
}

/* ── KPI: startup latency ──────────────────────────────────────────── */

static void maybe_log_startup_kpi(void)
{
    if (g_startup_reported || g_startup_t0 == 0)
        return;

    const uint64_t now = jce_time_perf_counter();
    const uint64_t freq = jce_time_perf_freq();
    if (freq == 0)
        return;

    const double startup_ms = (double)(now - g_startup_t0) * 1000.0 / (double)freq;
    fprintf(stderr, "kpi:startup_ms=%.3f\n", startup_ms);

    const char *startup_log_path = getenv("JCE_KPI_STARTUP_LOG");
    if (startup_log_path && startup_log_path[0]) {
        char line[64];
        int n = snprintf(line, sizeof(line), "startup_ms,%.3f\n", startup_ms);
        if (n > 0 && (size_t)n < sizeof(line)) {
            jce_fs_host_append(startup_log_path, line, (size_t)n);
        }
    }

    g_startup_reported = true;
}

/* Time to a FINISHED first frame, not to the first draw call.
 *
 * kpi:startup_ms is stamped at the TOP of the first editor_app_draw, so it
 * answers "how long until we began drawing".  Measured 2026-09-03 on
 * caged_kingdom/hidden_cove (1797 entities), that first draw then took another
 * 3.8 s on OpenGL and 3.7 s on D3D12 -- backend-independent, invisible in the
 * log, and larger than the reported startup itself.  A number that stops
 * before the expensive part is worse than no number: it says the editor is
 * ready while the window is still frozen. */
static bool g_first_frame_reported = false;

static void log_first_frame_kpi(void)
{
    if (g_first_frame_reported || g_startup_t0 == 0)
        return;
    const uint64_t freq = jce_time_perf_freq();
    if (freq == 0)
        return;
    g_first_frame_reported = true;
    jce_editor_mesh_predecode_report();
    fprintf(stderr, "kpi:first_frame_ms=%.3f\n",
            (double)(jce_time_perf_counter() - g_startup_t0) * 1000.0
                / (double)freq);
}

/* ── Renderer backend override from ~/.jce/editor-preferences.json ───
   (legacy ~/.jce/editor-config.json is a read-only fallback until it is
   retired to *.migrated — see jce_editor_config.cpp). */

static std::string to_lower_copy(const char *s)
{
    if (!s) return std::string();
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

static int renderer_name_to_backend_enum(const char *renderer_name)
{
    std::string r = to_lower_copy(renderer_name);
    if (r.empty() || r == "auto")                           return 0;  /* JCE_BACKEND_AUTO */
    if (r == "d3d11" || r == "direct3d11" || r == "dx11")   return 1;  /* JCE_BACKEND_D3D11 */
    if (r == "d3d12" || r == "direct3d12" || r == "dx12")   return 2;  /* JCE_BACKEND_D3D12 */
    if (r == "vulkan" || r == "vk")                         return 3;  /* JCE_BACKEND_VULKAN */
    if (r == "opengl" || r == "gl")                         return 4;  /* JCE_BACKEND_OPENGL */
    if (r == "opengl es" || r == "opengles" || r == "gles") return 5;  /* JCE_BACKEND_OPENGLES*/
    if (r == "metal")                                       return 6;  /* JCE_BACKEND_METAL   */
    return -1;
}

static void configure_engine_renderer_from_editor_config(void)
{
    JceEditorConfig ecfg;
    if (!jce_editor_config_load(&ecfg))
        return;

    int backend = renderer_name_to_backend_enum(ecfg.renderer);
    if (backend < 0)
        return;

    jce_engine_set_renderer_override(backend);
}

/* ── JceAppDesc callbacks ──────────────────────────────────────────── */

static bool editor_app_init(const JceServices *svc, void *ud)
{
    EditorState *st = (EditorState *)ud;
    st->svc = svc;

    /* BEFORE anything that could assert.  Without this the editor's default
     * for a broken engine invariant is abort(), which takes the user's
     * unsaved scene -- and the scene that provoked it -- with it. */
    jce_editor_assert_bridge_install();

    if (g_headless_build_mode) {
        jce_build_manager_init();
        return jce_editor_headless_build_initialize();
    }

    /* Editor authoring default: float the render pipeline to at least HIGH so the
     * stylized look (toon shading, 5-mip bloom, stylized sky dome, wrap/rim look
     * profile) is actually visible while authoring.  GPU-tier auto-detection is
     * conservative — it lands on MEDIUM for many D3D11 desktop GPUs, and MEDIUM/LOW
     * gate toon OFF, degrade bloom to 0 mips, and (at LOW) collapse the look profile
     * + sky to neutral/PREETHAM UPSTREAM of the postfx grade, so scenes render
     * grey/dim regardless of their authored look/postfx.  An ULTRA detection is left
     * untouched; the user can still pick any tier from the status bar (it re-applies).
     *
     * EXCEPT on a charter-baseline machine (512MB / single-core class): floating
     * to HIGH there would re-enable the full-res RGBA16F postfx family + 2048
     * shadow targets + the TAA RT chain that the whole-machine tier downgrade
     * exists to avoid — the editor must RUN on the weak box before it can look
     * pretty.  The status-bar tier picker still lets the user force HIGH
     * explicitly (and JCE_LOW_MEM=0/1 is the QA escape hatch). */
    {
        JceSysInfo si;
        jce_sysinfo_init(&si);
        bool charter_low = (si.ram_total_mb > 0 && si.ram_total_mb < 2048)
                        || si.cpu_cores <= 1;
        const char *lm = getenv("JCE_LOW_MEM");
        if (lm && lm[0]) charter_low = (lm[0] != '0');
        /* Do NOT float an INTEGRATED GPU up to HIGH: it has ample RAM/cores (so
         * charter_low is false) but a fraction of a discrete part's fill rate,
         * and HIGH re-enables 2048 shadows + the full RGBA16F postfx + TAA chain
         * that tanks its framerate — the same reason the whole-device auto-detect
         * now caps iGPUs at MEDIUM.  The editor must RUN smoothly on the weak GPU
         * before it looks pretty; the status-bar tier picker still forces HIGH
         * explicitly for a capable iGPU. */
        const bool has_discrete = jce_renderer_get_recommendation().has_discrete_gpu;
        if (!charter_low && has_discrete &&
            jce_renderer_get_tier() < JCE_GPU_TIER_HIGH)
            jce_renderer_set_tier_override(JCE_GPU_TIER_HIGH);
    }
    /* Re-apply the tier preset ONLY when engine boot fell back to it.
     * jce_render_pipeline_apply_boot (engine init) already honored the
     * project's Settings/RenderPipeline.rp.json when present; unconditionally
     * re-applying a tier preset here stomped that layer-3 asset and made the
     * editor preview diverge from the shipped game's layering.  There is no
     * "boot applied an asset" getter, so mirror apply_boot's own resolution:
     * host file exists AND parses. */
    {
        JceRenderPipelineDesc rpd;
        const char *rp_boot_path = "Settings/RenderPipeline.rp.json";
        const bool project_rp_applied =
            jce_fs_host_exists_file(rp_boot_path) &&
            jce_render_pipeline_load(rp_boot_path, &rpd);
        if (!project_rp_applied) {
            jce_render_pipeline_preset_for_current_tier(&rpd);
            jce_render_pipeline_apply(&rpd);
        }
    }

    /* ── Splash frame (G) ─────────────────────────────────────────────
       Submit a single themed-colour frame BEFORE the heavy init work
       (BRDF LUT, font atlas, i18n parse, demo scene). On first launch
       this turns the initial white "not responding" window into a dark
       window almost instantly, so the user sees something within a few
       ms instead of waiting for the whole startup pipeline to flush a
       frame. The actual bgfx clear lives behind jce_renderer's API to
       keep the editor TU free of backend-specific includes. */
    jce_renderer_present_splash(svc->renderer, svc->window, 0x1c1c1cff);

    jce_editor_scene_render_init(svc->renderer, svc->pak, svc->assets);
    jce_editor_game_render_init(svc->renderer, svc->window, svc->pak);

    /* Register editor built-in game modules (e.g. FPS Demo) so they
     * appear in the Game View "Module" dropdown. */
    jce_editor_register_builtin_modules();

    /* Anchor native dialogs to our window so SDL3's IFileDialog has a
       valid HWND owner.  Without this, SDL_ShowOpenFolderDialog can
       crash on the worker thread (heap corruption inside SDL3). */
    jce_host_dialog_set_parent_jce_window(svc->window);

    if (!jce_editor_init(svc->pak, svc->window))
        return false;

    /* Configure engine-owned PostFX defaults. The pipeline itself is
     * created/destroyed by the engine scene renderer. */
    JceSceneRenderer *sr = jce_editor_get_scene_renderer();
    JcePostFXPipeline *pfx = sr ? jce_scene_renderer_get_postfx(sr) : NULL;
    if (pfx) {
        JcePostFXParams p = jce_postfx_default_params();
        jce_postfx_set_params(pfx, &p);
        jce_postfx_enable(pfx, JCE_POSTFX_TONEMAP, false);
        jce_postfx_enable(pfx, JCE_POSTFX_FXAA, true);
    }

    return true;
}

static void editor_app_exit(void *ud)
{
    (void)ud;
    editor_shutdown_diag("application callback begin");
    if (g_headless_build_mode) {
        jce_editor_headless_build_shutdown();
        jce_build_manager_shutdown();
        editor_shutdown_diag("headless application stopped");
        return;
    }
    /* Detach dialogs before tearing down the window. */
    jce_host_dialog_set_parent_jce_window(NULL);
    editor_shutdown_diag("host dialogs detached");
    jce_editor_game_render_shutdown();
    editor_shutdown_diag("game renderer stopped");
    jce_editor_scene_render_shutdown();
    editor_shutdown_diag("scene renderer stopped");
    jce_editor_shutdown();
    editor_shutdown_diag("editor UI stopped");
}

static void editor_app_update(float dt, void *ud)
{
    (void)dt;
    (void)ud;

    if (g_headless_build_mode) {
        jce_build_manager_poll();
        jce_editor_headless_build_poll();
        return;
    }

    /* Engine passes 0.0f; compute a real delta from the perf counter. */
    uint64_t now = jce_time_perf_counter();
    float real_dt = 0.0f;
    if (g_last_update_counter != 0) {
        real_dt = (float)(now - g_last_update_counter) / (float)jce_time_perf_freq();
        if (real_dt > 0.1f)
            real_dt = 0.1f; /* clamp to avoid spiral */
    }
    g_last_update_counter = now;

    /* Drive a frame-sliced scene open (large-scene startup / open).  While
     * this is in flight the layout draws a modal "Loading…" overlay that
     * gates interaction, so the rest of the per-frame work below is safe to
     * run against a scene that is still being populated. */
    jce_state_scene_load_poll();
    /* ...and notice a scene file written by somebody else: an agent
     * working through the Automation API writes into the project and
     * then asks a human to look before the changeset is committed
     * (REQ-SCN-04).  Withheld while there are unsaved edits; see
     * core/jce_editor_scene_file_watch.h. */
    jce_editor_scene_file_watch_poll();

    if (jce_state_get_play_state() == JCE_PLAY_PLAYING) {
        JceRuntimeTouch touches[JCE_RUNTIME_MAX_TOUCHES] = {};
        int kept = 0;
        if (g_state.svc && g_state.svc->input) {
            int count = jce_input_touch_count(g_state.svc->input);
            if (count > JCE_RUNTIME_MAX_TOUCHES)
                count = JCE_RUNTIME_MAX_TOUCHES;
            for (int i = 0; i < count; ++i) {
                JceFingerID id = 0;
                float x = 0.0f;
                float y = 0.0f;
                float pressure = 0.0f;
                if (!jce_input_touch_get(g_state.svc->input, i, &id,
                                         &x, &y, &pressure)) {
                    continue;
                }
                touches[kept].id = static_cast<uint64_t>(id);
                touches[kept].x = x;
                touches[kept].y = y;
                touches[kept].pressure = pressure;
                ++kept;
            }
        }

        static const JceEditorGameInputCommitOps input_ops = {
            play_commit_pointer,
            play_commit_touches,
            play_commit_actions,
            play_commit_step,
        };
        (void)jce_editor_game_input_bridge_commit(
            jce_editor_game_input_bridge_shared(), touches, kept,
            g_state.svc ? g_state.svc->actions : nullptr,
            real_dt, &input_ops, nullptr);
    } else {
        if (jce_state_get_play_state() == JCE_PLAY_STOPPED) {
            jce_editor_game_input_bridge_reset(
                jce_editor_game_input_bridge_shared());
        }
        jce_state_play_mode_tick(real_dt);
    }

    /* Edit mode runs no simulation, so nothing calls jce_scene_update() and the
     * scene's environment would stop: a designer with time-of-day enabled would
     * see a frozen sky.  The panel used to advance its own copy, from ImGui's
     * frame time and only while its tab was visible; this advances THE clock,
     * whichever panels happen to be open.  While playing, the runtime's
     * jce_scene_update() is the driver and this must not double-advance. */
    /* STOPPED, not "not PLAYING".  Play has three states, and PAUSED is a
     * running session deliberately not stepping: advancing the environment
     * there would keep the sky moving behind a paused simulation, and move the
     * PLAY scene's clock while the thing that owns it is frozen.  Written as
     * != PLAYING first, which is the same bug the state enum exists to make
     * visible. */
    if (jce_state_get_play_state() == JCE_PLAY_STOPPED)
        jce_scene_environment_advance(jce_state_get_scene(), real_dt);
    jce_state_stress_move_tick(real_dt);   /* JCE_STRESS_MOVERS L2 soak (inert unless set) */

    /* VideoPlayer-as-texture and ParticleEmitter previews in Scene View while
     * editing.  In play mode the runtime (jce_runtime_step) drives these on the
     * same scene, so only pump here when STOPPED to avoid double-advance. */
    if (jce_state_get_play_state() == JCE_PLAY_STOPPED) {
        JceScene *scene = jce_state_get_scene();
        if (scene) {
            jce_scene_video_update(scene, (double)real_dt, NULL, NULL);
            jce_scene_particles_update(scene, real_dt);
        }
    }

    jce_run_manager_poll();
    /* Drive the cmake build subprocess every frame so save-hook
     * triggered repacks (which can run with no UI dialog open) still
     * stream their log lines to the console and detect completion. */
    jce_build_manager_poll();
    jce_cook_manager_poll();
    jce_state_scene_serial_poll();   /* async post-save mesh validation */
    /* Drive an in-flight Automation call: it is a subprocess and this
     * loop never blocks on one. See core/jce_editor_automation.h. */
    jce_editor_automation_poll();
    jce_asset_path_index_poll();     /* swap in a finished async reindex */

    /* Drain any folder/file dialog results enqueued by SDL worker threads.
     * Must run on the main thread before ImGui consumes the affected
     * static buffers (issue #5). */
    jce_editor_dialogs_pump_pending();

#ifndef NDEBUG
    /* Debug-only: every few seconds, dump engine-side (mimalloc) live-byte
     * totals + size buckets to stderr (→ VS Output).  CRT leak dumps and VS
     * native heap snapshots can't see mimalloc memory, so a steadily rising
     * "engine live" here means the leak is engine-side and shows its size
     * class.  Compiled out in release. */
    {
        static float s_mem_dump_accum = 0.0f;
        s_mem_dump_accum += real_dt;
        if (s_mem_dump_accum >= 3.0f) {
            s_mem_dump_accum = 0.0f;
            jce_alloc_track_dump();
        }
    }
#endif
}

static void editor_app_draw(const JceServices *svc, void *ud)
{
    (void)ud;
    if (g_headless_build_mode)
        return;
    /* Scene rendering is triggered from inside the ImGui scene panel
     * (jce_editor_scene_render_frame) so it renders to the FBO at the
     * panel's actual size; ImGui then displays the texture. */
    /* Once, before the first frame draws anything: let the decode workers
     * finish the scene's meshes.  They are decoding the exact files this frame
     * is about to ask for, and whichever ones it beats them to it decodes
     * again, serially, while the window is frozen. */
    if (!g_startup_reported)
        jce_editor_wait_for_mesh_predecode(8000u);
    maybe_log_startup_kpi();
    /* Advance the shared renderer's per-frame generation ONCE here, before the
     * Scene + Game viewport panels each render through it.  This lets the
     * skinned-animation sample + previous-frame TAA palette be produced once
     * (first viewport) and reused by the second, instead of being advanced
     * twice and clobbered to zero per-bone motion (which made animated
     * characters ghost in TAA). */
    jce_scene_renderer_begin_velocity_frame(jce_editor_get_scene_renderer());
    jce_editor_update(svc->window);
    log_first_frame_kpi();
}

static void editor_app_event(const JceEvent *event, void *ud)
{
    (void)ud;
    if (g_headless_build_mode)
        return;

    if (event->type == JCE_EVENT_QUIT
        && !jce_editor_layout_is_quit_confirmed())
    {
        jce_editor_layout_request_quit();
    }

    jce_editor_process_event(event);
}

static bool editor_should_quit(void *ud)
{
    (void)ud;
    return g_headless_build_mode
        ? jce_editor_headless_build_should_quit()
        : jce_editor_layout_is_quit_confirmed();
}

/* ── JCE entry point (engine owns SDL) ─────────────────────────────── */

/* Hot-reload all bgfx shader programs from disk.  Resolves dev_dir from:
   1. JCE_SHADER_DEV_DIR env var (highest priority)
   2. <exe_dir>/shaders relative path
   3. NULL (PAK-only — effectively a no-op reload)
   Defined here because we have access to the captured JceServices. */

/* ── Recompile stale shader source ──────────────────────────────────
 *
 * Two DIFFERENT roots are involved and they are easy to conflate, because one
 * environment variable has been used for both:
 *
 *   the SOURCE root .... holds engine/shaders/**\/(vs|fs)_<base>.sc
 *   the OUTPUT root .... holds <root>/shaders/(vs|fs)_<base>_<suffix>.bin,
 *                        which is what jce_renderer_reload_shaders_fs reads
 *
 * jce_sg::resolve_vs_pbr_path treats JCE_SHADER_DEV_DIR as the first;
 * jce_renderer_reload_shaders_fs treats its dev_dir as the second.  Both are
 * long-standing and both callers are right about their own meaning, so this
 * takes them SEPARATELY -- JCE_SHADER_SRC_DIR for source, falling back to
 * JCE_SHADER_DEV_DIR -- rather than picking a winner and silently breaking
 * whichever caller loses.
 */
namespace {

struct ScSweep {
    std::string out_root;      /* <root>/shaders/*.bin lives here */
    std::string include_dir;
    std::string varying_def;
    int         compiled  = 0;
    int         up_to_date = 0;
    int         failed    = 0;
};

bool sc_is_shader_source(const char *path, std::string *out_base, bool *out_vs)
{
    const char *slash = std::strrchr(path, '/');
    const char *bslash = std::strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    const char *leaf = slash ? slash + 1 : path;

    const size_t n = std::strlen(leaf);
    if (n < 7) return false;                       /* vs_x.sc */
    if (std::strcmp(leaf + n - 3, ".sc") != 0) return false;
    /* varying.def.sc is a declaration file, not a shader. */
    if (std::strstr(leaf, ".def.sc")) return false;

    if (std::strncmp(leaf, "vs_", 3) == 0)      *out_vs = true;
    else if (std::strncmp(leaf, "fs_", 3) == 0) *out_vs = false;
    else return false;                             /* cs_/template/include */

    out_base->assign(leaf + 3, n - 3 - 3);         /* strip "vs_" and ".sc" */
    return true;
}


/* The newest mtime in a shader's include closure.
 *
 * `#include "x.sh"` resolves beside the including file; `#include <x.sh>`
 * resolves in the shaderc include dir.  Anything that resolves to neither is
 * skipped rather than treated as an error: a missing include is shaderc's
 * problem to report, with its line number, not this sweep's to guess at.
 *
 * `seen` bounds the walk on the cyclic case and on the diamond, which
 * fs_pbr_decl.sh and fs_pbr_main.sh form through pbr_common.sh. */
void sc_newest_mtime(const std::string &path, const std::string &include_dir,
                     std::set<std::string> *seen, int64_t *newest, int depth)
{
    if (depth > 8) return;
    if (!seen->insert(path).second) return;

    int64_t mt = 0;
    if (jce_fs_host_get_mtime(path.c_str(), &mt) && mt > *newest) *newest = mt;

    uint64_t sz = 0;
    void *buf = jce_fs_host_read_all(path.c_str(), &sz);
    if (!buf) return;
    std::string src((const char *)buf, (size_t)sz);
    jce_fs_buffer_free(buf);

    std::string dir = path;
    size_t cut = dir.find_last_of("/\\");
    dir = (cut == std::string::npos) ? std::string(".") : dir.substr(0, cut);

    size_t at = 0;
    while ((at = src.find("#include", at)) != std::string::npos) {
        size_t q = src.find_first_of("\"<", at);
        size_t nl = src.find('\n', at);
        at += 8;
        if (q == std::string::npos || (nl != std::string::npos && q > nl))
            continue;
        const char close = (src[q] == '"') ? '"' : '>';
        size_t e = src.find(close, q + 1);
        if (e == std::string::npos) continue;
        const std::string name = src.substr(q + 1, e - q - 1);
        if (name.empty()) continue;

        const std::string local = dir + "/" + name;
        if (jce_fs_host_exists_file(local.c_str())) {
            sc_newest_mtime(local, include_dir, seen, newest, depth + 1);
        } else if (!include_dir.empty()) {
            const std::string sys = include_dir + "/" + name;
            if (jce_fs_host_exists_file(sys.c_str()))
                sc_newest_mtime(sys, include_dir, seen, newest, depth + 1);
        }
        at = e + 1;
    }
}

bool sc_walk_cb(const char *path, bool is_dir, void *user)
{
    if (is_dir) return true;
    ScSweep *sw = (ScSweep *)user;

    std::string base;
    bool is_vs = false;
    if (!sc_is_shader_source(path, &base, &is_vs)) return true;

    /* The whole include closure, not just this file: the PBR program lives in
     * fs_pbr_main.sh and fs_pbr.sc is eighteen lines of $input and one
     * #include.  Comparing only the .sc would recompile for the edit nobody
     * makes and stay silent for the edit everybody makes. */
    int64_t src_mtime = 0;
    {
        std::set<std::string> seen;
        sc_newest_mtime(path, sw->include_dir, &seen, &src_mtime, 0);
    }
    if (src_mtime <= 0) return true;

    int ntargets = 0;
    const jce_sg::GraphTarget *tg = jce_sg::graph_targets(&ntargets);
    for (int i = 0; i < ntargets; ++i) {
        char out[1024];
        std::snprintf(out, sizeof(out), "%s/shaders/%s_%s_%s.bin",
                      sw->out_root.c_str(), is_vs ? "vs" : "fs",
                      base.c_str(), tg[i].suffix);

        /* Only what this build actually produced: a profile the engine build
         * never emitted is not "stale", it is not part of this configuration,
         * and compiling it here would write a file nothing reads. */
        int64_t bin_mtime = 0;
        if (!jce_fs_host_get_mtime(out, &bin_mtime)) continue;
        if (bin_mtime >= src_mtime) { sw->up_to_date++; continue; }

        /* THE BUILD'S FLAGS, not the graph's.  tools/compile_shaders.cmake
         * passes no -O and defines BGFX_CONFIG_MAX_BONES=128; reproducing
         * both is what makes this a RELOAD rather than a silent recompile
         * with different settings.  Verified by compiling fs_pbr both ways:
         * these flags give 416168 bytes, which is byte-for-byte what the
         * engine build had already placed in the output directory. */
        jce_sg::CompileOpts co;
        co.pass_opt_flag = false;
        co.defines       = "BGFX_CONFIG_MAX_BONES=128";
        /* The shader's own directory, ahead of the bgfx ABI dir.  Without it
         * an #include resolved to whichever copy the shared dir happened to
         * hold -- and the SDK installs copies of these very files -- so the
         * sweep recompiled the SOURCE TREE's .sc against SOMEBODY ELSE'S
         * includes and wrote a binary that did not contain the edit. */
        {
            const char *sl = std::strrchr(path, '/');
            const char *bs = std::strrchr(path, '\\');
            if (bs && (!sl || bs > sl)) sl = bs;
            if (sl) co.extra_include.assign(path, (size_t)(sl - path));
        }
        jce_sg::ShadercResult r = jce_sg::compile_sc(
            path, sw->varying_def, sw->include_dir,
            is_vs ? jce_sg::ShaderKind::Vertex : jce_sg::ShaderKind::Fragment,
            tg[i].backend, co);
        if (!r.ok || r.blob.empty()) {
            sw->failed++;
            jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                "reload shaders: %s [%s] failed: %s",
                base.c_str(), tg[i].suffix, r.error.c_str());
            continue;
        }
        if (jce_fs_host_write_all(out, r.blob.data(), (uint64_t)r.blob.size()))
            sw->compiled++;
        else
            sw->failed++;
    }
    return true;
}

/* Returns false only when the sweep could not run at all. */
bool recompile_stale_shader_sources(const char *out_root)
{
    const char *src = std::getenv("JCE_SHADER_SRC_DIR");
    if (!src || !src[0]) src = std::getenv("JCE_SHADER_DEV_DIR");
    if (!src || !src[0]) return false;

    std::string include_dir = jce_sg::resolve_shader_include_dir();
    if (include_dir.empty()) {
        jce_editor_console_log_level(JCE_CONSOLE_WARNING,
            "reload shaders: no shader include dir "
            "(JCE_SHADERC_INCLUDE_DIR / BGFX_SHADER_INCLUDE_PATH); "
            "reloading the compiled blobs only");
        return false;
    }

    ScSweep sw;
    sw.out_root    = out_root ? out_root : "";
    sw.include_dir = include_dir;
    sw.varying_def = jce_sg::resolve_varying_def_path();

    char root[1024];
    std::snprintf(root, sizeof(root), "%s/shaders", src);
    if (!jce_fs_host_exists_dir(root))
        std::snprintf(root, sizeof(root), "%s", src);

    if (!jce_fs_host_walk(root, sc_walk_cb, &sw)) {
        jce_editor_console_log_level(JCE_CONSOLE_WARNING,
            "reload shaders: cannot walk %s", root);
        return false;
    }
    jce_editor_console_log(
        "reload shaders: %d recompiled, %d already current, %d failed (%s)",
        sw.compiled, sw.up_to_date, sw.failed, root);
    return true;
}

} /* namespace */

extern "C" bool jce_editor_reload_shaders(void)
{
    if (!g_state.svc || !g_state.svc->renderer || !g_state.svc->pak) {
        fprintf(stderr, "[editor] reload_shaders: services not ready\n");
        return false;
    }

    const char *dev_dir = getenv("JCE_SHADER_DEV_DIR");
    char inferred[1024];
    if (!dev_dir || !dev_dir[0]) {
        if (jce_fs_host_get_base_path(inferred, sizeof(inferred))) {
            /* base path returns trailing slash; strip it. */
            size_t n = strlen(inferred);
            if (n && (inferred[n-1] == '/' || inferred[n-1] == '\\'))
                inferred[n-1] = '\0';
            dev_dir = inferred;
        }
    }

    /* SOURCE FIRST.  This action used to re-read the compiled blobs, so
     * editing a .sc changed nothing until the engine was rebuilt outside the
     * editor -- the name promised source and the behaviour delivered output. */
    recompile_stale_shader_sources(dev_dir);

    bool ok = jce_renderer_reload_shaders_fs(g_state.svc->renderer,
                                             dev_dir,
                                             g_state.svc->pak);
    fprintf(stderr, "[editor] reload_shaders dev_dir=%s -> %s\n",
            dev_dir ? dev_dir : "(none)", ok ? "ok" : "FAILED");
    return ok;
}

extern "C" JceAppDesc editor_app_get_desc(void)
{
#if JCE_PLATFORM_WINDOWS && defined(_DEBUG)
    /* Debug-only leak diagnostics: at process exit, dump CRT-heap allocations
     * that were never freed (editor C++ / STL / ImGui) to the VS Output window.
     * NOTE: engine-side allocations go through mimalloc (jce_alloc), NOT the CRT
     * heap, so they will NOT appear here — run with the env var
     * MIMALLOC_SHOW_STATS=1 to see mimalloc's alloc/free totals on exit.
     * Each leaked block prints its allocation number {N}; to get its call
     * stack, on a later run set _crtBreakAlloc=N (or _CrtSetBreakAlloc(N)) so
     * the debugger breaks at that allocation. */
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
    g_startup_t0 = jce_time_perf_counter();
    g_startup_reported = false;
    jce_trace_set_enabled(true);
    jce_trace_thread_register("MAIN");

    /* Apply renderer override before jce_engine_create() picks a backend. */
    g_headless_build_mode =
        jce_editor_headless_build_environment_present();
    if (!g_headless_build_mode)
        configure_engine_renderer_from_editor_config();

    JceAppDesc desc = {};
    desc.name          = "JCE Editor";
    desc.maximized     = true;
    desc.window_width  = 1600;
    desc.window_height = 900;
    desc.headless   = g_headless_build_mode;
    desc.init      = editor_app_init;
    desc.exit      = editor_app_exit;
    desc.update    = editor_app_update;
    desc.draw      = editor_app_draw;
    desc.on_event  = editor_app_event;
    desc.should_quit = editor_should_quit;
    desc.user_data = &g_state;
    return desc;
}

JCE_MAIN(editor_app_get_desc)
