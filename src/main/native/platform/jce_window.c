/*
 * jce_window.c  Cross-platform SDL3 window management.
 */

#include "jce_window.h"
#include "core/jce_log.h"
#include <SDL3_image/SDL_image.h>
#include <string.h>

#define LOG_TAG "jce_window"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

struct JceWindow {
    SDL_Window *sdl_win;
    int         logical_w;
    int         logical_h;
    uint32_t    pixel_w;
    uint32_t    pixel_h;
};

JceWindow *jce_window_create(const JceWindowConfig *cfg)
{
    JceWindow *win = (JceWindow *)SDL_calloc(1, sizeof(*win));
    if (!win) return NULL;

    win->logical_w = cfg->logical_w;
    win->logical_h = cfg->logical_h;

    win->sdl_win = SDL_CreateWindow(cfg->title,
                                    cfg->logical_w, cfg->logical_h,
                                    cfg->flags);
    if (!win->sdl_win) {
        LOG_ERROR(LOG_TAG, "SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_free(win);
        return NULL;
    }

    /* Query actual pixel size (may differ from logical on HiDPI). */
    int pw, ph;
    SDL_GetWindowSizeInPixels(win->sdl_win, &pw, &ph);
    win->pixel_w = (uint32_t)pw;
    win->pixel_h = (uint32_t)ph;

    return win;
}

void jce_window_destroy(JceWindow *win)
{
    if (!win) return;
    if (win->sdl_win) SDL_DestroyWindow(win->sdl_win);
    SDL_free(win);
}

SDL_Window *jce_window_sdl(JceWindow *win)
{
    return win ? win->sdl_win : NULL;
}

void jce_window_get_size(JceWindow *win, uint32_t *w, uint32_t *h)
{
    if (w) *w = win ? win->pixel_w : 0;
    if (h) *h = win ? win->pixel_h : 0;
}

void jce_window_get_logical(JceWindow *win, int *w, int *h)
{
    if (w) *w = win ? win->logical_w : 0;
    if (h) *h = win ? win->logical_h : 0;
}

void jce_window_get_native(JceWindow *win, JceNativeWindow *out)
{
    if (!win || !out) return;
    memset(out, 0, sizeof(*out));

    SDL_PropertiesID props = SDL_GetWindowProperties(win->sdl_win);

#if defined(__ANDROID__)
    out->nwh = SDL_GetPointerProperty(props,
        SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);

#elif defined(__APPLE__)
  #if TARGET_OS_IPHONE
    out->nwh = SDL_GetPointerProperty(props,
        SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
  #else
    out->nwh = SDL_GetPointerProperty(props,
        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
  #endif

#elif defined(__EMSCRIPTEN__)
    /* bgfx's HTML5 GL context uses nwh as a CSS selector string.
       Must match the <canvas id="canvas"> in web-shell.html. */
    out->nwh = (void *)"#canvas";

#elif defined(_WIN32)
    out->nwh = SDL_GetPointerProperty(props,
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);

#elif defined(__linux__)
    /* Try Wayland first, fall back to X11. */
    void *wl_display = SDL_GetPointerProperty(props,
        SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
    if (wl_display) {
        out->ndt = wl_display;
        out->nwh = SDL_GetPointerProperty(props,
            SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
    } else {
        out->ndt = SDL_GetPointerProperty(props,
            SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
        out->nwh = (void *)(uintptr_t)SDL_GetNumberProperty(props,
            SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    }
#endif
}

void jce_window_handle_resize(JceWindow *win, uint32_t w, uint32_t h)
{
    if (!win) return;
    win->pixel_w = w;
    win->pixel_h = h;
    /* Keep logical height fixed, adapt width to new aspect ratio. */
    if (h > 0)
        win->logical_w = (int)((float)win->logical_h * (float)w / (float)h);
}

void jce_window_calc_viewport(JceWindow *win,
                              uint16_t *vp_x, uint16_t *vp_y,
                              uint16_t *vp_w, uint16_t *vp_h)
{
    if (!win) return;
    /* Full backbuffer  no letterboxing. */
    if (vp_x) *vp_x = 0;
    if (vp_y) *vp_y = 0;
    if (vp_w) *vp_w = (uint16_t)win->pixel_w;
    if (vp_h) *vp_h = (uint16_t)win->pixel_h;
}

void jce_window_set_icon(JceWindow *win, const void *data, size_t size)
{
    if (!win || !data || size == 0) return;

#ifdef __EMSCRIPTEN__
    /* SDL3's Emscripten SDL_SetWindowIcon uses MAIN_THREAD_EM_ASM with
       an `instanceof SharedArrayBuffer` guard that throws ReferenceError
       when the browser lacks cross-origin isolation.  Browser favicons
       should be set in the HTML shell instead. */
    (void)data; (void)size;
    return;
#endif

    SDL_IOStream *io = SDL_IOFromConstMem(data, size);
    if (!io) return;

    SDL_Surface *icon = IMG_Load_IO(io, true);  /* true = auto-close io */
    if (icon) {
        SDL_SetWindowIcon(win->sdl_win, icon);
        SDL_DestroySurface(icon);
    } else {
        LOG_WARN(LOG_TAG, "IMG_Load_IO failed: %s", SDL_GetError());
    }
}

void jce_window_toggle_fullscreen(JceWindow *win)
{
    if (!win || !win->sdl_win) return;

    bool is_fs = (SDL_GetWindowFlags(win->sdl_win) & SDL_WINDOW_FULLSCREEN) != 0;
    SDL_SetWindowFullscreen(win->sdl_win, !is_fs);

    /* Update pixel size + logical width immediately. */
    int pw, ph;
    SDL_GetWindowSizeInPixels(win->sdl_win, &pw, &ph);
    win->pixel_w = (uint32_t)pw;
    win->pixel_h = (uint32_t)ph;
    if (ph > 0)
        win->logical_w = (int)((float)win->logical_h * (float)pw / (float)ph);
}
