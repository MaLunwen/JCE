/* Canvas-owned font identities. Glyph objects retain stable addresses. */
#ifndef JCE_UI_CANVAS_FONT_CACHE_H
#define JCE_UI_CANVAS_FONT_CACHE_H
#include <jce/renderer/jce_text.h>
#include <stdbool.h>
#include <stddef.h>

/* Entry budget, not a claim about GPU bytes or the low-memory target. */
#define UC_FONT_CACHE_MAX_ENTRIES 128u

typedef struct {
    char *path;
    int px;
    bool sdf, leaf;
    JceFont *font;
} UCFontCacheEntry;
typedef struct {
    UCFontCacheEntry *entries;
    size_t count, capacity;
    bool warned_budget, warned_allocation;
    const void *filesystem;
    int filesystem_policy;
    unsigned resource_generation;
    bool source_bound;
} UCFontCache;
typedef JceFont *(*UCFontCacheOpen)(void *user, const char *path, int px, bool sdf);

/* Records negative results too. A successful new record is visible before
 * the caller attaches fallback fonts, which may re-enter the same cache. */
JceFont *uc_font_cache_get(UCFontCache *cache, const char *path, int px, bool sdf, bool leaf,
                          UCFontCacheOpen open, void *user, bool *created);
/* Returns true after an initial bind or source/configuration change. */
bool uc_font_cache_bind(UCFontCache *cache, const void *filesystem,
                        int policy, unsigned generation);
void uc_font_cache_clear(UCFontCache *cache);
#endif
