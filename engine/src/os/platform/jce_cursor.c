/*
 * jce_cursor.c  System cursor wrapper (SDL3 backend).
 */

#include <jce/os/platform/jce_cursor.h>

#include <SDL3/SDL_mouse.h>

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
    SDL_Cursor *h = SDL_CreateSystemCursor(map_shape(shape));
    if (!h) return NULL;
    JceCursor *c = (JceCursor *)JCE_MALLOC(sizeof *c);
    if (!c) { SDL_DestroyCursor(h); return NULL; }
    c->handle = h;
    return c;
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
