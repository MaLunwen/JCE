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
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <string>

extern "C" {
#include <jce/app/jce_engine.h>
#include <jce/app/jce_app_interface.h>
}
#include "jce_editor.h"
#include "jce_editor_panels.h"
#include "jce_editor_scene_render.h"
#include "jce_editor_config.h"

namespace fs = std::filesystem;

/* ── Editor state ──────────────────────────────────────────────────── */

static JceEngine *g_engine;

struct EditorState {
    const JceServices *svc;
};
static EditorState g_state;

/* ── Startup renderer backend override from editor config ─────────── */

static std::string to_lower_copy(const char *s)
{
    if (!s) return std::string();
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

static const char *renderer_name_to_backend_ini(const char *renderer_name)
{
    std::string r = to_lower_copy(renderer_name);
    if (r.empty()) return NULL;

    if (r == "auto")                         return "auto";
    if (r == "d3d12" || r == "direct3d12") return "d3d12";
    if (r == "d3d11" || r == "direct3d11") return "d3d11";
    if (r == "vulkan")                       return "vulkan";
    if (r == "metal")                        return "metal";
    if (r == "opengl" || r == "gl")        return "opengl";
    if (r == "opengl es" || r == "opengles" || r == "gles")
        return "opengles";

    return NULL;
}

static void configure_engine_renderer_from_editor_config(void)
{
    JceEditorConfig ecfg;
    if (!jce_editor_config_load(&ecfg))
        return;

    const char *backend = renderer_name_to_backend_ini(ecfg.renderer);
    if (!backend)
        return;

    std::error_code ec;
    fs::create_directories(".jce", ec);

    const char *override_path = ".jce/editor-engine.ini";
    std::ofstream out(override_path, std::ios::out | std::ios::trunc);
    if (!out.good()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "editor: failed to write %s, keeping default engine config resolution",
                    override_path);
        return;
    }

    out << "[renderer]\n";
    out << "backend = " << backend << "\n";
    out.close();

    jce_engine_set_config_path(override_path);
    SDL_Log("editor: renderer '%s' -> backend '%s' (config override: %s)",
            ecfg.renderer, backend, override_path);
}

/* ── JceAppDesc callbacks ──────────────────────────────────────────── */

static bool editor_app_init(const JceServices *svc, void *ud)
{
    EditorState *st = (EditorState *)ud;
    st->svc = svc;
    jce_editor_scene_render_init(svc->renderer, svc->pak);
    return jce_editor_init(svc->pak, svc->window);
}

static void editor_app_exit(void *ud)
{
    (void)ud;
    jce_editor_scene_render_shutdown();
    jce_editor_shutdown();
}

static void editor_app_update(float dt, void *ud)
{
    (void)dt; (void)ud;
}

static void editor_app_draw(const JceServices *svc, void *ud)
{
    (void)ud;

    /* Scene rendering is now triggered from inside the ImGui scene panel
     * (jce_editor_scene_render_frame) so it renders to the FBO with
     * the correct panel size. The ImGui panel then displays the texture. */

    jce_editor_update(svc->window);
}

static void editor_app_event(const SDL_Event *ev, void *ud)
{
    (void)ud;
    jce_editor_process_event(ev);
}

/* ── SDL3 callbacks ────────────────────────────────────────────────── */

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    (void)appstate;

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
    desc.user_data = &g_state;

    jce_engine_set_app_desc(&desc);
    g_engine = jce_engine_create(argc, argv);
    return g_engine ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    (void)appstate;
    return jce_engine_event(g_engine, event);
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    (void)appstate;
    return jce_engine_iterate(g_engine);
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    (void)appstate; (void)result;
    jce_engine_destroy(g_engine);
}
