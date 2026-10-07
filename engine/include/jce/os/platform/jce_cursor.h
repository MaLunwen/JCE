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

/* Create a colour cursor from tightly packed RGBA8 pixels. The platform copies
   the pixels; the caller may release them after this call. Dimensions and the
   hotspot must be valid, otherwise returns NULL. No SDL types cross this API. */
JCE_API JceCursor *JCE_CALL jce_cursor_create_rgba8(const void *pixels,
                                                  int width, int height,
                                                  int hot_x, int hot_y);

/* Restore the platform default cursor (for tools leaving an embedded game). */
JCE_API void JCE_CALL jce_cursor_reset(void);

/* Release a cursor previously returned by either create function. */
JCE_API void JCE_CALL jce_cursor_destroy(JceCursor *cursor);

/* Make `cursor` the active OS cursor.  No-op on NULL. */
JCE_API void JCE_CALL jce_cursor_set(JceCursor *cursor);

/* Show or hide the OS cursor. */
JCE_API void JCE_CALL jce_cursor_show(bool visible);

JCE_EXTERN_C_END

#endif /* JCE_CURSOR_H */
