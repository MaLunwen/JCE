/*
 * jce_localization.h  Runtime localization (L10n) API.
 *
 * Loads flat JSON key→value maps from host-filesystem locale files,
 * exposes a `jce_loc_t(key)` translate helper, and fires registered
 * callbacks whenever the active locale changes so that ECS text
 * components and other subsystems can refresh in-place.
 *
 * Usage:
 *   jce_loc_init("engine/resources/assets/i18n");
 *   jce_loc_set_locale("en");              // loads .../en.json
 *   const char *s = jce_loc_t("ui.hello"); // → "Hello"
 *   jce_loc_shutdown();
 *
 * Layer: middleware/ui (L4) — depends on jce_json, jce_alloc (L2).
 *
 * This is the authoritative localization system for game and UI strings:
 * open-ended key space, N locales, reloadable at runtime, change listeners.
 * Prefer it for anything new.
 *
 * It does NOT subsume <jce/os/core/jce_i18n.h> (jce_i18n_*), which stays for
 * two reasons that cannot be met here: it is reachable from L2/L3 (this
 * module is not), and it keeps every language resident at once so
 * jce_i18n_collect_codepoints() can build one font atlas covering all of
 * them — jce_loc holds a single locale and clears the table on each switch.
 * Both read the same i18n/<locale>.json files and share one key namespace;
 * the JceStringId key names are a reserved subset (see jce_i18n_key_name).
 */

#ifndef JCE_LOCALIZATION_H
#define JCE_LOCALIZATION_H

#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

typedef struct JcePakArchive JcePakArchive;

/* Callback signature for locale-change notifications. */
typedef void (*JceLocChangedFn)(const char *locale, void *userdata);

/* Initialise the localization module.
   host_dir_prefix: host-filesystem directory that contains <locale>.json files,
   e.g. "engine/resources/assets/i18n".  May be NULL/empty (disables file I/O).
   Safe to call multiple times (re-initialises). */
JCE_API void JCE_CALL jce_loc_init(const char *host_dir_prefix);

/* Release all interned strings and reset state. */
JCE_API void JCE_CALL jce_loc_shutdown(void);

/* Register a PAK archive as a fallback locale source.  When set,
   jce_loc_set_locale() first tries the host directory passed to
   jce_loc_init() (editor hot-edits win), then falls back to
   "<pak_dir_prefix>/<locale>.json" inside `pak` (e.g. "i18n/zh_cn.json").
   This is the load path for shipped/WASM builds where locale files live
   only inside the game PAK.  The archive is borrowed, never owned — it
   must outlive the source registration.  Pass pak=NULL to clear; the
   source is also cleared by jce_loc_init()/jce_loc_shutdown(). */
JCE_API void JCE_CALL jce_loc_set_source_pak(const JcePakArchive *pak,
                                             const char *pak_dir_prefix);

/* Load the locale file for `locale_name` (e.g. "en", "zh_cn"),
   replace the current translation table, and fire all registered listeners. */
JCE_API void JCE_CALL jce_loc_set_locale(const char *locale_name);

/* Return the active locale tag (e.g. "en").  Empty string if unset. */
JCE_API const char *JCE_CALL jce_loc_get_locale(void);

/* Translate `key`.  Returns the translated string on success, or `key`
   itself when no entry is found (never returns NULL when key is non-NULL). */
JCE_API const char *JCE_CALL jce_loc_t(const char *key);

/* Register a locale-change listener (max 8 slots).
   The callback is fired synchronously inside jce_loc_set_locale().
   No-op on NULL fn or when all slots are taken. */
JCE_API void JCE_CALL jce_loc_register_listener(JceLocChangedFn fn, void *userdata);

/* Unregister by function pointer.  No-op if not registered. */
JCE_API void JCE_CALL jce_loc_unregister_listener(JceLocChangedFn fn);

JCE_EXTERN_C_END

#endif /* JCE_LOCALIZATION_H */
