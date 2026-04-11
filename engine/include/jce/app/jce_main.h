/*
 * jce_main.h  Platform entry-point macro for JCE applications.
 *
 * Generates the SDL3 callback boilerplate so that game code
 * never needs to include SDL directly.
 *
 * Usage:
 *   #include <jce/app/jce_main.h>
 *   JCE_MAIN(my_app_get_desc)
 *
 * where `my_app_get_desc` is a function returning JceAppDesc.
 */

#ifndef JCE_MAIN_H
#define JCE_MAIN_H

#include <jce/app/jce_engine.h>
#include <jce/app/jce_app_interface.h>

/*
 * JCE_MAIN(get_desc_fn)
 *
 * Expands to the full SDL3 callback entry point implementation.
 * The argument must be a function: JceAppDesc get_desc_fn(void);
 *
 * Implementation detail: this macro includes SDL3 headers and
 * generates the four SDL_App* callbacks. The game source file
 * that uses this macro is the ONLY file that touches SDL.
 */
#define JCE_MAIN(get_desc_fn)                                           \
                                                                        \
/* SDL3 callback entry point plumbing. */                               \
    _Pragma("warning(push)")                                            \
    _Pragma("warning(disable:4115)")                                    \
                                                                        \
    /* Pull in SDL3 for the callback signatures. */                     \
    /* This include is confined to this single translation unit. */      \
    /* cppcheck-suppress misra-c2012-20.5 */                            \
    /* cppcheck-suppress misra-c2012-21.6 */                            \
    /* NOLINTBEGIN */                                                   \
                                                                        \
    /* The SDL_MAIN_USE_CALLBACKS contract requires these defines */     \
    /* before including SDL headers. */                                  \
                                                                        \
    _Pragma("warning(pop)")                                             \
                                                                        \
/* --- end preamble --- */

/* Because MSVC struggles with multi-line #define containing #define,
   we use a simpler approach: a pair of macros. */
#undef JCE_MAIN

/* The entry-point generation macro.
   Usage: put in exactly ONE .c file:
     #include <jce/app/jce_main.h>
     JCE_MAIN(my_get_desc)
*/
#define JCE_MAIN(get_desc_fn)                                           \
                                                                        \
static JceEngine *jce__g_engine;                                        \
                                                                        \
JceAppDesc get_desc_fn(void);  /* forward-declare */                    \
                                                                        \
JCE_MAIN_PLATFORM_INIT(get_desc_fn)                                    \
JCE_MAIN_PLATFORM_EVENT                                                 \
JCE_MAIN_PLATFORM_ITERATE                                               \
JCE_MAIN_PLATFORM_QUIT

/* ------------------------------------------------------------------- */
/* SDL3 callback backend                                               */
/* ------------------------------------------------------------------- */

/* These sub-macros are split out so each is a single statement.
   They rely on SDL3/SDL.h + SDL3/SDL_main.h being included in
   the translation unit that invokes JCE_MAIN, but we handle
   that via the JCE_MAIN_IMPL include below. */

#define JCE_MAIN_PLATFORM_INIT(get_desc_fn)                             \
JCE_MAIN_SDL_INCLUDE                                                    \
SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])      \
{                                                                       \
    (void)appstate;                                                     \
    JceAppDesc desc = get_desc_fn();                                    \
    jce_engine_set_app_desc(&desc);                                     \
    jce__g_engine = jce_engine_create(argc, argv);                      \
    return jce__g_engine ? SDL_APP_CONTINUE : SDL_APP_FAILURE;          \
}

/* cppcheck-suppress constParameterPointer ; SDL3 callback signature */
#define JCE_MAIN_PLATFORM_EVENT                                         \
SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)            \
{                                                                       \
    (void)appstate;                                                     \
    return (SDL_AppResult)jce_engine_event(jce__g_engine, event);       \
}

#define JCE_MAIN_PLATFORM_ITERATE                                       \
SDL_AppResult SDL_AppIterate(void *appstate)                            \
{                                                                       \
    (void)appstate;                                                     \
    return (SDL_AppResult)jce_engine_iterate(jce__g_engine);            \
}

#define JCE_MAIN_PLATFORM_QUIT                                          \
void SDL_AppQuit(void *appstate, SDL_AppResult result)                  \
{                                                                       \
    (void)appstate; (void)result;                                       \
    jce_engine_destroy(jce__g_engine);                                  \
}

/* This sub-macro brings in SDL only inside the .c that uses JCE_MAIN. */
#define JCE_MAIN_SDL_INCLUDE                                            \
    _Pragma("once")         /* no-op but harmless */

/* Actually, macros can't conditionally include headers.
   We handle this by requiring the game main.c to include SDL
   via this header.  Since jce_main.h is the ONLY include the
   game needs, the SDL leak is confined. */

/* Override: include SDL here so the game main.c doesn't have to. */
#ifndef SDL_MAIN_USE_CALLBACKS
#define SDL_MAIN_USE_CALLBACKS 1
#endif
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

/* Re-define the sub-macro now that SDL is included: */
#undef JCE_MAIN_SDL_INCLUDE
#define JCE_MAIN_SDL_INCLUDE /* SDL already included via jce_main.h */

#endif /* JCE_MAIN_H */
