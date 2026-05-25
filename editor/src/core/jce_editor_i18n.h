/*
 * jce_editor_i18n.h  Editor internationalisation.
 *
 * Loads JSON string tables from the PAK and provides
 * a simple lookup function for translated strings.
 */

#ifndef JCE_EDITOR_I18N_H
#define JCE_EDITOR_I18N_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/resource/jce_pak_loader.h>

/* Locale handle.
 *
 * The set of available locales is discovered at runtime by scanning
 * the editor PAK for ``i18n/*.json`` files — there is no compile-time
 * enum any more.  A ``JceLocale`` is just an index into the locale
 * registry; valid values are ``0 .. jce_editor_i18n_locale_count()-1``.
 *
 * Index 0 is always English (``en.json``); it is the universal fallback
 * and the only locale guaranteed to exist.  Adding a new translation
 * is purely a content change: drop ``editor/resources/assets/i18n/<code>.json``
 * into the tree, rebuild, and it appears in the language picker.
 *
 * To advertise a human-readable label for the language picker the JSON
 * file may include a meta entry:
 *
 *     "_meta.nativeName": "한국어"
 *
 * If absent, the uppercased filename stem is used (``"ko"`` -> ``"KO"``). */
typedef int JceLocale;

#define JCE_LOCALE_INVALID ((JceLocale)-1)
#define JCE_LOCALE_EN      ((JceLocale)0)

/* Hard cap on the number of installed locales.  Stack arrays sized by
   this constant remain ABI-stable for callers that build language
   pickers without heap allocation. */
#define JCE_MAX_LOCALES    32

/* Stable identifier code for a locale (matches the JSON filename stem and
   the value persisted to JceEditorConfig.language).  Returns "en" for any
   out-of-range input so callers can use it as a safe default. */
const char *jce_editor_i18n_locale_code(JceLocale locale);

/* Parse a stable code (e.g. "en", "zh_cn", "ko") back to its enum value.
   Returns JCE_LOCALE_EN when the code is unknown or NULL. */
JceLocale   jce_editor_i18n_locale_from_code(const char *code);

/* Native display name of a locale (UTF-8, written in that language) for
   use in language pickers.  Returns "English" for unknown locales. */
const char *jce_editor_i18n_locale_native_name(JceLocale locale);

/* Initialize the i18n system and load all locale files from PAK. */
bool jce_editor_i18n_init(const JcePakArchive *pak);

/* Shut down and free all string tables. */
void jce_editor_i18n_shutdown(void);

/* Set the active locale. */
void jce_editor_i18n_set_locale(JceLocale locale);

/* Get the active locale. */
JceLocale jce_editor_i18n_get_locale(void);

/* Look up a translated string by key.
   Returns the key itself if not found. */
const char *jce_editor_i18n(const char *key);

/* Look up a translated string by key, returning `fallback` (not the key)
   when no translation is registered. Useful for opportunistic
   translation of strings that already have a sensible English literal
   in code (e.g. reflection field labels). */
const char *jce_editor_i18n_or(const char *key, const char *fallback);

/* Look up a translated string in a SPECIFIC locale (not the active one).
   Used for cross-locale searches such as the Inspector "Add Component"
   filter that should match queries typed in any installed language.
   Returns NULL when the key is not registered in that locale.
   The returned pointer is valid for the lifetime of the i18n system. */
const char *jce_editor_i18n_lookup_locale(JceLocale locale, const char *key);

/* Number of supported locales (fixed compile-time count). Useful for
   loops that walk every locale, e.g. multi-language search. */
int jce_editor_i18n_locale_count(void);

/* Look up a translated string and append "##id_suffix" for ImGui IDs.
   Returns a pointer into a thread-local rotating buffer pool
   (16 slots * 192 bytes), safe across many SameLine() calls per frame.
   Pass id_suffix as the bare id (no leading "##"). */
const char *jce_editor_i18n_id(const char *key, const char *id_suffix);

/* Build a NUL-separated combo string from i18n keys.
   Result: "<txt0>\0<txt1>\0...\0<txtN-1>\0\0" suitable for ImGui::Combo
   (immediate-mode array form). Returns a pointer into a rotating
   buffer pool (8 slots * 1024 bytes); good for one frame. */
const char *jce_editor_i18n_combo(const char *const *keys, int count);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_I18N_H */
