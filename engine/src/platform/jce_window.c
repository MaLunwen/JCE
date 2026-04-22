/*
 * jce_window.c  Cross-platform SDL3 window management.
 */

#include <jce/platform/jce_window.h>
#include "jce_window_internal.h"
#include <jce/core/jce_log.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3/SDL_metal.h>
#include "core/jce_memory.h"
#include <string.h>

/* Verify JCE_WINDOW_* flags match SDL_WINDOW_* at compile time (C99-safe). */
#define JCE_SASSERT(cond, tag)  typedef char jce_sa_##tag[(cond) ? 1 : -1]
JCE_SASSERT(JCE_WINDOW_FULLSCREEN == SDL_WINDOW_FULLSCREEN, win_fs);
JCE_SASSERT(JCE_WINDOW_RESIZABLE  == SDL_WINDOW_RESIZABLE,  win_rs);
JCE_SASSERT(JCE_WINDOW_MAXIMIZED  == SDL_WINDOW_MAXIMIZED,  win_mx);
JCE_SASSERT(JCE_WINDOW_BORDERLESS == SDL_WINDOW_BORDERLESS, win_bl);
#undef JCE_SASSERT

#define LOG_TAG "jce_window"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

struct JceWindow {
    SDL_Window *sdl_win;
#if defined(__APPLE__)
    SDL_MetalView metal_view;
#endif
    int         logical_w;
    int         logical_h;
    uint32_t    pixel_w;
    uint32_t    pixel_h;
};

JceWindow *jce_window_create(const JceWindowConfig *cfg)
{
    JceWindow *win = (JceWindow *)JCE_CALLOC(1, sizeof(*win));
    if (!win) return NULL;

    win->logical_w = cfg->logical_w;
    win->logical_h = cfg->logical_h;

    uint64_t flags = cfg->flags;
#if defined(__APPLE__)
    /* Ensure Apple windows are Metal-capable for bgfx initialization. */
    flags |= SDL_WINDOW_METAL;
#endif

    win->sdl_win = SDL_CreateWindow(cfg->title,
                                    cfg->logical_w, cfg->logical_h,
                                    flags);
    if (!win->sdl_win) {
        LOG_ERROR(LOG_TAG, "SDL_CreateWindow failed: %s", SDL_GetError());
        JCE_FREE(win);
        return NULL;
    }

    /* Query actual pixel size (may differ from logical on HiDPI). */
    int pw, ph;
    SDL_GetWindowSizeInPixels(win->sdl_win, &pw, &ph);
    win->pixel_w = (uint32_t)pw;
    win->pixel_h = (uint32_t)ph;

#if defined(__APPLE__) && TARGET_OS_IPHONE
    /* On iOS use a 1:1 logical size to match device drawable pixels. */
    win->logical_w = pw;
    win->logical_h = ph;
#endif

#if defined(__APPLE__)
    win->metal_view = SDL_Metal_CreateView(win->sdl_win);
    if (!win->metal_view) {
        LOG_ERROR(LOG_TAG, "SDL_Metal_CreateView failed: %s", SDL_GetError());
        SDL_DestroyWindow(win->sdl_win);
        JCE_FREE(win);
        return NULL;
    }

    /* bgfx Metal backend expects CAMetalLayer* as native handle. */
    if (!SDL_Metal_GetLayer(win->metal_view)) {
        LOG_ERROR(LOG_TAG, "SDL_Metal_GetLayer failed: %s", SDL_GetError());
        SDL_Metal_DestroyView(win->metal_view);
        SDL_DestroyWindow(win->sdl_win);
        JCE_FREE(win);
        return NULL;
    }
#endif

    return win;
}

