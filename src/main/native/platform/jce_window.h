/*
 * jce_window.h  Cross-platform SDL3 window management for JCE.
 *
 * Wraps SDL_Window creation, native handle retrieval (Win32, macOS,
 * iOS, Android, X11, Wayland), and letterbox viewport calculation.
 */

#ifndef JCE_WINDOW_H
#define JCE_WINDOW_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct JceWindowConfig {
    const char *title;
    int         logical_w;
    int         logical_h;
    uint32_t    flags;          /* SDL_WINDOW_* */
} JceWindowConfig;

typedef struct JceWindow JceWindow;

/* Create / destroy. */
JceWindow  *jce_window_create(const JceWindowConfig *cfg);
void        jce_window_destroy(JceWindow *win);

/* Accessors. */
SDL_Window *jce_window_sdl(JceWindow *win);
void        jce_window_get_size(JceWindow *win, uint32_t *w, uint32_t *h);
void        jce_window_get_logical(JceWindow *win, int *w, int *h);

/* Native window handle info for renderer backends. */
typedef struct JceNativeWindow {
    void *nwh;    /* native window handle */
    void *ndt;    /* native display type (X11/Wayland, NULL elsewhere) */
} JceNativeWindow;

/* Retrieve native window handles.
   Supports Win32, macOS/Cocoa, iOS/UIKit, Android, X11, Wayland. */
void        jce_window_get_native(const JceWindow *win, JceNativeWindow *out);

/* Call from SDL_EVENT_WINDOW_RESIZED handler. */
void        jce_window_handle_resize(JceWindow *win, uint32_t w, uint32_t h);

/* Compute a letterbox viewport that preserves the logical aspect ratio. */
void        jce_window_calc_viewport(const JceWindow *win,
                                     uint16_t *vp_x, uint16_t *vp_y,
                                     uint16_t *vp_w, uint16_t *vp_h);

/* Set window icon from in-memory PNG/image data (e.g. decompressed from PAK). */
void        jce_window_set_icon(JceWindow *win,
                                const void *data, size_t size);

/* Toggle between fullscreen and windowed mode. */
void        jce_window_toggle_fullscreen(JceWindow *win);

#endif /* JCE_WINDOW_H */
