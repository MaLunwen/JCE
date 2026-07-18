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

#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_engine.h>
#include <jce/os/core/jce_allocator.h>   /* jce_alloc_hook_sdl (heap bridge) */

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
    /* Route SDL's heap into the engine allocator FIRST — before any engine
     * call gives SDL a chance to allocate — so SDL memory (surfaces, event
     * queue, audio buffers) becomes visible to jce_mem_stats and reclaimable
     * by the periodic jce_alloc_trim.  The hook refuses when SDL already
     * holds allocations (the main-callbacks shim may allocate on some
     * platforms) — SDL then simply stays on its own heap, never mixed.
     * JCE_NO_ALLOC_HOOKS=1 opts out entirely. */
    {
        const char *no_hooks = SDL_getenv("JCE_NO_ALLOC_HOOKS");
        if (!(no_hooks && no_hooks[0] && no_hooks[0] != '0'))
            (void)jce_alloc_hook_sdl();
    }
    JceAppDesc desc = jce_app_get_desc();
    jce_engine_set_app_desc(&desc);
    s_engine = jce_engine_create(argc, argv);
    return s_engine ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
}

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
