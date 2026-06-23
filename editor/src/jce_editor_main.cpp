/*
 * editor_main.cpp  JCE_Editor application entry point.
 *
 * Mirrors caged_kingdom/src/main.c: the engine owns SDL via the
 * Phase A jce_main.h firewall.  This TU only assembles the editor's
 * JceAppDesc; the actual SDL_App* callbacks live in jce_main_sdl.c
 * (compiled into the jce_application static library).
 */
#include <jce/application/jce_main.h>
#include <jce/os/core/jce_defs.h>   /* JCE_PLATFORM_WINDOWS */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#if JCE_PLATFORM_WINDOWS && defined(_DEBUG)
#include <crtdbg.h>   /* CRT-heap leak dump (debug builds only) */
#endif

extern "C" {
#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_engine.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/platform/jce_host_dialog.h>
#include <jce/os/platform/jce_window.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_scene_renderer.h>
}

#include "core/jce_editor.h"
#include "core/jce_editor_config.h"
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

/* ── Renderer backend override from editor-config.json ─────────────── */

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
    if (r.empty() || r == "auto")  return 0;  /* JCE_BACKEND_AUTO */
    if (r == "d3d11" || r == "direct3d11")  return 1;  /* JCE_BACKEND_D3D11 */
    if (r == "d3d12" || r == "direct3d12")  return 2;  /* JCE_BACKEND_D3D12 */
    if (r == "vulkan")                      return 3;  /* JCE_BACKEND_VULKAN */
    if (r == "opengl" || r == "gl")         return 4;  /* JCE_BACKEND_OPENGL */
    if (r == "opengl es" || r == "opengles" || r == "gles")
        return 5; /* JCE_BACKEND_OPENGLES*/
    if (r == "metal")
        return 6; /* JCE_BACKEND_METAL   */
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
    /* Detach dialogs before tearing down the window. */
    jce_host_dialog_set_parent_jce_window(NULL);
    jce_editor_game_render_shutdown();
    jce_editor_scene_render_shutdown();
    jce_editor_shutdown();
}

static void editor_app_update(float dt, void *ud)
{
    (void)dt;
    (void)ud;

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

    jce_state_play_mode_tick(real_dt);

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
    /* Scene rendering is triggered from inside the ImGui scene panel
     * (jce_editor_scene_render_frame) so it renders to the FBO at the
     * panel's actual size; ImGui then displays the texture. */
    maybe_log_startup_kpi();
    /* Advance the shared renderer's per-frame generation ONCE here, before the
     * Scene + Game viewport panels each render through it.  This lets the
     * skinned-animation sample + previous-frame TAA palette be produced once
     * (first viewport) and reused by the second, instead of being advanced
     * twice and clobbered to zero per-bone motion (which made animated
     * characters ghost in TAA). */
    jce_scene_renderer_begin_velocity_frame(jce_editor_get_scene_renderer());
    jce_editor_update(svc->window);
}

static void editor_app_event(const JceEvent *event, void *ud)
{
    (void)ud;

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
    return jce_editor_layout_is_quit_confirmed();
}

/* ── JCE entry point (engine owns SDL) ─────────────────────────────── */

/* Hot-reload all bgfx shader programs from disk.  Resolves dev_dir from:
   1. JCE_SHADER_DEV_DIR env var (highest priority)
   2. <exe_dir>/shaders relative path
   3. NULL (PAK-only — effectively a no-op reload)
   Defined here because we have access to the captured JceServices. */
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

    /* Apply renderer override before jce_engine_create() picks a backend. */
    configure_engine_renderer_from_editor_config();

    JceAppDesc desc = {};
    desc.name          = "JCE Editor";
    desc.maximized     = true;
    desc.window_width  = 1600;
    desc.window_height = 900;
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
