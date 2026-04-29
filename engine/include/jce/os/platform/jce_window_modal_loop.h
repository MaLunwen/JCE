/*
 * jce_window_modal_loop.h  Portable modal-loop tick hook.
 *
 * On Windows, holding the title bar / window border enters a modal
 * message loop inside DefWindowProc (WM_ENTERSIZEMOVE) during which
 * SDL_PollEvent never returns.  Any code relying on a frame loop
 * stalls — including bgfx_reset() in response to live-resize events.
 *
 * This module exposes a portable, platform-agnostic API to install a
 * "tick" callback that fires periodically while the OS is inside a
 * modal window loop.  On platforms without such a loop (Linux/macOS/
 * mobile) the install / uninstall calls are no-ops; the caller's
 * normal frame loop handles resizes naturally.
 *
 * Usage:
 *
 *     static void on_modal_tick(void *user) {
 *         JceEngine *e = user;
 *         jce_engine_iterate(e);
 *     }
 *     ...
 *     jce_window_install_modal_tick(on_modal_tick, engine);
 *     ...
 *     jce_window_uninstall_modal_tick();
 *
 * Layer: OS / Platform.
 */

#ifndef JCE_WINDOW_MODAL_LOOP_H
#define JCE_WINDOW_MODAL_LOOP_H


#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

/* Callback fired at ~60 Hz while the OS is inside a modal window loop.
   May be invoked on the main thread only; should be reentrancy-safe
   (the typical implementation guards against re-entering the frame
   pipeline using an atomic flag). */
typedef void (*JceModalTickFn)(void *user);

/* Install a modal-loop tick callback.  At most one callback may be
   installed at a time; calling install twice replaces the previous.
   On non-Windows platforms this is a documented no-op. */
JCE_API void jce_window_install_modal_tick(JceModalTickFn cb, void *user);

/* Uninstall the previously-installed callback (if any).  Safe to call
   even if nothing is installed.  Always safe at engine shutdown. */
JCE_API void jce_window_uninstall_modal_tick(void);

JCE_EXTERN_C_END

#endif /* JCE_WINDOW_MODAL_LOOP_H */
