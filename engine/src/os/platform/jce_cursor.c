/*
 * jce_cursor.c  System cursor wrapper (SDL3 backend).
 */

#include <jce/os/platform/jce_cursor.h>

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_surface.h>
#include <limits.h>

#include "os/core/jce_memory.h"

#include <stddef.h>

struct JceCursor {
    SDL_Cursor *handle;
};

static SDL_SystemCursor map_shape(JceCursorShape s)
{
    switch (s) {
    case JCE_CURSOR_ARROW:        return SDL_SYSTEM_CURSOR_DEFAULT;
    case JCE_CURSOR_TEXT:         return SDL_SYSTEM_CURSOR_TEXT;
    case JCE_CURSOR_MOVE:         return SDL_SYSTEM_CURSOR_MOVE;
    case JCE_CURSOR_NS_RESIZE:    return SDL_SYSTEM_CURSOR_NS_RESIZE;
    case JCE_CURSOR_EW_RESIZE:    return SDL_SYSTEM_CURSOR_EW_RESIZE;
    case JCE_CURSOR_NESW_RESIZE:  return SDL_SYSTEM_CURSOR_NESW_RESIZE;
    case JCE_CURSOR_NWSE_RESIZE:  return SDL_SYSTEM_CURSOR_NWSE_RESIZE;
    case JCE_CURSOR_HAND:         return SDL_SYSTEM_CURSOR_POINTER;
    case JCE_CURSOR_NOT_ALLOWED:  return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
    default:                      return SDL_SYSTEM_CURSOR_DEFAULT;
    }
}

JceCursor *jce_cursor_create_system(JceCursorShape shape)
{
    if (!(SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO)) return NULL;
    SDL_Cursor *h = SDL_CreateSystemCursor(map_shape(shape));
    if (!h) return NULL;
    JceCursor *c = (JceCursor *)JCE_MALLOC(sizeof *c);
    if (!c) { SDL_DestroyCursor(h); return NULL; }
    c->handle = h;
    return c;
}

JceCursor *jce_cursor_create_rgba8(const void *pixels, int width, int height,
                                  int hot_x, int hot_y)
{
    if (!(SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO)) return NULL;
    if (!pixels || width <= 0 || height <= 0 || width > INT_MAX / 4 ||
        hot_x < 0 || hot_y < 0 || hot_x >= width || hot_y >= height) return NULL;
    SDL_Surface *surface = SDL_CreateSurfaceFrom(width, height,
        SDL_PIXELFORMAT_RGBA32, (void *)pixels, width * 4);
    if (!surface) return NULL;
    SDL_Cursor *handle = SDL_CreateColorCursor(surface, hot_x, hot_y);
    SDL_DestroySurface(surface);
    if (!handle) return NULL;
    JceCursor *cursor = (JceCursor *)JCE_MALLOC(sizeof *cursor);
    if (!cursor) { SDL_DestroyCursor(handle); return NULL; }
    cursor->handle = handle;
    return cursor;
}

void jce_cursor_reset(void)
{
    if (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO)
        SDL_SetCursor(SDL_GetDefaultCursor());
}

void jce_cursor_destroy(JceCursor *cursor)
{
    if (!cursor) return;
    if (cursor->handle) SDL_DestroyCursor(cursor->handle);
    JCE_FREE(cursor);
}

void jce_cursor_set(JceCursor *cursor)
{
    if (cursor && cursor->handle) SDL_SetCursor(cursor->handle);
}

void jce_cursor_show(bool visible)
{
    if (visible) SDL_ShowCursor();
    else         SDL_HideCursor();
}
