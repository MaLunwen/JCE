/*
 * jce_editor_i18n.cpp  Internationalisation implementation.
 *
 * Parses the JSON string tables in i18n/*.json from the PAK.
 * Uses the engine JSON facade for parsing.
 */

#include "jce_editor_i18n.h"

#include "jce_editor_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_pak_loader.h>
}

#define LOG_TAG       "i18n"
#define MAX_STRINGS   16384
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
    I18nTable             tables[JCE_LOCALE_COUNT];
    bool                  loaded[JCE_LOCALE_COUNT];
    JceLocale             active;
    bool                  initialized;
    const JcePakArchive  *pak;   /* kept so we can lazy-load other locales */
} s_i18n;

static char s_expand_ring[EXPAND_RING_SIZE][MAX_EXPAND_LEN];
static int  s_expand_ring_index;

/* ── JSON parser using jce_json ────────────────────────────────────── */

static bool parse_json_table(const char *json, I18nTable *table)
{
    table->count = 0;
    JceJson *root = jce_json_parse(json, 0);
    if (!root || !jce_json_is_object(root)) {
        jce_json_free(root);
        return false;
    }

    int overflow = 0;
    for (JceJson *item = jce_json_first_child(root); item;
         item = jce_json_next_sibling(item)) {
        if (table->count >= MAX_STRINGS) { overflow++; continue; }
        const char *key = jce_json_member_key(item);
        if (!jce_json_is_string(item) || !key) continue;

        I18nEntry *e = &table->entries[table->count];
        strncpy(e->key, key, MAX_KEY_LEN - 1);
        e->key[MAX_KEY_LEN - 1] = '\0';
        const char *val = jce_json_string_value(item, "");
        strncpy(e->value, val, MAX_VALUE_LEN - 1);
        e->value[MAX_VALUE_LEN - 1] = '\0';
        table->count++;
    }
    if (overflow > 0) {
        LOG_WARN(LOG_TAG, "i18n table overflow: %d keys dropped (raise MAX_STRINGS)", overflow);
    }

    jce_json_free(root);
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

static const char *locale_filename(JceLocale locale)
{
    switch (locale) {
        case JCE_LOCALE_EN:    return "i18n/en.json";
        case JCE_LOCALE_ZH_CN: return "i18n/zh_cn.json";
        case JCE_LOCALE_KO:    return "i18n/ko.json";
        default:               return NULL;
    }
}

const char *jce_editor_i18n_locale_code(JceLocale locale)
{
    switch (locale) {
        case JCE_LOCALE_EN:    return "en";
        case JCE_LOCALE_ZH_CN: return "zh_cn";
        case JCE_LOCALE_KO:    return "ko";
        default:               return "en";
    }
}

JceLocale jce_editor_i18n_locale_from_code(const char *code)
{
    if (!code) return JCE_LOCALE_EN;
    if (strcmp(code, "zh_cn") == 0) return JCE_LOCALE_ZH_CN;
    if (strcmp(code, "ko")    == 0) return JCE_LOCALE_KO;
    return JCE_LOCALE_EN;
}

const char *jce_editor_i18n_locale_native_name(JceLocale locale)
{
    switch (locale) {
        case JCE_LOCALE_EN:    return "English";
        /* "中文(简体)" */
        case JCE_LOCALE_ZH_CN: return "\xe4\xb8\xad\xe6\x96\x87(\xe7\xae\x80\xe4\xbd\x93)";
        /* "한국어" */
        case JCE_LOCALE_KO:    return "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4";
        default:               return "English";
    }
}

static void ensure_locale_loaded(JceLocale locale)
{
    if (locale < 0 || locale >= JCE_LOCALE_COUNT) return;
    if (s_i18n.loaded[locale]) return;
    if (!s_i18n.pak) return;
    const char *path = locale_filename(locale);
    if (!path) return;
    if (load_locale(s_i18n.pak, path, &s_i18n.tables[locale]))
        s_i18n.loaded[locale] = true;
}

bool jce_editor_i18n_init(const JcePakArchive *pak)
{
    memset(&s_i18n, 0, sizeof(s_i18n));
    s_i18n.active = JCE_LOCALE_EN;
    s_i18n.pak    = pak;
    s_expand_ring_index = 0;

    /* Only load the English fallback up-front. The currently-active
       non-English locale (if any) is loaded lazily on the first
       jce_editor_i18n_set_locale() call, which during startup typically
       happens once when the editor reads the saved language preference.
       This trims one ~1 ms JSON parse from cold start when the user
       stays in EN — and avoids loading translations the user never
       switches to. */
    ensure_locale_loaded(JCE_LOCALE_EN);

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
    if (locale < 0 || locale >= JCE_LOCALE_COUNT) return;
    ensure_locale_loaded(locale);
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

const char *jce_editor_i18n_or(const char *key, const char *fallback)
{
    if (!s_i18n.initialized || !key) return fallback ? fallback : "";
    bool found = false;
    const char *raw = lookup_raw_value(key, &found);
    if (!found) return fallback ? fallback : key;
    if (!strchr(raw, '{'))
        return raw;
    char *expanded = s_expand_ring[s_expand_ring_index % EXPAND_RING_SIZE];
    s_expand_ring_index = (s_expand_ring_index + 1) % EXPAND_RING_SIZE;
    expand_placeholders(raw, expanded, MAX_EXPAND_LEN, 0);
    return expanded;
}

const char *jce_editor_i18n_lookup_locale(JceLocale locale, const char *key)
{
    if (!s_i18n.initialized || !key) return NULL;
    if (locale < 0 || locale >= JCE_LOCALE_COUNT) return NULL;
    ensure_locale_loaded(locale);
    return lookup_in_table(&s_i18n.tables[locale], key);
}

int jce_editor_i18n_locale_count(void)
{
    return (int)JCE_LOCALE_COUNT;
}

/* Rotating buffer pool for "label##id" strings. 16 slots avoids clobber
   even when many widgets share a single ImGui::SameLine() row. */
#define LABEL_ID_RING 16
#define LABEL_ID_LEN  192
static char s_label_id_ring[LABEL_ID_RING][LABEL_ID_LEN];
static unsigned s_label_id_idx = 0;

const char *jce_editor_i18n_id(const char *key, const char *id_suffix)
{
    const char *txt = jce_editor_i18n(key);
    char *buf = s_label_id_ring[s_label_id_idx % LABEL_ID_RING];
    s_label_id_idx = (s_label_id_idx + 1) % LABEL_ID_RING;
    /* The ImGui ID is encoded after "###" so it stays stable across
       locale changes (the visible label may change but the ID does not).
       We always include `key` in the ID portion because many call sites
       reuse the same `id_suffix` for every widget in a component (e.g.
       "uim", "uib", "wc", "j2d") — without `key` those widgets would
       collide and ImGui would warn about conflicting IDs. */
    if (key && key[0]) {
        if (id_suffix && id_suffix[0])
            snprintf(buf, LABEL_ID_LEN, "%s###%s.%s", txt, id_suffix, key);
        else
            snprintf(buf, LABEL_ID_LEN, "%s###%s", txt, key);
    } else if (id_suffix && id_suffix[0]) {
        snprintf(buf, LABEL_ID_LEN, "%s###%s", txt, id_suffix);
    } else {
        snprintf(buf, LABEL_ID_LEN, "%s", txt);
    }
    return buf;
}

/* Combo NUL-separated string buffer pool. */
#define COMBO_RING 8
#define COMBO_LEN  1024
static char s_combo_ring[COMBO_RING][COMBO_LEN];
static unsigned s_combo_idx = 0;

const char *jce_editor_i18n_combo(const char *const *keys, int count)
{
    char *buf = s_combo_ring[s_combo_idx % COMBO_RING];
    s_combo_idx = (s_combo_idx + 1) % COMBO_RING;
    size_t off = 0;
    for (int i = 0; i < count; ++i) {
        const char *t = jce_editor_i18n(keys[i]);
        size_t len = strlen(t);
        if (off + len + 2 >= COMBO_LEN) break;
        memcpy(buf + off, t, len);
        off += len;
        buf[off++] = '\0';
    }
    if (off < COMBO_LEN) buf[off] = '\0';
    else buf[COMBO_LEN - 1] = '\0';
    return buf;
}
