/*
 * jce_clipboard.h  System clipboard text access.
 *
 * Wraps the platform clipboard so callers don't need to include
 * <SDL3/SDL_clipboard.h>.
 *
 * Layer: OS / Platform (Layer 2).
 */

#ifndef JCE_CLIPBOARD_H
#define JCE_CLIPBOARD_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* Returns the current clipboard text as a UTF-8 string.  The pointer
   is owned by the platform; the contents remain valid until the next
   call into this API. Returns "" (never NULL) when the clipboard is
   empty or not text. */
JCE_API const char *JCE_CALL jce_clipboard_get_text(void);

/* Set the clipboard to the given UTF-8 text.  Returns true on success. */
JCE_API bool JCE_CALL jce_clipboard_set_text(const char *text);

JCE_EXTERN_C_END

#endif /* JCE_CLIPBOARD_H */
