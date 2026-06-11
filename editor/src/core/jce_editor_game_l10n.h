/*
 * jce_editor_game_l10n.h  Editor-side GAME localization string-table model.
 *
 * Key × locale grid over the open project's game string tables
 * (<project_root>/<source_assets>/i18n/<locale>.json, flat {"key":"value"}
 * JSON — the exact files jce_loc_* and the legacy jce_i18n consume at
 * runtime).  Backs the Project Settings ▸ Localization tab and the UIText
 * locale_key picker.
 *
 * This module is also the EDITOR's init point for the engine's
 * process-global jce_loc table: loading a project points jce_loc at the
 * project's host i18n dir so the game view / Play preview resolve
 * locale_key fields live.  (Editor Play never re-inits jce_loc — see
 * jce_runtime_create gating.)  The editor PAK is NEVER handed to jce_loc:
 * it ships editor-chrome strings at the identical "i18n/*.json" path and
 * would leak editor UI text into the game preview.
 *
 * Distinct from jce_editor_i18n (editor-chrome translations) — the two
 * systems stay name-disjoint by design.
 *
 * Returned string pointers stay valid until the next mutating call
 * (load/unload/set/add/remove).
 */

#ifndef JCE_EDITOR_GAME_L10N_H
#define JCE_EDITOR_GAME_L10N_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Scan <project_root>/<source_assets>/i18n/*.json into the model and point
   jce_loc at that directory.  `source_assets` NULL/"" falls back to
   "assets".  Returns true when at least one locale file parsed (a missing
   i18n dir still "loads" an empty model so the UI can offer Add Locale). */
bool        jce_editor_gl10n_load(const char *project_root,
                                  const char *source_assets);

/* Clear the model and detach jce_loc from the project (table emptied). */
void        jce_editor_gl10n_unload(void);

/* True when a project is loaded (even with zero locale files). */
bool        jce_editor_gl10n_loaded(void);

/* Host directory backing the model ("" when not loaded). */
const char *jce_editor_gl10n_dir(void);

int         jce_editor_gl10n_locale_count(void);
const char *jce_editor_gl10n_locale_code_at(int i);

/* Visible (non _meta.*) keys, sorted.  _meta.* entries are preserved on
   save but hidden from the grid (mirrors editor i18n's _meta.nativeName
   convention). */
int         jce_editor_gl10n_key_count(void);
const char *jce_editor_gl10n_key_at(int i);

/* Value lookup — NULL when the key has no entry in that locale. */
const char *jce_editor_gl10n_get(const char *locale, const char *key);
void        jce_editor_gl10n_set(const char *locale, const char *key,
                                 const char *value);

bool        jce_editor_gl10n_add_key(const char *key);
bool        jce_editor_gl10n_remove_key(const char *key);   /* refuses protected keys */
bool        jce_editor_gl10n_add_locale(const char *code);

/* True for the legacy fixed jce_i18n keys (paused/continue/quit/...) the
   engine's built-in pause/settings menu reads by enum — the grid marks
   them with a lock and refuses deletion (removing them would break the
   shipped pause menu). */
bool        jce_editor_gl10n_key_protected(const char *key);

/* Write every locale back to <dir>/<locale>.json (pretty, key-sorted,
   union of keys so locale files stay in parity), then re-fire
   jce_loc_set_locale so the live preview / Play session refreshes. */
bool        jce_editor_gl10n_save_all(void);
bool        jce_editor_gl10n_dirty(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_GAME_L10N_H */
