/*
 * jce_window_internal.h  Engine-internal window accessors.
 *
 * Provides access to the underlying SDL_Window for engine subsystems
 * that need it (renderer, engine lifecycle).  Not exposed to game code.
 */

#ifndef JCE_WINDOW_INTERNAL_H
#define JCE_WINDOW_INTERNAL_H

#include <jce/os/platform/jce_window.h>
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return the underlying SDL_Window.  Engine-internal only. */
SDL_Window *jce_window_sdl(JceWindow *win);

#ifdef __cplusplus
}
#endif

#endif /* JCE_WINDOW_INTERNAL_H */
