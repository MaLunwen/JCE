/*
 * jce_main_sdl.c  Private SDL3 entry-point backend for JCE applications.
 *
 * This translation unit is the ONLY place SDL_main is included.  It
 * implements the four SDL_App* callbacks required by
 * SDL_MAIN_USE_CALLBACKS and routes them into the SDL-free public
 * engine API (jce_engine_create / jce_engine_event /
 * jce_engine_iterate / jce_engine_destroy).
 *
 * Application/game code must define a single C function with the
 * signature:
 *
 *     JceAppDesc jce_app_get_desc(void);
 *
 * The convenience macro JCE_MAIN(my_factory) in <jce/app/jce_main.h>
 * generates that definition by forwarding to a user-named factory.
 *
 * Linkage notes:
 *   - SDL_main.h provides main()/WinMain in this TU; static-lib link
 *     pulls the TU into the executable because the C runtime startup
 *     looks up the main symbol.
 *   - The engine library remains usable as a host for non-SDL entry
 *     paths (e.g., the JNI bridge in jce_jni_bridge.c) — those paths
 *     do not link this TU because their executables provide their own
 *     entry points.
 */

#include <jce/application/jce_engine.h>
#include <jce/application/jce_app_interface.h>

/* SDL3 callback contract.  Must precede SDL_main.h. */
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

/* Application-supplied factory.  See <jce/app/jce_main.h>. */
extern JceAppDesc jce_app_get_desc(void);

static JceEngine *s_engine;

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    (void)appstate;
    JceAppDesc desc = jce_app_get_desc();
    jce_engine_set_app_desc(&desc);
    s_engine = jce_engine_create(argc, argv);
    return s_engine ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
}

/* cppcheck-suppress constParameterPointer ; SDL3 callback signature */
SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    (void)appstate;
    return (SDL_AppResult)jce_engine_event(s_engine, event);
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    (void)appstate;
    return (SDL_AppResult)jce_engine_iterate(s_engine);
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    (void)appstate;
    (void)result;
    jce_engine_destroy(s_engine);
    s_engine = NULL;
}
