/*
 * jce_cursor.h  Backend-neutral system mouse cursors.
 *
 * Wraps SDL3 cursor management so editor / tool code never includes
 * <SDL3/SDL_mouse.h> or <SDL3/SDL.h> just to change the cursor shape.
 *
 * Layer: OS / Platform (Layer 2).
 */

#ifndef JCE_CURSOR_H
#define JCE_CURSOR_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_CURSOR_ARROW = 0,
    JCE_CURSOR_TEXT,
    JCE_CURSOR_MOVE,
    JCE_CURSOR_NS_RESIZE,
    JCE_CURSOR_EW_RESIZE,
    JCE_CURSOR_NESW_RESIZE,
    JCE_CURSOR_NWSE_RESIZE,
    JCE_CURSOR_HAND,
    JCE_CURSOR_NOT_ALLOWED,
    JCE_CURSOR_COUNT
} JceCursorShape;

typedef struct JceCursor JceCursor;

/* Create (or fetch) a system cursor for the given shape.  Returns NULL
   if the platform does not provide a cursor for that shape; callers
   should fall back to JCE_CURSOR_ARROW. */
JCE_API JceCursor *jce_cursor_create_system(JceCursorShape shape);

/* Release a cursor previously returned by jce_cursor_create_system. */
JCE_API void JCE_CALL jce_cursor_destroy(JceCursor *cursor);

/* Make `cursor` the active OS cursor.  No-op on NULL. */
JCE_API void JCE_CALL jce_cursor_set(JceCursor *cursor);

/* Show or hide the OS cursor. */
JCE_API void JCE_CALL jce_cursor_show(bool visible);

JCE_EXTERN_C_END

#endif /* JCE_CURSOR_H */
