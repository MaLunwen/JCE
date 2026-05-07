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

/* Supported locales. */
typedef enum {
    JCE_LOCALE_EN = 0,
    JCE_LOCALE_ZH_CN,
    JCE_LOCALE_COUNT
} JceLocale;

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
