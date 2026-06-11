/*
 * jce_host_locale.c  Host preferred-locale query (SDL3 backend).
 *
 * SDL_GetPreferredLocales returns a NULL-terminated array of SDL_Locale
 * pointers in one allocation owned by the caller (SDL_free).  SDL3's
 * locale API does not require SDL_Init, but it can legitimately return
 * NULL/empty (headless CI, exotic platforms) — fail soft so callers
 * default to "en".
 */

#include <jce/os/platform/jce_host_locale.h>

#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_stdinc.h>

bool jce_host_preferred_locale(char *out, size_t cap)
{
    if (!out || cap < 2) return false;

    int count = 0;
    SDL_Locale **locales = SDL_GetPreferredLocales(&count);
    if (!locales) return false;

    bool ok = false;
    if (count > 0 && locales[0] && locales[0]->language &&
        locales[0]->language[0]) {
        const SDL_Locale *l = locales[0];
        if (l->country && l->country[0])
            SDL_snprintf(out, cap, "%s_%s", l->language, l->country);
        else
            SDL_snprintf(out, cap, "%s", l->language);
        for (char *p = out; *p; ++p) {
            if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
        }
        ok = true;
    }

    SDL_free(locales);
    return ok;
}
