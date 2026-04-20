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

#include <jce/core/pak_loader.h>

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

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_I18N_H */
