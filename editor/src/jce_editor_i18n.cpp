/*
 * jce_editor_i18n.cpp  Internationalisation implementation.
 *
 * Parses the simple JSON string tables in i18n/*.json from the PAK.
 * Uses a minimal hand-rolled parser (no dependency on a JSON library).
 */

#include "jce_editor_i18n.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

extern "C" {
#include <jce/resource/pak_loader.h>
#include <jce/core/jce_log.h>
}

#define LOG_TAG       "i18n"
#define MAX_STRINGS   1024
#define MAX_KEY_LEN   128
#define MAX_VALUE_LEN 512

/* ── String entry ──────────────────────────────────────────────────── */

typedef struct {
    char key[MAX_KEY_LEN];
    char value[MAX_VALUE_LEN];
} I18nEntry;

typedef struct {
    I18nEntry entries[MAX_STRINGS];
    int       count;
} I18nTable;

/* ── State ─────────────────────────────────────────────────────────── */

static struct {
    I18nTable  tables[JCE_LOCALE_COUNT];
    JceLocale  active;
    bool       initialized;
} s_i18n;

/* ── Minimal JSON string-pair parser ───────────────────────────────── */

static void skip_whitespace(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

static bool parse_string(const char **p, char *out, int max_len)
{
    skip_whitespace(p);
    if (**p != '"') return false;
    (*p)++;

    int i = 0;
    while (**p && **p != '"' && i < max_len - 1) {
        if (**p == '\\') {
            (*p)++;
            switch (**p) {
            case '"':  out[i++] = '"';  break;
            case '\\': out[i++] = '\\'; break;
            case 'n':  out[i++] = '\n'; break;
            case 't':  out[i++] = '\t'; break;
            default:   out[i++] = **p;  break;
            }
        } else {
            out[i++] = **p;
        }
        (*p)++;
    }
    out[i] = '\0';
    if (**p == '"') (*p)++;
    return true;
}

static bool parse_json_table(const char *json, I18nTable *table)
{
    table->count = 0;
    const char *p = json;
    skip_whitespace(&p);
    if (*p != '{') return false;
    p++;

    while (*p && *p != '}' && table->count < MAX_STRINGS) {
        I18nEntry *e = &table->entries[table->count];
        if (!parse_string(&p, e->key, MAX_KEY_LEN)) break;
        skip_whitespace(&p);
        if (*p != ':') break;
        p++;
        if (!parse_string(&p, e->value, MAX_VALUE_LEN)) break;
        table->count++;
        skip_whitespace(&p);
        if (*p == ',') p++;
    }
    return true;
}

/* ── Load a single locale file from PAK ────────────────────────────── */

static bool load_locale(const PakArchive *pak, const char *path, I18nTable *table)
{
    const PakAsset *asset = pak_find(pak, path);
    if (!asset) {
        LOG_WARN(LOG_TAG, "locale file not found: %s", path);
        return false;
    }

    char *buf = (char *)malloc((size_t)asset->original_size + 1);
    if (!buf) return false;

    size_t n = pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) {
        free(buf);
        return false;
    }
    buf[n] = '\0';

    bool ok = parse_json_table(buf, table);
    free(buf);

    if (ok)
        LOG_INFO(LOG_TAG, "loaded %s (%d strings)", path, table->count);
    return ok;
}

/* ── Public API ────────────────────────────────────────────────────── */

bool jce_editor_i18n_init(const PakArchive *pak)
{
    memset(&s_i18n, 0, sizeof(s_i18n));
    s_i18n.active = JCE_LOCALE_EN;

    load_locale(pak, "i18n/en.json",    &s_i18n.tables[JCE_LOCALE_EN]);
    load_locale(pak, "i18n/zh_cn.json", &s_i18n.tables[JCE_LOCALE_ZH_CN]);

    s_i18n.initialized = true;
    return true;
}

void jce_editor_i18n_shutdown(void)
{
    memset(&s_i18n, 0, sizeof(s_i18n));
}

void jce_editor_i18n_set_locale(JceLocale locale)
{
    if (locale >= 0 && locale < JCE_LOCALE_COUNT)
        s_i18n.active = locale;
}

JceLocale jce_editor_i18n_get_locale(void)
{
    return s_i18n.active;
}

const char *jce_editor_i18n(const char *key)
{
    if (!s_i18n.initialized || !key) return key ? key : "";

    const I18nTable *table = &s_i18n.tables[s_i18n.active];
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->entries[i].key, key) == 0)
            return table->entries[i].value;
    }

    /* Fallback to English. */
    if (s_i18n.active != JCE_LOCALE_EN) {
        const I18nTable *en = &s_i18n.tables[JCE_LOCALE_EN];
        for (int i = 0; i < en->count; i++) {
            if (strcmp(en->entries[i].key, key) == 0)
                return en->entries[i].value;
        }
    }

    return key; /* key as fallback */
}
