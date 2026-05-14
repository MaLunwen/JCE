/*
 * jce_locale_table.h  Game-level localisation tables (CSV-based).
 *
 * Complementary to jce_i18n.h (engine-bundled string IDs).  This
 * module loads CSV-style string tables authored by designers, keyed
 * by a short identifier ("ui.menu.start") with one column per locale
 * code ("en", "zh_cn", "ja", ...).  Game code calls
 * jce_locale_get("ui.menu.start") to fetch the active-locale value.
 *
 * Mirrors Unity Localization Package's StringTable at the data
 * layer — no automatic asset resolution; caller decides which CSV(s)
 * to load.
 *
 * CSV format (UTF-8, no BOM):
 *   key,en,zh_cn,ja[,...locales]
 *   ui.menu.start,Start,开始,スタート
 *   ui.menu.quit,Quit,退出,終了
 *
 * Empty values fall back to the first column.  Quoted strings honour
 * standard CSV escaping ("She said ""hi""").
 *
 * Layer: os/core (Layer 1) — public.
 */

#ifndef JCE_LOCALE_TABLE_H
#define JCE_LOCALE_TABLE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_LOCALE_KEY_MAX    96
#define JCE_LOCALE_CODE_MAX   8
#define JCE_LOCALE_VAL_MAX    512
#define JCE_LOCALE_MAX_LOCALES 16

/* Load entries from a CSV file.  Subsequent loads merge — keys
 * already present are overwritten.  Returns the number of entries
 * loaded, or 0 on failure. */
JCE_API uint32_t jce_locale_load_csv(const char *path);

/* Load entries from an in-memory CSV string (e.g. embedded asset).
 * `len` may be 0 → treat `text` as NUL-terminated. */
JCE_API uint32_t jce_locale_load_csv_memory(const char *text, size_t len);

/* Set the active locale code ("en", "zh_cn", ...).  Subsequent
 * jce_locale_get() calls return values from this column.  If the
 * code isn't present in the loaded table, get() returns the first
 * column (the "fallback locale" by convention). */
JCE_API void        jce_locale_set_active(const char *code);
JCE_API const char *jce_locale_get_active(void);

/* Fetch a localised string.  Returns `def` (which may be NULL) when
 * the key isn't present. */
JCE_API const char *jce_locale_get(const char *key, const char *def);

/* Reverse query: number of entries / iterate keys. */
JCE_API uint32_t    jce_locale_entry_count(void);
JCE_API const char *jce_locale_entry_key (uint32_t index);

/* Enumerate registered locale codes from the most recent CSV. */
JCE_API uint32_t    jce_locale_code_count(void);
JCE_API const char *jce_locale_code_at(uint32_t index);

/* Drop all loaded entries (e.g. on level change). */
JCE_API void        jce_locale_clear(void);

JCE_EXTERN_C_END

#endif /* JCE_LOCALE_TABLE_H */
