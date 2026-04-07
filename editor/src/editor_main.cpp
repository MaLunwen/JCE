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

extern "C" {
#include <jce/app/jce_engine.h>
#include <jce/app/jce_app_interface.h>
}
#include "jce_editor.h"
#include "jce_editor_panels.h"
#include "jce_editor_scene_render.h"

/* ── Editor state ──────────────────────────────────────────────────── */

static JceEngine *g_engine;

struct EditorState {
    const JceServices *svc;
};
static EditorState g_state;

/* ── JceAppDesc callbacks ──────────────────────────────────────────── */

static bool editor_app_init(const JceServices *svc, void *ud)
{
    EditorState *st = (EditorState *)ud;
    st->svc = svc;
    jce_editor_scene_render_init(svc->renderer);
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
