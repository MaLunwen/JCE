/*
 * jce_i18n.h  Minimal internationalisation support.
 *
 * Loads translation strings from JSON files in the PAK archive
 * (assets/i18n/en.json, zh_cn.json, ...).
 */

#ifndef JCE_I18N_H
#define JCE_I18N_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PakArchive PakArchive;

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
void        jce_i18n_init(const PakArchive *pak);

/* Set / get the active language. */
void        jce_i18n_set_lang(JceLang lang);
JceLang     jce_i18n_get_lang(void);

/* Return the translated string for the active language.
   Falls back to English if the key is missing. */
const char *jce_i18n_get(JceStringId id);

/* Short display name for the current language ("EN", "ZH", ...). */
const char *jce_i18n_lang_name(void);

/* Collect all unique non-ASCII codepoints used across ALL languages.
   Writes up to `cap` codepoints into `buf`.
   Returns the number written. */
int         jce_i18n_collect_codepoints(uint32_t *buf, int cap);

#ifdef __cplusplus
}
#endif

#endif /* JCE_I18N_H */
