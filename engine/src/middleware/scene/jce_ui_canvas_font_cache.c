#include "jce_ui_canvas_font_cache.h"
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_log.h>
#include <string.h>

#define LOG_TAG "ui_canvas"

JceFont *uc_font_cache_get(UCFontCache *cache, const char *path, int px, bool sdf, bool leaf,
                          UCFontCacheOpen open, void *user, bool *created)
{
    if (created) *created = false;
    if (!cache || !path || !path[0] || !open) return NULL;
    for (size_t i = 0; i < cache->count; ++i) {
        UCFontCacheEntry *entry = &cache->entries[i];
        if (entry->px == px && entry->sdf == sdf && entry->leaf == leaf &&
            strcmp(entry->path, path) == 0)
            return entry->font;
    }
    if (cache->count >= UC_FONT_CACHE_MAX_ENTRIES) {
        if (!cache->warned_budget) {
            LOG_ERROR(LOG_TAG, "font phase=cache_admission reason=entry_budget "
                      "count=%zu limit=%u path=%s px=%d sdf=%u",
                      cache->count, UC_FONT_CACHE_MAX_ENTRIES, path, px,
                      (unsigned)sdf);
            cache->warned_budget = true;
        }
        return NULL;
    }
    size_t length = strlen(path);
    char *owned = JCE_MALLOC(length + 1u);
    if (!owned) goto allocation_failed;
    memcpy(owned, path, length + 1u);
    if (cache->count == cache->capacity) {
        size_t capacity = cache->capacity ? cache->capacity * 2u : 8u;
        if (capacity > UC_FONT_CACHE_MAX_ENTRIES)
            capacity = UC_FONT_CACHE_MAX_ENTRIES;
        UCFontCacheEntry *entries = JCE_REALLOC(cache->entries,
                                               capacity * sizeof(*entries));
        if (!entries) {
            JCE_FREE(owned);
            goto allocation_failed;
        }
        cache->entries = entries;
        cache->capacity = capacity;
    }
    UCFontCacheEntry *entry = &cache->entries[cache->count++];
    entry->path = owned;
    entry->px = px;
    entry->sdf = sdf;
    entry->leaf = leaf;
    entry->font = open(user, path, px, sdf);
    if (created) *created = true;
    return entry->font;

allocation_failed:
    if (!cache->warned_allocation) {
        LOG_ERROR(LOG_TAG, "font phase=cache_admission reason=allocation "
                  "count=%zu path=%s px=%d sdf=%u",
                  cache->count, path, px, (unsigned)sdf);
        cache->warned_allocation = true;
    }
    return NULL;
}

void uc_font_cache_clear(UCFontCache *cache)
{
    if (!cache) return;
    /* Closing a font only clears its borrowed fallback references; it does
     * not dereference or destroy fallback objects. Every record owns one. */
    for (size_t i = 0; i < cache->count; ++i) {
        if (cache->entries[i].font) jce_font_close(cache->entries[i].font);
        JCE_FREE(cache->entries[i].path);
    }
    JCE_FREE(cache->entries);
    memset(cache, 0, sizeof(*cache));
}

bool uc_font_cache_bind(UCFontCache *cache, const void *filesystem,
                        int policy, unsigned generation)
{
    if (!cache) return false;
    if (cache->source_bound && cache->filesystem == filesystem &&
        cache->filesystem_policy == policy &&
        cache->resource_generation == generation) return false;
    uc_font_cache_clear(cache);
    cache->filesystem = filesystem;
    cache->filesystem_policy = policy;
    cache->resource_generation = generation;
    cache->source_bound = true;
    return true;
}
