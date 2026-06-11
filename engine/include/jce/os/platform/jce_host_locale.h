/*
 * jce_host_locale.h  Host preferred-locale query (SDL3 backend).
 *
 * Normalizes the OS user-preferred locale into the engine's lowercase
 * "lang[_country]" tag convention ("en", "zh_cn", "ja", ...) — the same
 * tags used for <locale>.json string-table filenames (jce_localization,
 * jce_i18n) and the editor i18n locale codes.
 *
 * Layer: os/platform (L1) — wraps SDL_GetPreferredLocales.
 */

#ifndef JCE_HOST_LOCALE_H
#define JCE_HOST_LOCALE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

/* Write the host's top preferred locale into `out` as a lowercase
   "lang[_country]" tag (e.g. "zh_cn", "en").  Returns false when the OS
   reports no locale (headless/CI) or `out`/`cap` are unusable — callers
   should then default to "en".  Never writes more than `cap` bytes and
   always NUL-terminates on success. */
JCE_API bool JCE_CALL jce_host_preferred_locale(char *out, size_t cap);

JCE_EXTERN_C_END

#endif /* JCE_HOST_LOCALE_H */
