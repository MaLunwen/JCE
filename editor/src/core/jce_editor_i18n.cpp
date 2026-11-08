/*
 * jce_editor_i18n.cpp  Internationalisation implementation.
 *
 * Parses the JSON string tables in i18n/*.json from the PAK.
 *
 * The set of locales is *discovered* at startup by walking the PAK
 * archive for entries that match ``i18n/<code>.json``. Adding a new
 * translation therefore needs no code change at all — drop the JSON
 * into editor/resources/assets/i18n/, rebuild (CMake GLOB picks it up
 * and packs it into the editor PAK), and the locale appears in the
 * language picker automatically.
 *
 * Convention:
 *   - filename stem == ISO-ish locale code (``en``, ``zh_cn``, ``ko`` …)
 *   - optional ``"_meta.nativeName"`` key supplies the picker label
 *     in its own language; missing values fall back to uppercased code.
 *   - ``en`` is mandatory and is always index 0 (universal fallback).
 */

#include "jce_editor_i18n.h"

#include "jce_editor_alloc.h"

#include <ctype.h>
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
#define MAX_VALUE_LEN 1024   /* was 512; multibyte (Cyrillic ~1.8B/char) +
                                longer locales (de/ru) overflowed 512 on the
                                longest guide prose (~408 src chars). 1024 B
                                clears the worst case (~755 B for ru). */
#define MAX_EXPAND_LEN 2048
#define MAX_EXPAND_DEPTH 6
#define EXPAND_RING_SIZE 8
#define MAX_CODE_LEN  16
#define MAX_NATIVE_LEN 64
#define I18N_DIR_PREFIX "i18n/"
#define I18N_FILE_SUFFIX ".json"
#define I18N_META_KEY "_meta.nativeName"
/* The catalog stores only admitted strings and an index sized for the
 * locale's actual entry count. One allocation owns all three regions. */
struct I18nEntry {
    uint32_t key;
    uint32_t value;
};

struct I18nTable {
    I18nEntry *entries;
    uint16_t *index;
    char *strings;
    uint32_t index_mask;
};

static uint32_t i18n_key_hash(const char *key)
{
    uint32_t h = 2166136261u;               /* FNV-1a */
    for (const unsigned char *p = (const unsigned char *)key; *p; ++p)
        h = (h ^ *p) * 16777619u;
    return h;
}

/* Reverse insertion preserves the first value for duplicate keys. */
static void i18n_table_index(I18nTable *table, uint32_t count)
{
    for (uint32_t i = count; i > 0; --i) {
        const char *key = table->strings + table->entries[i - 1].key;
        uint32_t h = i18n_key_hash(key) & table->index_mask;
        while (table->index[h] &&
               strcmp(table->strings + table->entries[table->index[h] - 1].key, key) != 0)
            h = (h + 1) & table->index_mask;
        table->index[h] = (uint16_t)i;
    }
}

struct LocaleSlot {
    char code[MAX_CODE_LEN];
    char native_name[MAX_NATIVE_LEN];
    char path[MAX_CODE_LEN + sizeof(I18N_DIR_PREFIX) + sizeof(I18N_FILE_SUFFIX)];
    I18nTable table;
};

/* ── State ─────────────────────────────────────────────────────────── */

static struct {
    LocaleSlot            slots[JCE_MAX_LOCALES];
    int                   count;
    JceLocale             active;
    bool                  initialized;
} s_i18n;

static char s_expand_ring[EXPAND_RING_SIZE][MAX_EXPAND_LEN];
static int  s_expand_ring_index;

/* ── JSON parser ──────────────────────────────────────────────────── */

static size_t string_bytes(const char *text, size_t cap)
{
    size_t length = strlen(text);
    return length < cap ? length + 1 : cap;
}

static uint32_t pack_string(char *strings, uint32_t *offset, const char *text, size_t cap)
{
    const uint32_t start = *offset;
    const size_t bytes = string_bytes(text, cap);
    memcpy(strings + start, text, bytes - 1);
    strings[start + bytes - 1] = '\0';
    *offset += (uint32_t)bytes;
    return start;
}

