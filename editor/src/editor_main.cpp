/*
 * editor_main.cpp  Standalone editor executable entry point.
 *
 * Uses the engine via JceAppDesc callbacks. The editor draws
 * ImGui on top of the engine's render loop. Engine code is
 * completely unmodified.
 */

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <string>
#include <cstdio>

extern "C" {
#include <jce/app/jce_engine.h>
#include <jce/app/jce_app_interface.h>
#include <jce/graphics/jce_postfx.h>
#include <jce/core/jce_allocator.h>
#include <jce/platform/jce_window.h>
}
#include "jce_editor.h"
#include "jce_editor_panels.h"
#include "scene/jce_editor_scene_render.h"
#include "jce_editor_config.h"
#include "jce_editor_state.h"
#include "jce_editor_layout.h"

/* ── Editor state ──────────────────────────────────────────────────── */

static JceEngine *g_engine;
static uint64_t g_startup_t0;
static bool g_startup_reported;

struct EditorState {
    const JceServices *svc;
};
static EditorState g_state;

static void maybe_log_startup_kpi(void)
{
    if (g_startup_reported || g_startup_t0 == 0) {
        return;
    }

    const uint64_t now = SDL_GetPerformanceCounter();
    const uint64_t freq = SDL_GetPerformanceFrequency();
    if (freq == 0) {
        return;
    }

    const double startup_ms = (double)(now - g_startup_t0) * 1000.0 / (double)freq;
    SDL_Log("kpi:startup_ms=%.3f", startup_ms);

    const char *startup_log_path = SDL_getenv("JCE_KPI_STARTUP_LOG");
    if (startup_log_path && startup_log_path[0]) {
        FILE *fp = fopen(startup_log_path, "a");
        if (fp) {
            fprintf(fp, "startup_ms,%.3f\n", startup_ms);
            fclose(fp);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "editor: failed to write startup KPI log to %s",
                        startup_log_path);
        }
    }

    g_startup_reported = true;
}

/* ── Startup renderer backend override from editor config ─────────── */

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
        return 5;  /* JCE_BACKEND_OPENGLES */
    if (r == "metal")                       return 6;  /* JCE_BACKEND_METAL */
    return -1;  /* unknown */
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
    SDL_Log("editor: renderer '%s' -> backend %d (direct override, no temp file)",
            ecfg.renderer, backend);
}

/* ── PostFX global (defined in jce_editor.cpp, used by panels) ───── */
extern JcePostFXPipeline *g_editor_postfx;

/* ── JceAppDesc callbacks ──────────────────────────────────────────── */

static bool editor_app_init(const JceServices *svc, void *ud)
{
    EditorState *st = (EditorState *)ud;
    st->svc = svc;
    jce_editor_scene_render_init(svc->renderer, svc->pak, svc->assets);
    if (!jce_editor_init(svc->pak, svc->window))
        return false;

    /* Create and load the PostFX pipeline. */
    if (!g_editor_postfx) {
        uint32_t w, h;
        jce_window_get_size(svc->window, &w, &h);
        g_editor_postfx = jce_postfx_create(jce_allocator_default(), w, h);
        if (g_editor_postfx) {
            jce_postfx_load_shaders(g_editor_postfx, svc->pak);
            JcePostFXParams p = jce_postfx_default_params();
            jce_postfx_set_params(g_editor_postfx, &p);
            jce_postfx_enable(g_editor_postfx, JCE_POSTFX_TONEMAP, false);
            jce_postfx_enable(g_editor_postfx, JCE_POSTFX_FXAA, true);
        }
    }

    return true;
}

static void editor_app_exit(void *ud)
{
    (void)ud;
    if (g_editor_postfx) {
        jce_postfx_destroy(g_editor_postfx);
        g_editor_postfx = NULL;
    }
    jce_editor_scene_render_shutdown();
    jce_editor_shutdown();
}

static uint64_t s_last_update_counter = 0;

static void editor_app_update(float dt, void *ud)
{
    (void)dt;
    (void)ud;

    /* Compute real delta time since engine passes 0.0f. */
    uint64_t now = SDL_GetPerformanceCounter();
    float real_dt = 0.0f;
    if (s_last_update_counter != 0) {
        real_dt = (float)(now - s_last_update_counter)
                / (float)SDL_GetPerformanceFrequency();
        if (real_dt > 0.1f) real_dt = 0.1f;  /* clamp to avoid spiral */
    }
    s_last_update_counter = now;

    /* Tick play-mode physics when playing. */
    jce_state_play_mode_tick(real_dt);
}

static void editor_app_draw(const JceServices *svc, void *ud)
{
    (void)ud;

    /* Scene rendering is now triggered from inside the ImGui scene panel
     * (jce_editor_scene_render_frame) so it renders to the FBO with
     * the correct panel size. The ImGui panel then displays the texture. */

    maybe_log_startup_kpi();
    jce_editor_update(svc->window);
}

static void editor_app_event(const void *ev, void *ud)
{
    (void)ud;
    const SDL_Event *event = (const SDL_Event *)ev;

    if (event->type == SDL_EVENT_QUIT
        && !jce_editor_layout_is_quit_confirmed())
    {
        jce_editor_layout_request_quit();
    }

    jce_editor_process_event(event);
}

/* ── SDL3 callbacks ────────────────────────────────────────────────── */

static bool editor_should_quit(void *user_data)
{
    (void)user_data;
    return jce_editor_layout_is_quit_confirmed();
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    (void)appstate;

    g_startup_t0 = SDL_GetPerformanceCounter();
    g_startup_reported = false;

    /* Let editor-config.json renderer drive backend selection at startup.
     * This avoids requiring users to manually edit executable-adjacent .config/jce.ini. */
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

    jce_engine_set_app_desc(&desc);
    g_engine = jce_engine_create(argc, argv);
    if (!g_engine)
        return SDL_APP_FAILURE;
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    (void)appstate;
    return (SDL_AppResult)jce_engine_event(g_engine, event);
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    (void)appstate;
    return (SDL_AppResult)jce_engine_iterate(g_engine);
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    (void)appstate; (void)result;
    jce_engine_destroy(g_engine);
}
