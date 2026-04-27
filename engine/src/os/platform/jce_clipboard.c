/*
 * jce_clipboard.c  Clipboard wrapper (SDL3 backend).
 */

#include <jce/os/platform/jce_clipboard.h>

#include <SDL3/SDL_clipboard.h>

const char *jce_clipboard_get_text(void)
{
    /* SDL3 returns "" when the clipboard is empty (never NULL). */
    const char *s = SDL_GetClipboardText();
    return s ? s : "";
}

bool jce_clipboard_set_text(const char *text)
{
    if (!text) text = "";
    return SDL_SetClipboardText(text);
}
