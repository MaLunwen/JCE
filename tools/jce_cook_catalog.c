/*
 * jce_cook_catalog.c  Per-asset incremental cook cache (see header).
 *
 * Pure, linkable: no main(), no global state.  Unit-tested directly.
 */

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "jce_cook_catalog.h"

#include <jce/os/core/jce_filesystem.h>

#include <xxhash.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* Hashing                                                             */
/* ================================================================== */

uint64_t jce_cook_hash_buffer(const void *data, size_t size)
{
    /* XXH3 accepts a NULL pointer only with length 0; guard explicitly so a
       zero-byte source still produces a stable, well-defined fingerprint. */
    if (size == 0)
        return (uint64_t)XXH3_64bits("", 0);
    return (uint64_t)XXH3_64bits(data, size);
}

uint64_t jce_cook_hash_file(const char *source_path, uint64_t salt,
                            uint64_t *out_size, int64_t *out_mtime,
                            bool *out_ok)
{
    if (out_ok)    *out_ok = false;
    if (out_size)  *out_size = 0;
    if (out_mtime) *out_mtime = 0;

    if (!source_path)
        return 0;

    uint64_t raw_size = 0;
    void    *raw = jce_fs_host_read_all(source_path, &raw_size);
    if (!raw)
        return 0;

    XXH3_state_t *xs = XXH3_createState();
    if (!xs) {
        jce_fs_buffer_free(raw);
        return 0;
    }
    XXH3_64bits_reset(xs);

    /* 1. Source bytes. */
    XXH3_64bits_update(xs, raw, (size_t)raw_size);
    jce_fs_buffer_free(raw);

    /* 2. Sibling "<source>.import.json" sidecar, if present.  Folding the
       sidecar means editing an import preset (target_format / max_size /
       quality...) invalidates the cached cook even though the source image
       is unchanged. */
    {
        char sidecar[1408];
        snprintf(sidecar, sizeof(sidecar), "%s.import.json", source_path);
        uint64_t isz = 0;
        void    *iraw = jce_fs_host_read_all(sidecar, &isz);
        if (iraw) {
            uint64_t ih = (uint64_t)XXH3_64bits(iraw, (size_t)isz);
            XXH3_64bits_update(xs, &ih, sizeof(ih));
            jce_fs_buffer_free(iraw);
        }
    }

    /* 3. Caller salt (cook options fingerprint: platform/texfmt/quality...). */
    XXH3_64bits_update(xs, &salt, sizeof(salt));

    uint64_t h = (uint64_t)XXH3_64bits_digest(xs);
    XXH3_freeState(xs);

    /* Stat is advisory (debugging / reporting); a read failure here does not
       invalidate the already-computed content hash. */
    if (out_size)
        (void)jce_fs_host_get_size(source_path, out_size);
    if (out_mtime)
        (void)jce_fs_host_get_mtime(source_path, out_mtime);

    if (out_ok)
        *out_ok = true;
    return h;
}

/* ================================================================== */
/* Catalog container                                                   */
/* ================================================================== */

void jce_cook_catalog_init(JceCookCatalog *cat)
{
    if (!cat) return;
    cat->entries  = NULL;
    cat->count    = 0;
    cat->capacity = 0;
}

void jce_cook_catalog_free(JceCookCatalog *cat)
{
    if (!cat) return;
    for (size_t i = 0; i < cat->count; ++i)
        free(cat->entries[i].path);
    free(cat->entries);
    cat->entries  = NULL;
    cat->count    = 0;
    cat->capacity = 0;
}

static JceCookCatalogEntry *catalog_find_mut(JceCookCatalog *cat,
                                             const char *path)
{
    if (!cat || !path) return NULL;
    for (size_t i = 0; i < cat->count; ++i) {
        if (cat->entries[i].path && strcmp(cat->entries[i].path, path) == 0)
            return &cat->entries[i];
    }
    return NULL;
}

const JceCookCatalogEntry *jce_cook_catalog_find(const JceCookCatalog *cat,
                                                 const char *path)
{
    /* Cast away const for the shared lookup; we do not mutate. */
    return catalog_find_mut((JceCookCatalog *)cat, path);
}

static bool catalog_reserve(JceCookCatalog *cat, size_t need)
{
    if (cat->capacity >= need)
        return true;
    size_t cap = cat->capacity ? cat->capacity * 2 : 16;
    while (cap < need)
        cap *= 2;
    JceCookCatalogEntry *e =
        (JceCookCatalogEntry *)realloc(cat->entries, cap * sizeof(*e));
    if (!e)
        return false;
    cat->entries  = e;
    cat->capacity = cap;
    return true;
}

bool jce_cook_catalog_record(JceCookCatalog *cat, const char *path,
                             uint64_t hash, uint64_t size, int64_t mtime)
{
    if (!cat || !path) return false;

    JceCookCatalogEntry *e = catalog_find_mut(cat, path);
    if (e) {
        e->hash  = hash;
        e->size  = size;
        e->mtime = mtime;
        e->seen  = true;   /* recording an asset proves it is live this run */
        return true;
    }

    if (!catalog_reserve(cat, cat->count + 1))
        return false;

    char *dup = NULL;
    {
        size_t n = strlen(path) + 1;
        dup = (char *)malloc(n);
        if (!dup) return false;
        memcpy(dup, path, n);
    }

    e = &cat->entries[cat->count++];
    e->path  = dup;
    e->hash  = hash;
    e->size  = size;
    e->mtime = mtime;
    e->seen  = true;   /* fresh insert this run is live by definition */
    return true;
}