static void parse_locale(const char *json, LocaleSlot *slot)
{
    JceJson *root = jce_json_parse(json, 0);
    if (!root || !jce_json_is_object(root)) {
        jce_json_free(root);
        return;
    }

    uint32_t count = 0;
    int overflow = 0;
    size_t bytes = 0;
    bool has_native_name = false;
    for (JceJson *item = jce_json_first_child(root); item;
         item = jce_json_next_sibling(item)) {
        const char *key = jce_json_member_key(item);
        if (count >= MAX_STRINGS) ++overflow;
        if (!key || !jce_json_is_string(item)) continue;

        const char *value = jce_json_string_value(item, "");
        if (!has_native_name && strcmp(key, I18N_META_KEY) == 0) {
            snprintf(slot->native_name, sizeof(slot->native_name), "%s", value);
            has_native_name = true;
        }
        if (count >= MAX_STRINGS) continue;
        bytes += string_bytes(key, MAX_KEY_LEN) + string_bytes(value, MAX_VALUE_LEN);
        ++count;
    }
    if (overflow > 0)
        LOG_WARN(LOG_TAG, "i18n table overflow: %d keys dropped (raise MAX_STRINGS)", overflow);

    if (count) {
        uint32_t capacity = 1;
        while (capacity < count * 2) capacity *= 2;
        const size_t index_bytes = capacity * sizeof(uint16_t);
        /* The admitted counts and lengths bound all offsets to < 19 MiB. */
        auto *entries = (I18nEntry *)ED_MALLOC(count * sizeof(I18nEntry) + index_bytes + bytes);
        if (entries) {
            auto *index = (uint16_t *)(entries + count);
            char *strings = (char *)(index + capacity);
            memset(index, 0, index_bytes);

            uint32_t i = 0;
            uint32_t offset = 0;
            for (JceJson *item = jce_json_first_child(root); item && i < count;
                 item = jce_json_next_sibling(item)) {
                const char *key = jce_json_member_key(item);
                if (!key || !jce_json_is_string(item)) continue;
                entries[i].key = pack_string(strings, &offset, key, MAX_KEY_LEN);
                entries[i].value = pack_string(strings, &offset,
                    jce_json_string_value(item, ""), MAX_VALUE_LEN);
                ++i;
            }
            slot->table = {entries, index, strings, capacity - 1};
            i18n_table_index(&slot->table, count);
            LOG_INFO(LOG_TAG, "loaded %s (%u strings)", slot->path, count);
        }
    }
    jce_json_free(root);
}

static const char *lookup_in_table(const I18nTable *table, const char *key, uint32_t hash)
{
    if (!table || !table->index) return nullptr;

    uint32_t h = hash & table->index_mask;
    while (table->index[h]) {
        const I18nEntry &entry = table->entries[table->index[h] - 1];
        if (strcmp(table->strings + entry.key, key) == 0)
            return table->strings + entry.value;
        h = (h + 1) & table->index_mask;
    }
    return nullptr;
}

static bool locale_in_range(JceLocale locale)
{
    return locale >= 0 && locale < s_i18n.count;
}

static const I18nTable *locale_table(JceLocale locale)
{
    return locale_in_range(locale) ? &s_i18n.slots[locale].table : nullptr;
}

