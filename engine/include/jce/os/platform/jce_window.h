/*
 * jce_window.h  Cross-platform window management for JCE.
 *
 * Wraps window creation, native handle retrieval (Win32, macOS,
 * iOS, Android, X11, Wayland), and letterbox viewport calculation.
 *
 * This header is SDL-free; game/application code does not need SDL.
 */

#ifndef JCE_WINDOW_H
#define JCE_WINDOW_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ----- Window creation flags ----------------------------------------- */
/* Values match SDL_WINDOW_* so no conversion is needed internally.      */

#define JCE_WINDOW_FULLSCREEN          0x00000001u
#define JCE_WINDOW_BORDERLESS          0x00000010u
#define JCE_WINDOW_RESIZABLE           0x00000020u
#define JCE_WINDOW_MINIMIZED           0x00000040u
#define JCE_WINDOW_MAXIMIZED           0x00000080u
#define JCE_WINDOW_HIGH_PIXEL_DENSITY  0x00002000u

typedef struct JceWindowConfig {
    const char *title;
    int         logical_w;
    int         logical_h;
    uint32_t    flags;          /* JCE_WINDOW_* */
} JceWindowConfig;

typedef struct JceWindow JceWindow;

/* Create / destroy. */
JCE_API JceWindow  *jce_window_create(const JceWindowConfig *cfg);
JCE_API void        jce_window_destroy(JceWindow *win);

/* Accessors. */
JCE_API void        jce_window_get_size(JceWindow *win, uint32_t *w, uint32_t *h);
JCE_API void        jce_window_get_logical(JceWindow *win, int *w, int *h);

/* Update the window-manager title (UTF-8). No-op when win or title is NULL. */
JCE_API void        jce_window_set_title(JceWindow *win, const char *title);

/* Native window handle info for renderer backends. */
typedef struct JceNativeWindow {
    void *nwh;    /* native window handle */
    void *ndt;    /* native display type (X11/Wayland, NULL elsewhere) */
} JceNativeWindow;

/* Retrieve native window handles.
   Supports Win32, macOS/Cocoa, iOS/UIKit, Android, X11, Wayland. */
JCE_API void        jce_window_get_native(const JceWindow *win, JceNativeWindow *out);

/* Call from window-resize handler. */
JCE_API void        jce_window_handle_resize(JceWindow *win, uint32_t w, uint32_t h);

/* Compute a letterbox viewport that preserves the logical aspect ratio. */
void        jce_window_calc_viewport(const JceWindow *win,
                                     uint16_t *vp_x, uint16_t *vp_y,
                                     uint16_t *vp_w, uint16_t *vp_h);

/* Set window icon from in-memory PNG/image data (e.g. decompressed from PAK). */
void        jce_window_set_icon(JceWindow *win,
                                const void *data, size_t size);

/* Toggle between fullscreen and windowed mode. */
JCE_API void        jce_window_toggle_fullscreen(JceWindow *win);

/* Query whether the window is currently fullscreen. */
JCE_API bool        jce_window_is_fullscreen(const JceWindow *win);

/* Enable/disable relative (captured) mouse mode for FPS-style controls. */
JCE_API void        jce_window_set_relative_mouse_mode(JceWindow *win, bool enabled);

/* Force the OS cursor to stay inside the window (in addition to relative
 * mode).  Use as a hard guard when embedding game viewports inside larger
 * editor windows so the cursor cannot escape to other applications. */
JCE_API void        jce_window_set_mouse_grab(JceWindow *win, bool enabled);

/* Warp the OS cursor to (x, y) in window-local pixel coordinates.  Useful
 * to re-center the cursor each frame to defend against cursor drifting
 * outside an embedded viewport while in FPS capture. */
JCE_API void        jce_window_warp_mouse(JceWindow *win, int x, int y);

/* Confine the OS cursor to the rectangle (x, y, w, h) given in window-local
 * pixel coordinates, even when the window has focus.  Pass w<=0 or h<=0 to
 * clear the constraint and let the cursor roam the whole window again.
 * Use to lock the cursor inside an embedded sub-viewport (e.g. Game View). */
JCE_API void        jce_window_set_mouse_rect(JceWindow *win,
                                              int x, int y, int w, int h);

/* DPI scale factor: ratio of physical pixels to logical points.
   Returns 1.0 on standard displays, 2.0 on Retina/HiDPI, etc.
   Useful for scaling UI elements and touch targets. */
JCE_API float       jce_window_get_dpi_scale(const JceWindow *win);

/* Start/stop SDL text input without exposing SDL_Window to consumers. */
JCE_API bool        jce_window_start_text_input(JceWindow *win);
JCE_API bool        jce_window_stop_text_input(JceWindow *win);

JCE_EXTERN_C_END

#endif /* JCE_WINDOW_H */
