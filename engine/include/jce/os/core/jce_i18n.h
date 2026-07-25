/*
 * jce_i18n.h  Fixed-set internationalisation for the engine's built-in UI.
 *
 * Enum-keyed (JceStringId) string table for the small, compile-time-fixed
 * set of strings the engine's OWN built-in screens use — the pause menu and
 * the jce_ui_settings panel — plus the font-atlas codepoint pre-pass.
 * Values are read from flat {"key":"value"} JSON in the game PAK
 * (i18n/en.json, i18n/zh_cn.json) through the inverted PAK provider
 * (jce_fs_get_pak_provider), so this L2 module takes no dependency on the
 * L3 resource layer.
 *
 * This is NOT the general localization system.  That is jce_loc_* in
 * <jce/middleware/ui/jce_localization.h> (L4), and it is where new game and
 * UI strings belong.  The two are deliberately separate, not redundant:
 *
 *   - jce_i18n keeps EVERY language resident at once, because
 *     jce_i18n_collect_codepoints() must build a single font atlas that
 *     covers all of them.  jce_loc keeps exactly one active locale and
 *     clears its table on each switch, so it cannot answer that query.
 *   - jce_i18n is reachable from L2/L3; jce_loc is not.
 *   - A missing key here yields "" (after an English fallback); jce_loc_t
 *     returns the key itself, which is the signal UIText uses to fall back
 *     to its authored text.
 *
 * Both read the SAME project files and share ONE key namespace: the key
 * names behind JceStringId (see jce_i18n_key_name) are a reserved subset of
 * the project's i18n/<locale>.json keys — which is why the editor's game
 * string-table grid refuses to delete them.
 *
 * Known limitation: JceLang is a two-entry enum (en, zh_cn).  Widening it is
 * an ABI change and forces every project to ship the added locale file, so
 * multi-language content must use jce_loc instead.
 */

#ifndef JCE_I18N_H
#define JCE_I18N_H


#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JcePakArchive JcePakArchive;

typedef enum {
    JCE_LANG_EN,
    JCE_LANG_ZH_CN,
    JCE_LANG_COUNT
} JceLang;

typedef enum {
    JCE_STR_PAUSED,
    JCE_STR_CONTINUE,
    JCE_STR_QUIT,
    JCE_STR_CONTROLS_HINT,
    JCE_STR_TEXT_DEMO,
    /* Settings menu strings */
    JCE_STR_SETTINGS,
    JCE_STR_VIDEO,
    JCE_STR_AUDIO,
    JCE_STR_FULLSCREEN,
    JCE_STR_RESOLUTION,
    JCE_STR_VSYNC,
    JCE_STR_MASTER_VOLUME,
    JCE_STR_MUSIC_VOLUME,
    JCE_STR_SFX_VOLUME,
    JCE_STR_OK,
    JCE_STR_CANCEL,
    JCE_STR_APPLY,
    JCE_STR_ON,
    JCE_STR_OFF,
    JCE_STR_COUNT
} JceStringId;

/* Load all translation JSON files from PAK.  Call once at startup. */
JCE_API void        jce_i18n_init(const JcePakArchive *pak);

/* Set / get the active language. */
JCE_API void        jce_i18n_set_lang(JceLang lang);
JCE_API JceLang     jce_i18n_get_lang(void);

/* Return the translated string for the active language.
   Falls back to English if the key is missing. */
JCE_API const char *jce_i18n_get(JceStringId id);

/* Short display name for the current language ("EN", "ZH", ...). */
JCE_API const char *jce_i18n_lang_name(void);

/* JSON key name backing `id` ("paused" for JCE_STR_PAUSED, ...), or "" when
   the id is out of range.  Single source of truth for the reserved key set:
   tools that must enumerate it walk id over [0, JCE_STR_COUNT) instead of
   keeping their own copy of the list. */
JCE_API const char *jce_i18n_key_name(JceStringId id);

/* Collect all unique non-ASCII codepoints used across ALL languages.
   Writes up to `cap` codepoints into `buf`.
   Returns the number written. */
JCE_API int         jce_i18n_collect_codepoints(uint32_t *buf, int cap);

JCE_EXTERN_C_END

#endif /* JCE_I18N_H */
