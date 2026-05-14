/*
 * jce_locale_table.c  CSV-driven game localisation table.
 *
 * Storage: contiguous Entry array (linear lookup, bounded N).  Locale
 * codes live in a parallel small array referenced by column index.
 * CSV parser handles quoted fields, embedded commas, and "" escapes.
 *
 * Per-key memory: each Entry holds a key string + JCE_LOCALE_MAX_LOCALES
 * string slots (zeroed when unused).  This is wasteful for sparse
 * tables but keeps the impl trivial; designers shouldn't have more
 * than a handful of locales.
 */

#include <jce/os/core/jce_locale_table.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "locale"
#define LOCALE_ENTRY_MAX 4096

typedef struct {
    char key[JCE_LOCALE_KEY_MAX];
    char vals[JCE_LOCALE_MAX_LOCALES][JCE_LOCALE_VAL_MAX];
    bool used;
} Entry;

static Entry s_entries[LOCALE_ENTRY_MAX];
static uint32_t s_entry_count = 0;

static char s_codes[JCE_LOCALE_MAX_LOCALES][JCE_LOCALE_CODE_MAX];
static uint32_t s_code_count = 0;

static char s_active[JCE_LOCALE_CODE_MAX] = "en";

/* ── CSV tokenizer ───────────────────────────────────────────── */

/* Reads one field from `*p` into `out` (truncating to `cap-1`).
 * Advances `*p` past the field's trailing comma or newline.
 * Returns:
 *   1  — field read, more fields on this line
 *   2  — field read, end-of-line
 *   0  — end-of-input / parse error */
static int csv_next_field(const char **p, const char *end,
                          char *out, size_t cap)
{
    const char *s = *p;
    size_t w = 0;
    if (cap > 0) out[0] = '\0';

    if (s >= end) return 0;

    bool quoted = false;
    if (*s == '"') { quoted = true; s++; }

    while (s < end) {
        char c = *s;
        if (quoted) {
            if (c == '"') {
                /* Either escaped quote ("") or end of field. */
                if (s + 1 < end && s[1] == '"') {
                    if (w + 1 < cap) out[w++] = '"';
                    s += 2;
                    continue;
                }
                s++;            /* close quote */
                quoted = false;
                continue;
            }
            if (w + 1 < cap) out[w++] = c;
            s++;
            continue;
        }
        if (c == ',' || c == '\n' || c == '\r') break;
        if (w + 1 < cap) out[w++] = c;
        s++;
    }
    if (w < cap) out[w] = '\0';

    /* Consume trailing separator. */
    if (s < end && *s == ',')  { s++; *p = s; return 1; }
    if (s < end && (*s == '\n' || *s == '\r')) {
        while (s < end && (*s == '\n' || *s == '\r')) s++;
        *p = s;
        return 2;
    }
    *p = s;
    return 2; /* end of buffer treated as EOL */
}

/* ── Public API ──────────────────────────────────────────────── */

static int find_code_column(const char *code)
{
    for (uint32_t i = 0; i < s_code_count; ++i)
        if (strncmp(s_codes[i], code, JCE_LOCALE_CODE_MAX) == 0)
            return (int)i;
    return -1;
}

static int find_or_add_entry(const char *key)
{
    if (!key || !key[0]) return -1;
    for (uint32_t i = 0; i < s_entry_count; ++i)
        if (s_entries[i].used &&
            strncmp(s_entries[i].key, key, JCE_LOCALE_KEY_MAX) == 0)
            return (int)i;
    if (s_entry_count >= LOCALE_ENTRY_MAX) return -1;
    Entry *e = &s_entries[s_entry_count];
    memset(e, 0, sizeof(*e));
    strncpy(e->key, key, JCE_LOCALE_KEY_MAX - 1);
    e->used = true;
    return (int)s_entry_count++;
}

