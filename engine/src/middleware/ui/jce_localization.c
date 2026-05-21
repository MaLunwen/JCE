/*
 * jce_localization.c  Runtime localization module.
 *
 * Open-addressing hash table (linear probe, capacity 4096) stores
 * interned UTF-8 key→value pairs loaded from a flat JSON object.
 * FNV-1a is used for fast string hashing.  All allocations go
 * through jce_alloc; file I/O uses jce_json_parse_file (host FS).
 */

#include <jce/middleware/ui/jce_localization.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LOC_TAG          "jce_loc"
#define LOC_CAP          4096   /* must be power-of-two */
#define LOC_LISTENER_MAX 8

/* ── Hash table entry ────────────────────────────────────────────── */

typedef struct {
    char *key;    /* heap-allocated; NULL = empty slot */
    char *value;
} LocEntry;

/* ── Module state ────────────────────────────────────────────────── */

static struct {
    char         locale[64];
    char         dir_prefix[320];
    LocEntry     table[LOC_CAP];
    JceLocChangedFn listeners[LOC_LISTENER_MAX];
    void           *listener_ud[LOC_LISTENER_MAX];
    int             listener_count;
    bool            initialized;
} g_loc;

/* ── Helpers ─────────────────────────────────────────────────────── */

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static char *intern_str(const char *s)
{
    size_t n = strlen(s);
    char  *p = (char *)jce_malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

static void table_clear(void)
{
    for (int i = 0; i < LOC_CAP; ++i) {
        jce_free(g_loc.table[i].key);
        jce_free(g_loc.table[i].value);
        g_loc.table[i].key   = NULL;
        g_loc.table[i].value = NULL;
    }
}

static void table_insert(const char *k, const char *v)
{
    uint32_t base = fnv1a(k) & (LOC_CAP - 1);
    for (int i = 0; i < LOC_CAP; ++i) {
        uint32_t  idx = (base + (uint32_t)i) & (LOC_CAP - 1);
        LocEntry *e   = &g_loc.table[idx];
        if (!e->key) {
            e->key   = intern_str(k);
            e->value = intern_str(v);
            return;
        }
        if (strcmp(e->key, k) == 0) {
            jce_free(e->value);
            e->value = intern_str(v);
            return;
        }
    }
    LOG_WARN(LOC_TAG, "table full — dropping key: %s", k);
}

static const char *table_lookup(const char *k)
{
    uint32_t base = fnv1a(k) & (LOC_CAP - 1);
    for (int i = 0; i < LOC_CAP; ++i) {
        uint32_t  idx = (base + (uint32_t)i) & (LOC_CAP - 1);
        LocEntry *e   = &g_loc.table[idx];
        if (!e->key)              return NULL;
        if (strcmp(e->key, k) == 0) return e->value;
    }
    return NULL;
}

static void load_locale_file(const char *locale_name)
{
    if (!g_loc.dir_prefix[0]) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.json", g_loc.dir_prefix, locale_name);

    JceJson *root = jce_json_parse_file(path);
    if (!root) {
        LOG_WARN(LOC_TAG, "could not load locale file: %s", path);
        return;
    }

    table_clear();

    for (JceJson *it = jce_json_first_child(root); it;
         it = jce_json_next_sibling(it)) {
        const char *k = jce_json_member_key(it);
        const char *v = jce_json_string_value(it, NULL);
        if (k && v) table_insert(k, v);
    }

    jce_json_free(root);
    LOG_INFO(LOC_TAG, "loaded locale '%s' from %s", locale_name, path);
}

/* ── Public API ──────────────────────────────────────────────────── */

void jce_loc_init(const char *host_dir_prefix)
{
    table_clear();
    memset(g_loc.locale,         0, sizeof(g_loc.locale));
    memset(g_loc.listeners,      0, sizeof(g_loc.listeners));
    memset(g_loc.listener_ud,    0, sizeof(g_loc.listener_ud));
    g_loc.listener_count = 0;

    if (host_dir_prefix && *host_dir_prefix)
        snprintf(g_loc.dir_prefix, sizeof(g_loc.dir_prefix),
                 "%s", host_dir_prefix);
    else
        g_loc.dir_prefix[0] = '\0';

    g_loc.initialized = true;
}

void jce_loc_shutdown(void)
{
    table_clear();
    memset(&g_loc, 0, sizeof(g_loc));
}

void jce_loc_set_locale(const char *locale_name)
{
    if (!locale_name || !*locale_name) return;
    snprintf(g_loc.locale, sizeof(g_loc.locale), "%s", locale_name);
    load_locale_file(locale_name);
    for (int i = 0; i < g_loc.listener_count; ++i) {
        if (g_loc.listeners[i])
            g_loc.listeners[i](g_loc.locale, g_loc.listener_ud[i]);
    }
}

const char *jce_loc_get_locale(void)
{
    return g_loc.locale;
}

const char *jce_loc_t(const char *key)
{
    if (!key || !*key) return key;
    const char *v = table_lookup(key);
    return v ? v : key;
}

void jce_loc_register_listener(JceLocChangedFn fn, void *ud)
{
    if (!fn || g_loc.listener_count >= LOC_LISTENER_MAX) return;
    g_loc.listeners[g_loc.listener_count]   = fn;
    g_loc.listener_ud[g_loc.listener_count] = ud;
    ++g_loc.listener_count;
}

void jce_loc_unregister_listener(JceLocChangedFn fn)
{
    for (int i = 0; i < g_loc.listener_count; ++i) {
        if (g_loc.listeners[i] != fn) continue;
        int last = g_loc.listener_count - 1;
        g_loc.listeners[i]   = g_loc.listeners[last];
        g_loc.listener_ud[i] = g_loc.listener_ud[last];
        g_loc.listeners[last]   = NULL;
        g_loc.listener_ud[last] = NULL;
        --g_loc.listener_count;
        return;
    }
}
