/*
 * jce_clipboard.c  Clipboard wrapper (SDL3 backend).
 */

#include <jce/os/platform/jce_clipboard.h>

#include <SDL3/SDL_clipboard.h>

const char *jce_clipboard_get_text(void)
{
    /* SDL3 transfers ownership of the returned buffer to the caller, who must
     * SDL_free() it. ImGui's GetClipboardTextFn treats the pointer as borrowed
     * (valid until the next call), so cache it and free the previous buffer on
     * each call — this bounds live clipboard buffers to one instead of leaking
     * one per read. SDL3 returns "" (never NULL) when the clipboard is empty. */
    static char *s_cached = NULL;
    if (s_cached) { SDL_free(s_cached); s_cached = NULL; }
    s_cached = SDL_GetClipboardText();
    return s_cached ? s_cached : "";
}

bool jce_clipboard_set_text(const char *text)
{
    if (!text) text = "";
    return SDL_SetClipboardText(text);
}
