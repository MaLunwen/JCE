/*
 * JCE  SDL3 callback entry point (C99, SDL_MAIN_USE_CALLBACKS).
 *
 * Pure SDL lifecycle: every callback delegates to jce_engine.
 * All subsystem creation, asset loading, and game logic live
 * in core/jce_engine and game/jce_app respectively.
 */

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "app/jce_engine.h"
#include "game/jce_app.h"

static JceEngine *g_engine;

SDL_AppResult SDL_AppInit(void **appstate, int argc,
                          char *argv[])
{
    (void)appstate;
    JceAppDesc desc = jce_app_get_desc();
    jce_engine_set_app_desc(&desc);
    g_engine = jce_engine_create(argc, argv);
    return g_engine ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
}

// cppcheck-suppress constParameterPointer   ; SDL3 callback signature is fixed
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