static const char *lookup_raw_value(JceLocale locale, const char *key, bool *out_found)
{
    if (out_found) *out_found = false;
    if (!key) return "";

    const uint32_t hash = i18n_key_hash(key);
    const char *value = lookup_in_table(locale_table(locale), key, hash);
    if (!value && locale != JCE_LOCALE_EN)
        value = lookup_in_table(locale_table(JCE_LOCALE_EN), key, hash);
    if (value && out_found) *out_found = true;
    return value ? value : key;
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

static void expand_placeholders(JceLocale locale, const char *src, char *dst, size_t cap, int depth)
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
                const char *replacement = lookup_raw_value(locale, token, &found);

                if (found && depth < MAX_EXPAND_DEPTH) {
                    char nested[MAX_EXPAND_LEN];
                    expand_placeholders(locale, replacement, nested, sizeof(nested), depth + 1);
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

/* ── Locale discovery (PAK scan) ────────────────────────────────────── */

/* Returns true when ``path`` matches ``i18n/<stem>.json`` with a stem
   that is a plausible locale code (lower-case ASCII letters / digits /
   underscores / hyphens, max MAX_CODE_LEN-1 chars). Extracts the stem
   into ``out_code``. */
static bool match_locale_path(const char *path, char out_code[MAX_CODE_LEN])
{
    if (!path) return false;
    const size_t prefix_len = sizeof(I18N_DIR_PREFIX) - 1;
    if (strncmp(path, I18N_DIR_PREFIX, prefix_len) != 0) return false;
    const char *stem = path + prefix_len;
    /* Reject nested paths like i18n/sub/foo.json — locales live flat. */
    if (strchr(stem, '/') || strchr(stem, '\\')) return false;
    size_t n = strlen(stem);
    const size_t suffix_len = sizeof(I18N_FILE_SUFFIX) - 1;
    if (n <= suffix_len) return false;
    if (strcmp(stem + n - suffix_len, I18N_FILE_SUFFIX) != 0) return false;
    size_t stem_len = n - suffix_len;
    if (stem_len == 0 || stem_len >= MAX_CODE_LEN) return false;
    for (size_t i = 0; i < stem_len; i++) {
        char c = stem[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                  || c == '_' || c == '-';
        if (!ok) return false;
    }
    memcpy(out_code, stem, stem_len);
    out_code[stem_len] = '\0';
    return true;
}

static int find_slot_by_code(const char *code)
{
    for (int i = 0; i < s_i18n.count; i++) {
        if (strcmp(s_i18n.slots[i].code, code) == 0) return i;
    }
    return -1;
}

/* Read locale metadata and fall back to the uppercased code for its name. */
static void resolve_locale(const JcePakArchive *pak, LocaleSlot *slot)
{
    slot->native_name[0] = '\0';

    if (pak) {
        const JcePakAsset *asset = jce_pak_find(pak, slot->path);
        if (asset && asset->original_size < SIZE_MAX) {
            char *buf = (char *)ED_MALLOC((size_t)asset->original_size + 1);
            if (buf) {
                size_t n = jce_pak_decompress(asset, buf, (size_t)asset->original_size);
                if (n > 0) {
                    buf[n] = '\0';
                    parse_locale(buf, slot);
                }
                ED_FREE(buf);
            }
        }
    }

    if (slot->native_name[0] == '\0') {
        /* English is special: most files omit _meta.nativeName, but the
           historical label is "English". */
        if (strcmp(slot->code, "en") == 0) {
            strncpy(slot->native_name, "English", sizeof(slot->native_name) - 1);
            slot->native_name[sizeof(slot->native_name) - 1] = '\0';
        } else {
            size_t i = 0;
            for (; slot->code[i] && i + 1 < sizeof(slot->native_name); i++)
                slot->native_name[i] = (char)toupper((unsigned char)slot->code[i]);
            slot->native_name[i] = '\0';
        }
    }
}

static int compare_slots(const void *a, const void *b)
{
    const LocaleSlot *sa = (const LocaleSlot *)a;
    const LocaleSlot *sb = (const LocaleSlot *)b;
    /* English is pinned to index 0 — handled by the caller before
       sorting the remainder, but be defensive. */
    if (strcmp(sa->code, "en") == 0) return -1;
    if (strcmp(sb->code, "en") == 0) return  1;
    return strcmp(sa->code, sb->code);
}

static void discover_locales(const JcePakArchive *pak)
{
    s_i18n.count = 0;
    if (!pak) return;

    uint32_t n = jce_pak_count(pak);
    for (uint32_t i = 0; i < n && s_i18n.count < JCE_MAX_LOCALES; i++) {
        const JcePakAsset *asset = jce_pak_get(pak, i);
        if (!asset || !asset->path) continue;

        char code[MAX_CODE_LEN];
        if (!match_locale_path(asset->path, code)) continue;
        if (find_slot_by_code(code) >= 0) continue; /* duplicate guard */

        LocaleSlot *slot = &s_i18n.slots[s_i18n.count++];
        memset(slot, 0, sizeof(*slot));
        strncpy(slot->code, code, sizeof(slot->code) - 1);
        slot->code[sizeof(slot->code) - 1] = '\0';
        snprintf(slot->path, sizeof(slot->path),
                 I18N_DIR_PREFIX "%s" I18N_FILE_SUFFIX, code);
    }

    /* Sort: "en" first, then the rest alphabetically by code. */
    if (s_i18n.count > 1)
        qsort(s_i18n.slots, (size_t)s_i18n.count, sizeof(LocaleSlot), compare_slots);
}

/* ── Public API ────────────────────────────────────────────────────── */

const char *jce_editor_i18n_locale_code(JceLocale locale)
{
    if (!locale_in_range(locale)) return "en";
    return s_i18n.slots[locale].code;
}

JceLocale jce_editor_i18n_locale_from_code(const char *code)
{
    if (!code) return JCE_LOCALE_EN;
    int idx = find_slot_by_code(code);
    if (idx >= 0) return (JceLocale)idx;
    return JCE_LOCALE_EN;
}

const char *jce_editor_i18n_locale_native_name(JceLocale locale)
{
    if (!locale_in_range(locale)) return "English";
    return s_i18n.slots[locale].native_name;
}

bool jce_editor_i18n_init(const JcePakArchive *pak)
{
    jce_editor_i18n_shutdown();
    s_i18n.active = JCE_LOCALE_EN;

    discover_locales(pak);

    /* English must exist — synthesise a placeholder slot otherwise so
       that the rest of the code (and the language picker) can keep
       running with key-as-value fallback. */
    if (s_i18n.count == 0 || strcmp(s_i18n.slots[0].code, "en") != 0) {
        LOG_WARN(LOG_TAG, "no en.json found in editor PAK; falling back to keys");
        const int keep = s_i18n.count < JCE_MAX_LOCALES ? s_i18n.count : JCE_MAX_LOCALES - 1;
        memmove(&s_i18n.slots[1], &s_i18n.slots[0], (size_t)keep * sizeof(LocaleSlot));
        s_i18n.count = keep + 1;
        LocaleSlot *slot = &s_i18n.slots[0];
        memset(slot, 0, sizeof(*slot));
        strcpy(slot->code, "en");
        strcpy(slot->native_name, "English");
        snprintf(slot->path, sizeof(slot->path), "i18n/en.json");
    }

    for (int i = 0; i < s_i18n.count; ++i)
        resolve_locale(pak, &s_i18n.slots[i]);

    LOG_INFO(LOG_TAG, "discovered %d locale(s)", s_i18n.count);
    s_i18n.initialized = true;
    return true;
}

void jce_editor_i18n_shutdown(void)
{
    for (int i = 0; i < s_i18n.count; ++i)
        ED_FREE(s_i18n.slots[i].table.entries);
    memset(&s_i18n, 0, sizeof(s_i18n));
    s_expand_ring_index = 0;
}

void jce_editor_i18n_set_locale(JceLocale locale)
{
    if (!locale_in_range(locale)) return;
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
    const char *raw = lookup_raw_value(s_i18n.active, key, &found);
    if (!strchr(raw, '{'))
        return raw;

    char *expanded = s_expand_ring[s_expand_ring_index % EXPAND_RING_SIZE];
    s_expand_ring_index = (s_expand_ring_index + 1) % EXPAND_RING_SIZE;
    expand_placeholders(s_i18n.active, raw, expanded, MAX_EXPAND_LEN, 0);
    return expanded;
}

const char *jce_editor_i18n_or(const char *key, const char *fallback)
{
    if (!s_i18n.initialized || !key) return fallback ? fallback : "";
    bool found = false;
    const char *raw = lookup_raw_value(s_i18n.active, key, &found);
    if (!found) return fallback ? fallback : key;
    if (!strchr(raw, '{'))
        return raw;
    char *expanded = s_expand_ring[s_expand_ring_index % EXPAND_RING_SIZE];
    s_expand_ring_index = (s_expand_ring_index + 1) % EXPAND_RING_SIZE;
    expand_placeholders(s_i18n.active, raw, expanded, MAX_EXPAND_LEN, 0);
    return expanded;
}

const char *jce_editor_i18n_lookup_locale(JceLocale locale, const char *key)
{
    if (!s_i18n.initialized || !key) return nullptr;
    return lookup_in_table(locale_table(locale), key, i18n_key_hash(key));
}

static char search_ascii_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool search_starts_with(const char *text, const char *query)
{
    while (*query && *text && search_ascii_lower(*text) == search_ascii_lower(*query)) {
        ++text;
        ++query;
    }
    return !*query;
}

static bool search_contains(const char *text, const char *query)
{
    if (!text) return false;
    for (const char *start = text; *start; ++start) {
        if (search_starts_with(start, query)) return true;
    }
    return false;
}

static bool search_key_matches(const char *key, const char *query)
{
    if (!key) return false;
    const char *separator = strrchr(key, '.');
    const char *leaf = separator ? separator + 1 : key;
    if (search_contains(leaf, query)) return true;

    const char *query_separator = strrchr(query, '.');
    if (!query_separator || !query_separator[1]) return false;
    const size_t prefix = (size_t)(query_separator + 1 - query);
    /* Match only if the qualified query reaches the key's leaf. */
    return (size_t)(leaf - key) >= prefix && search_starts_with(leaf - prefix, query);
}

static bool search_label_matches(JceLocale locale, const char *label, const char *query)
{
    if (!label) return false;
    if (search_contains(label, query)) return true;
    if (!strchr(label, '{')) return false;
    char expanded[MAX_EXPAND_LEN];
    expand_placeholders(locale, label, expanded, sizeof(expanded), 0);
    return search_contains(expanded, query);
}

bool jce_editor_i18n_search_match(const char *key, const char *fallback, const char *query,
                                 JceLocale *matched_locale)
{
    if (matched_locale) *matched_locale = JCE_LOCALE_INVALID;
    if (!query || !*query || search_key_matches(key, query) || search_contains(fallback, query))
        return true;
    if (!s_i18n.initialized || !key) return false;

    const uint32_t hash = i18n_key_hash(key);
    const JceLocale active = s_i18n.active;
    const char *label = lookup_in_table(locale_table(active), key, hash);
    if (search_label_matches(active, label, query)) {
        if (matched_locale) *matched_locale = active;
        return true;
    }
    for (JceLocale locale = 0; locale < s_i18n.count; ++locale) {
        if (locale == active) continue;
        label = lookup_in_table(locale_table(locale), key, hash);
        if (search_label_matches(locale, label, query)) {
            if (matched_locale) *matched_locale = locale;
            return true;
        }
    }
    return false;
}

int jce_editor_i18n_locale_count(void)
{
    return s_i18n.count;
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
