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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
JceWindow  *jce_window_create(const JceWindowConfig *cfg);
void        jce_window_destroy(JceWindow *win);

/* Accessors. */
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

/* Call from window-resize handler. */
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

/* Enable/disable relative (captured) mouse mode for FPS-style controls. */
void        jce_window_set_relative_mouse_mode(JceWindow *win, bool enabled);

/* DPI scale factor: ratio of physical pixels to logical points.
   Returns 1.0 on standard displays, 2.0 on Retina/HiDPI, etc.
   Useful for scaling UI elements and touch targets. */
float       jce_window_get_dpi_scale(const JceWindow *win);

/* Start/stop SDL text input without exposing SDL_Window to consumers. */
bool        jce_window_start_text_input(JceWindow *win);
bool        jce_window_stop_text_input(JceWindow *win);

#ifdef __cplusplus
}
#endif

#endif /* JCE_WINDOW_H */
