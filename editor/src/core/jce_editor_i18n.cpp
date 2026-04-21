/*
 * jce_editor_i18n.cpp  Internationalisation implementation.
 *
 * Parses the JSON string tables in i18n/*.json from the PAK.
 * Uses cJSON (engine dependency) for parsing.
 */

#include "jce_editor_i18n.h"
#include "jce_editor_alloc.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

extern "C" {
#include <jce/core/pak_loader.h>
#include <jce/core/jce_log.h>
#include <cjson/cJSON.h>
}

#define LOG_TAG       "i18n"
#define MAX_STRINGS   1024
#define MAX_KEY_LEN   128
#define MAX_VALUE_LEN 512
#define MAX_EXPAND_LEN 1024
#define MAX_EXPAND_DEPTH 6
#define EXPAND_RING_SIZE 8

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

static char s_expand_ring[EXPAND_RING_SIZE][MAX_EXPAND_LEN];
static int  s_expand_ring_index;

/* ── JSON parser using cJSON ───────────────────────────────────────── */

static bool parse_json_table(const char *json, I18nTable *table)
{
    table->count = 0;
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (table->count >= MAX_STRINGS) break;
        if (!cJSON_IsString(item) || !item->string) continue;

        I18nEntry *e = &table->entries[table->count];
        strncpy(e->key, item->string, MAX_KEY_LEN - 1);
        e->key[MAX_KEY_LEN - 1] = '\0';
        strncpy(e->value, item->valuestring ? item->valuestring : "",
                MAX_VALUE_LEN - 1);
        e->value[MAX_VALUE_LEN - 1] = '\0';
        table->count++;
    }

    cJSON_Delete(root);
    return true;
}

/* ── Load a single locale file from PAK ────────────────────────────── */

static bool load_locale(const JcePakArchive *pak, const char *path, I18nTable *table)
{
    const JcePakAsset *asset = jce_pak_find(pak, path);
    if (!asset) {
        LOG_WARN(LOG_TAG, "locale file not found: %s", path);
        return false;
    }

    char *buf = (char *)ED_MALLOC((size_t)asset->original_size + 1);
    if (!buf) return false;

    size_t n = jce_pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) {
        ED_FREE(buf);
        return false;
    }
    buf[n] = '\0';

    bool ok = parse_json_table(buf, table);
    ED_FREE(buf);

    if (ok)
        LOG_INFO(LOG_TAG, "loaded %s (%d strings)", path, table->count);
    return ok;
}

static const char *lookup_in_table(const I18nTable *table, const char *key)
{
    if (!table || !key) return NULL;

    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->entries[i].key, key) == 0)
            return table->entries[i].value;
    }
    return NULL;
}

static const char *lookup_raw_value(const char *key, bool *out_found)
{
    if (out_found) *out_found = false;
    if (!key) return "";

    const I18nTable *table = &s_i18n.tables[s_i18n.active];
    const char *value = lookup_in_table(table, key);
    if (value) {
        if (out_found) *out_found = true;
        return value;
    }

    if (s_i18n.active != JCE_LOCALE_EN) {
        const I18nTable *en = &s_i18n.tables[JCE_LOCALE_EN];
        value = lookup_in_table(en, key);
        if (value) {
            if (out_found) *out_found = true;
            return value;
        }
    }

    return key;
}

static void append_char(char *dst, size_t cap, size_t *io_pos, char ch)
{
    if (!dst || !io_pos || cap == 0) return;
    if (*io_pos + 1 >= cap) return;
    dst[*io_pos] = ch;
    (*io_pos)++;
}

static void append_cstr(char *dst, size_t cap, size_t *io_pos, const char *src)
{
    if (!src) return;
    while (*src) {
        append_char(dst, cap, io_pos, *src);
        src++;
    }
}

static void expand_placeholders(const char *src, char *dst, size_t cap, int depth)
{
    if (!dst || cap == 0) return;

    size_t out_pos = 0;
    dst[0] = '\0';
    if (!src) return;

    const char *p = src;
    while (*p) {
        if (*p == '{') {
            const char *end = strchr(p + 1, '}');
            if (end && end > p + 1) {
                char token[MAX_KEY_LEN];
                size_t token_len = (size_t)(end - (p + 1));
                if (token_len >= sizeof(token))
                    token_len = sizeof(token) - 1;
                memcpy(token, p + 1, token_len);
                token[token_len] = '\0';

                bool found = false;
                const char *replacement = lookup_raw_value(token, &found);

                if (found && depth < MAX_EXPAND_DEPTH) {
                    char nested[MAX_EXPAND_LEN];
                    expand_placeholders(replacement, nested, sizeof(nested), depth + 1);
                    append_cstr(dst, cap, &out_pos, nested);
                } else {
                    append_char(dst, cap, &out_pos, '{');
                    append_cstr(dst, cap, &out_pos, token);
                    append_char(dst, cap, &out_pos, '}');
                }

                p = end + 1;
                continue;
            }
        }

        append_char(dst, cap, &out_pos, *p);
        p++;
    }

    dst[out_pos] = '\0';
}

/* ── Public API ────────────────────────────────────────────────────── */

bool jce_editor_i18n_init(const JcePakArchive *pak)
{
    memset(&s_i18n, 0, sizeof(s_i18n));
    s_i18n.active = JCE_LOCALE_EN;
    s_expand_ring_index = 0;

    load_locale(pak, "i18n/en.json",    &s_i18n.tables[JCE_LOCALE_EN]);
    load_locale(pak, "i18n/zh_cn.json", &s_i18n.tables[JCE_LOCALE_ZH_CN]);

    s_i18n.initialized = true;
    return true;
}

void jce_editor_i18n_shutdown(void)
{
    memset(&s_i18n, 0, sizeof(s_i18n));
    s_expand_ring_index = 0;
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

    bool found = false;
    const char *raw = lookup_raw_value(key, &found);
    if (!strchr(raw, '{'))
        return raw;

    char *expanded = s_expand_ring[s_expand_ring_index % EXPAND_RING_SIZE];
    s_expand_ring_index = (s_expand_ring_index + 1) % EXPAND_RING_SIZE;
    expand_placeholders(raw, expanded, MAX_EXPAND_LEN, 0);
    return expanded;
}