bool jce_cook_entry_is_up_to_date(const JceCookCatalog *cat, const char *path,
                                  uint64_t hash)
{
    const JceCookCatalogEntry *e = jce_cook_catalog_find(cat, path);
    return e != NULL && e->hash == hash;
}

/* ================================================================== */
/* Stale-entry GC (per-run liveness sweep)                             */
/* ================================================================== */

void jce_cook_catalog_begin_epoch(JceCookCatalog *cat)
{
    if (!cat) return;
    for (size_t i = 0; i < cat->count; ++i)
        cat->entries[i].seen = false;
}

void jce_cook_catalog_mark_seen(JceCookCatalog *cat, const char *path)
{
    JceCookCatalogEntry *e = catalog_find_mut(cat, path);
    if (e)
        e->seen = true;
}

size_t jce_cook_catalog_sweep_unseen(JceCookCatalog *cat)
{
    if (!cat) return 0;

    size_t pruned = 0;
    size_t w = 0;   /* write cursor for the compaction */
    for (size_t r = 0; r < cat->count; ++r) {
        if (cat->entries[r].seen) {
            if (w != r)
                cat->entries[w] = cat->entries[r];
            ++w;
        } else {
            /* This source vanished since the last cook: drop the record and
               free its owned key.  The caller may delete the orphan output
               separately (it owns the output-path mapping). */
            free(cat->entries[r].path);
            ++pruned;
        }
    }
    cat->count = w;
    return pruned;
}

/* ================================================================== */
/* Persistence                                                         */
/* ================================================================== */

#define JCE_COOK_CATALOG_MAGIC "# jce_cook_catalog v1"

bool jce_cook_catalog_load(JceCookCatalog *cat, const char *catalog_path)
{
    if (!cat) return false;
    jce_cook_catalog_init(cat);
    if (!catalog_path) return true;

    /* Missing file == first cook == empty catalog (not an error). */
    if (!jce_fs_host_exists_file(catalog_path))
        return true;

    uint64_t size = 0;
    void    *raw = jce_fs_host_read_all(catalog_path, &size);
    if (!raw)
        return false;

    /* NUL-terminate into a private copy so we can use line tokenizers. */
    char *text = (char *)malloc((size_t)size + 1);
    if (!text) {
        jce_fs_buffer_free(raw);
        return false;
    }
    memcpy(text, raw, (size_t)size);
    text[size] = '\0';
    jce_fs_buffer_free(raw);

    bool ok = true;
    char *line = text;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        /* Strip a trailing CR (catalog may have been written on Windows). */
        size_t llen = strlen(line);
        if (llen && line[llen - 1] == '\r')
            line[--llen] = '\0';

        if (llen && line[0] != '#') {
            /* Format: <hash_hex16> <size_dec> <mtime_dec>\t<path> */
            unsigned long long h = 0, sz = 0;
            long long mt = 0;
            const char *tab = strchr(line, '\t');
            if (tab && tab[1]) {
                /* Parse the three leading numeric fields up to the tab. */
                if (sscanf(line, "%llx %llu %lld", &h, &sz, &mt) == 3) {
                    if (!jce_cook_catalog_record(cat, tab + 1,
                                                 (uint64_t)h, (uint64_t)sz,
                                                 (int64_t)mt)) {
                        ok = false;
                        break;
                    }
                }
                /* A malformed numeric prefix is skipped, not fatal. */
            }
        }

        if (!nl) break;
        line = nl + 1;
    }

    free(text);
    if (!ok)
        jce_cook_catalog_free(cat);
    return ok;
}

bool jce_cook_catalog_save(const JceCookCatalog *cat, const char *catalog_path)
{
    if (!cat || !catalog_path) return false;

    /* Build the whole file in memory then write atomically. */
    size_t cap = 64;
    for (size_t i = 0; i < cat->count; ++i)
        cap += (cat->entries[i].path ? strlen(cat->entries[i].path) : 0) + 64;

    char *buf = (char *)malloc(cap);
    if (!buf) return false;

    int n = snprintf(buf, cap, "%s\n", JCE_COOK_CATALOG_MAGIC);
    size_t len = (n > 0) ? (size_t)n : 0;

    for (size_t i = 0; i < cat->count; ++i) {
        const JceCookCatalogEntry *e = &cat->entries[i];
        if (!e->path) continue;
        int w = snprintf(buf + len, cap - len,
                         "%016llx %llu %lld\t%s\n",
                         (unsigned long long)e->hash,
                         (unsigned long long)e->size,
                         (long long)e->mtime,
                         e->path);
        if (w < 0 || (size_t)w >= cap - len) {
            /* Should not happen given the reservation above; bail safely. */
            free(buf);
            return false;
        }
        len += (size_t)w;
    }

    bool ok = jce_fs_host_write_all(catalog_path, buf, (uint64_t)len);
    free(buf);
    return ok;
}
