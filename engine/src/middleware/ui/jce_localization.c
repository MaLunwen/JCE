/*
 * jce_localization.c  Runtime localization module.
 *
 * Open-addressing hash table (linear probe, capacity 4096) stores
 * interned UTF-8 key→value pairs loaded from a flat JSON object.
 * Keys are hashed with the shared FNV-1a helper (jce_fnv1a32_str,
 * <jce/os/core/jce_hash.h>).  All allocations go
 * through jce_alloc; file I/O uses jce_json_parse_file (host FS)
 * with an optional in-PAK fallback source (jce_loc_set_source_pak)
 * for shipped/WASM builds.  jce_pak_loader (L3) is a legal downward
 * include from this L4 TU — unlike jce_i18n.c, which sits at L2 and must
 * reach the PAK through the inverted jce_fs provider instead.
 */

#include <jce/middleware/ui/jce_localization.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_hash.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_pak_loader.h>

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
    const JcePakArchive *pak;        /* borrowed; NULL = no PAK source */
    char         pak_prefix[64];     /* in-PAK dir, e.g. "i18n" */
    LocEntry     table[LOC_CAP];
    JceLocChangedFn listeners[LOC_LISTENER_MAX];
    void           *listener_ud[LOC_LISTENER_MAX];
    int             listener_count;
    bool            initialized;
} g_loc;

/* ── Helpers ─────────────────────────────────────────────────────── */

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
    uint32_t base = jce_fnv1a32_str(k) & (LOC_CAP - 1);
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
    uint32_t base = jce_fnv1a32_str(k) & (LOC_CAP - 1);
    for (int i = 0; i < LOC_CAP; ++i) {
        uint32_t  idx = (base + (uint32_t)i) & (LOC_CAP - 1);
        LocEntry *e   = &g_loc.table[idx];
        if (!e->key)              return NULL;
        if (strcmp(e->key, k) == 0) return e->value;
    }
    return NULL;
}

/* PAK fallback: decompress "<pak_prefix>/<locale>.json" and parse it.
 * Returns NULL when the source is unset, the asset is missing, or the
 * JSON does not parse. */
static JceJson *parse_locale_from_pak(const char *locale_name, char *path,
                                      size_t path_cap)
{
    if (!g_loc.pak) return NULL;

    if (g_loc.pak_prefix[0])
        snprintf(path, path_cap, "%s/%s.json", g_loc.pak_prefix, locale_name);
    else
        snprintf(path, path_cap, "%s.json", locale_name);

    const JcePakAsset *asset = jce_pak_find(g_loc.pak, path);
    if (!asset) return NULL;

    char *json = (char *)jce_malloc((size_t)asset->original_size + 1);
    if (!json) return NULL;

    size_t n = jce_pak_decompress(asset, json, (size_t)asset->original_size);
    if (n == 0) { jce_free(json); return NULL; }
    json[n] = '\0';

    JceJson *root = jce_json_parse(json, 0);
    jce_free(json);
    return root;
}

static void load_locale_file(const char *locale_name)
{
    if (!g_loc.dir_prefix[0] && !g_loc.pak) return;

    /* Host FS first (editor hot-edits win), then the PAK source. */
    char path[512] = {0};
    JceJson *root = NULL;
    if (g_loc.dir_prefix[0]) {
        snprintf(path, sizeof(path), "%s/%s.json",
                 g_loc.dir_prefix, locale_name);
        root = jce_json_parse_file(path);
    }
    if (!root)
        root = parse_locale_from_pak(locale_name, path, sizeof(path));

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
    g_loc.pak            = NULL;
    g_loc.pak_prefix[0]  = '\0';

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
    memset(&g_loc, 0, sizeof(g_loc));   /* also clears the PAK source */
}

void jce_loc_set_source_pak(const JcePakArchive *pak,
                            const char *pak_dir_prefix)
{
    g_loc.pak = pak;
    if (pak && pak_dir_prefix && *pak_dir_prefix)
        snprintf(g_loc.pak_prefix, sizeof(g_loc.pak_prefix),
                 "%s", pak_dir_prefix);
    else
        g_loc.pak_prefix[0] = '\0';
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