void jce_window_destroy(JceWindow *win)
{
    if (!win) return;
#if defined(__APPLE__)
    if (win->metal_view) SDL_Metal_DestroyView(win->metal_view);
#endif
    if (win->sdl_win) SDL_DestroyWindow(win->sdl_win);
    JCE_FREE(win);
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

void jce_window_get_native(const JceWindow *win, JceNativeWindow *out)
{
    if (!win || !out) return;
    memset(out, 0, sizeof(*out));

    // cppcheck-suppress unreadVariable   ; props used in all #if platform branches below
    SDL_PropertiesID props = SDL_GetWindowProperties(win->sdl_win);

#if defined(__ANDROID__)
    out->nwh = SDL_GetPointerProperty(props,
        SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);

#elif defined(__APPLE__)
    if (win->metal_view) {
        out->nwh = SDL_Metal_GetLayer(win->metal_view);
    } else {
  #if TARGET_OS_IPHONE
        out->nwh = SDL_GetPointerProperty(props,
            SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
  #else
        out->nwh = SDL_GetPointerProperty(props,
            SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
  #endif
    }

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
#if defined(__APPLE__) && TARGET_OS_IPHONE
    /* Keep logical size in sync with iOS drawable size. */
    win->logical_w = (int)w;
    win->logical_h = (int)h;
#else
    /* Keep logical height fixed, adapt width to new aspect ratio. */
    if (h > 0)
        win->logical_w = (int)((float)win->logical_h * (float)w / (float)h);
#endif
}

void jce_window_calc_viewport(const JceWindow *win,
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
    /* SDL3: passing NULL display mode = borderless desktop fullscreen.
     * The transition is async; do NOT call SDL_SyncWindow here — it pumps
     * events recursively which re-enters the resize watcher / render frame
     * and crashes. SDL will deliver SDL_EVENT_WINDOW_ENTER_FULLSCREEN /
     * PIXEL_SIZE_CHANGED on the next normal event pump and the watcher
     * picks it up safely. */
    SDL_SetWindowFullscreen(win->sdl_win, !is_fs);
    /* Do NOT proactively query SDL_GetWindowSizeInPixels here.
     * On Windows, SetWindowPos (called by SDL_SetWindowFullscreen) sends
     * WM_SIZE synchronously, which means GetClientRect already returns the
     * new fullscreen size before SDL_SetWindowFullscreen returns.  Updating
     * win->pixel_w/h to the new size while bgfx still has the old backbuffer
     * fools the reconcile check in jce_engine_iterate into seeing no mismatch
     * (both JceWindow and SDL report the new size), so bgfx_reset is never
     * called.  The result is view rects set for 1920x1080 in a 1280x720
     * backbuffer: old content in the top-left corner, rest black.
     *
     * Leave win->pixel_w/h at the old size.  The resize watcher (fired between
     * frames when s_in_render_frame==0) or the iterate reconcile check will
     * detect the SDL vs JceWindow mismatch and call jce_renderer_resize. */
}

bool jce_window_is_fullscreen(const JceWindow *win)
{
    if (!win || !win->sdl_win) return false;
    return (SDL_GetWindowFlags(win->sdl_win) & SDL_WINDOW_FULLSCREEN) != 0;
}

void jce_window_set_relative_mouse_mode(JceWindow *win, bool enabled)
{
    if (!win || !win->sdl_win) return;
    SDL_SetWindowRelativeMouseMode(win->sdl_win, enabled);
}

float jce_window_get_dpi_scale(const JceWindow *win)
{
    if (!win || !win->sdl_win) return 1.0f;

    /* SDL3: display content scale factor is the ratio between
       physical pixels and device-independent points.  This is
       the most reliable DPI indicator across platforms. */
    SDL_DisplayID display = SDL_GetDisplayForWindow(win->sdl_win);
    if (display) {
        float scale = SDL_GetDisplayContentScale(display);
        if (scale > 0.0f) return scale;
    }

    /* Fallback: derive from pixel/logical size ratio. */
    if (win->logical_w > 0 && win->pixel_w > 0) {
        float ratio = (float)win->pixel_w / (float)win->logical_w;
        if (ratio > 0.5f) return ratio;
    }

    return 1.0f;
}

bool jce_window_start_text_input(JceWindow *win)
{
    if (!win || !win->sdl_win) return false;
    return SDL_StartTextInput(win->sdl_win);
}

bool jce_window_stop_text_input(JceWindow *win)
{
    if (!win || !win->sdl_win) return false;
    return SDL_StopTextInput(win->sdl_win);
}
