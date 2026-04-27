/*
 * editor_main.cpp  JCE_Editor application entry point.
 *
 * Mirrors caged_kingdom/src/main.c: the engine owns SDL via the
 * Phase A jce_main.h firewall.  This TU only assembles the editor's
 * JceAppDesc; the actual SDL_App* callbacks live in jce_main_sdl.c
 * (compiled into the jce_application static library).
 */
#include <jce/application/jce_main.h>

#include <algorithm>
#include <cstdio>
#include <string>

extern "C" {
#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_engine.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/platform/jce_window.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_scene_renderer.h>
}

#include "core/jce_editor.h"
#include "core/jce_editor_config.h"
#include "core/jce_editor_state.h"
#include "core/jce_run_manager.h"
#include "scene/jce_editor_scene_render.h"
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
        FILE *fp = fopen(startup_log_path, "a");
        if (fp) {
            fprintf(fp, "startup_ms,%.3f\n", startup_ms);
            fclose(fp);
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
    jce_editor_scene_render_init(svc->renderer, svc->pak, svc->assets);
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

    jce_state_play_mode_tick(real_dt);
    jce_run_manager_poll();
}

static void editor_app_draw(const JceServices *svc, void *ud)
{
    (void)ud;
    /* Scene rendering is triggered from inside the ImGui scene panel
     * (jce_editor_scene_render_frame) so it renders to the FBO at the
     * panel's actual size; ImGui then displays the texture. */
    maybe_log_startup_kpi();
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

extern "C" JceAppDesc editor_app_get_desc(void)
{
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
