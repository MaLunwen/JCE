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
#include <jce/os/core/jce_log.h>
#include "application/jce_engine_terminal_internal.h"
#include "os/core/jce_memory.h"

/* SDL3 callback contract.  Must precede SDL_main.h. */
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

/* Application-supplied factory.  See <jce/app/jce_main.h>. */
extern JceAppDesc jce_app_get_desc(void);

typedef struct JceSdlHost {
    JceEngine *engine;
    JceAppResult terminal_result;
    uint64_t terminal_session;
    bool closing, finalizing;
} JceSdlHost;

/* SDL may dispatch pushed events on a foreign thread, including during Quit.
 * Engine callbacks already reject that thread. Do so before borrowing context;
 * SDL owns the atomic terminal result, not a worker write to host session state. */
static JceSdlHost *jce_sdl_host_context(void *appstate)
{
    return SDL_IsMainThread() ? (JceSdlHost *)appstate : NULL;
}

static void jce_sdl_terminal_ack(uint64_t session, JceAppResult result)
{
    (void)jce_engine_terminal_emit_internal(session, "sdl", result, true);
}

/* SDL must keep invoking Iterate until borrowed workers have released the
 * owner. A terminal event never skips this drain, including a later QUIT. */
static SDL_AppResult jce_sdl_defer_terminal(JceSdlHost *host, JceAppResult result)
{
    if (!host->closing || result == JCE_APP_FAILURE)
        host->terminal_result = result;
    host->closing = true;
    return host->engine ? SDL_APP_CONTINUE : (SDL_AppResult)host->terminal_result;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    if (!SDL_IsMainThread() || !appstate) return SDL_APP_FAILURE;
    if (*appstate)
        return jce_sdl_defer_terminal((JceSdlHost *)*appstate, JCE_APP_FAILURE);
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
    JceSdlHost *host = (JceSdlHost *)JCE_CALLOC(1, sizeof(*host));
    if (!host) return SDL_APP_FAILURE;
    *appstate = host;
    JceAppDesc desc = jce_app_get_desc();
    jce_engine_set_app_desc(&desc);
    const uint64_t before = jce_engine_terminal_cursor_internal();
    JceAppResult result = jce_engine_create_owned(argc, argv, &host->engine);
    host->terminal_session = jce_engine_terminal_after_create_internal(before);
    return result == JCE_APP_CONTINUE ? SDL_APP_CONTINUE
        : jce_sdl_defer_terminal(host, result);
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    JceSdlHost *host = jce_sdl_host_context(appstate);
    if (!host) return SDL_APP_FAILURE;
    if (host->closing)
        return host->engine ? SDL_APP_CONTINUE : (SDL_AppResult)host->terminal_result;
    JceAppResult result = jce_engine_event(host->engine, event);
    return result == JCE_APP_CONTINUE ? SDL_APP_CONTINUE
        : jce_sdl_defer_terminal(host, result);
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    JceSdlHost *host = jce_sdl_host_context(appstate);
    if (!host) return SDL_APP_FAILURE;
    if (host->closing) {
        /* One bounded cleanup attempt per callback, never an AppQuit spin.
         * Refusal retains the same owner and suppresses all gameplay. */
        if (host->engine) {
            SDL_Delay(10);
            if (!jce_engine_try_destroy(&host->engine)) return SDL_APP_CONTINUE;
        }
        return (SDL_AppResult)host->terminal_result;
    }
    JceAppResult result = jce_engine_iterate(host->engine);
    return result == JCE_APP_CONTINUE ? SDL_APP_CONTINUE
        : jce_sdl_defer_terminal(host, result);
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    JceSdlHost *host = jce_sdl_host_context(appstate);
    if (!host) return;
    if (result == SDL_APP_FAILURE) host->terminal_result = JCE_APP_FAILURE;
    if (host->finalizing) return;
    host->finalizing = true;
    host->closing = true;
    if (!jce_engine_try_destroy(&host->engine)) {
        /* SDL calls Quit once and drops appstate after return. Keep this block
         * and its Engine alive until process termination; do not free a borrowed
         * owner or invent a post-Quit recovery/terminal acknowledgement. */
        jce_log_write(JCE_LOG_LEVEL_ERROR, "sdl-host", __FILE__, __LINE__,
            "Host quit before engine cleanup completed; unresolved session retained until process termination");
        return;
    }
    const uint64_t session = host->terminal_session;
    const JceAppResult terminal_result = host->terminal_result == JCE_APP_FAILURE
        ? JCE_APP_FAILURE : JCE_APP_SUCCESS;
    /* This is SDL's final use of appstate, including immediate failed Init.
     * The process-lifetime Core heap remains valid after Engine/SDL shutdown. */
    JCE_FREE(host);
    jce_sdl_terminal_ack(session, terminal_result);
}