uint32_t jce_locale_load_csv_memory(const char *text, size_t len)
{
    if (!text) return 0;
    if (len == 0) len = strlen(text);
    const char *p   = text;
    const char *end = text + len;

    /* Header row: key,locale1,locale2,... */
    char field[JCE_LOCALE_VAL_MAX];
    int more = csv_next_field(&p, end, field, sizeof(field));
    if (more == 0) return 0;
    /* First column is the key column — must literally be "key". */
    /* We allow any header in the key column, just consume it. */

    /* Read locale codes until end-of-line. */
    uint32_t new_codes = 0;
    s_code_count = 0;
    while (more == 1 && s_code_count < JCE_LOCALE_MAX_LOCALES) {
        more = csv_next_field(&p, end, field, sizeof(field));
        strncpy(s_codes[s_code_count], field, JCE_LOCALE_CODE_MAX - 1);
        s_codes[s_code_count][JCE_LOCALE_CODE_MAX - 1] = '\0';
        s_code_count++;
        new_codes++;
        if (more == 2) break;
    }

    /* Body rows. */
    uint32_t added = 0;
    while (p < end) {
        more = csv_next_field(&p, end, field, sizeof(field));
        if (more == 0) break;
        if (!field[0]) {
            /* Skip empty key rows (often trailing newlines). */
            while (more == 1) more = csv_next_field(&p, end, field, sizeof(field));
            continue;
        }
        int idx = find_or_add_entry(field);
        if (idx < 0) break;
        Entry *e = &s_entries[idx];
        for (uint32_t col = 0; col < s_code_count; ++col) {
            if (more != 1) break;
            more = csv_next_field(&p, end, field, sizeof(field));
            strncpy(e->vals[col], field, JCE_LOCALE_VAL_MAX - 1);
            e->vals[col][JCE_LOCALE_VAL_MAX - 1] = '\0';
        }
        added++;
    }
    LOG_INFO(LOG_TAG, "loaded %u keys, %u locales", added, new_codes);
    return added;
}

uint32_t jce_locale_load_csv(const char *path)
{
    if (!path) return 0;
    uint64_t size = 0;
    void *raw = jce_fs_host_read_all(path, &size);
    if (!raw || size == 0) {
        if (raw) jce_fs_buffer_free(raw);
        return 0;
    }
    uint32_t n = jce_locale_load_csv_memory((const char *)raw, (size_t)size);
    jce_fs_buffer_free(raw);
    return n;
}

void jce_locale_set_active(const char *code)
{
    if (!code) return;
    strncpy(s_active, code, sizeof(s_active) - 1);
    s_active[sizeof(s_active) - 1] = '\0';
}

const char *jce_locale_get_active(void) { return s_active; }

const char *jce_locale_get(const char *key, const char *def)
{
    if (!key) return def;
    for (uint32_t i = 0; i < s_entry_count; ++i) {
        if (!s_entries[i].used) continue;
        if (strncmp(s_entries[i].key, key, JCE_LOCALE_KEY_MAX) != 0) continue;
        int col = find_code_column(s_active);
        if (col < 0) col = 0;
        const char *v = s_entries[i].vals[col];
        if (v[0]) return v;
        /* Fallback to first column. */
        return s_entries[i].vals[0][0] ? s_entries[i].vals[0] : def;
    }
    return def;
}

uint32_t jce_locale_entry_count(void) { return s_entry_count; }

const char *jce_locale_entry_key(uint32_t i)
{
    if (i >= s_entry_count || !s_entries[i].used) return NULL;
    return s_entries[i].key;
}

uint32_t jce_locale_code_count(void) { return s_code_count; }

const char *jce_locale_code_at(uint32_t i)
{
    if (i >= s_code_count) return NULL;
    return s_codes[i];
}

void jce_locale_clear(void)
{
    s_entry_count = 0;
    s_code_count = 0;
    memset(s_entries, 0, sizeof(s_entries));
    memset(s_codes,   0, sizeof(s_codes));
}
