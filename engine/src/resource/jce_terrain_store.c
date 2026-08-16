/*
 * jce_terrain_store.c -- authoritative terrain tile residency.
 * See jce_terrain_store.h for the contract and the ownership rules.
 */

#include "jce_terrain_store.h"

#include "os/core/jce_memory.h"

#include <string.h>

#define TS_PATH_MAX 256u

/* Handle packing: 20 bits of index, 12 bits of generation.  Generation is
 * what makes a stale handle fail closed instead of resolving to whoever
 * recycled the slot. */
#define TS_INDEX_BITS 20u
#define TS_INDEX_MASK ((1u << TS_INDEX_BITS) - 1u)
#define TS_GEN_MASK   0xFFFu

static JceTerrainHandle ts_pack(uint32_t idx, uint32_t gen)
{
    JceTerrainHandle h;
    h.bits = (idx & TS_INDEX_MASK) | ((gen & TS_GEN_MASK) << TS_INDEX_BITS);
    return h;
}
static uint32_t ts_index(JceTerrainHandle h) { return h.bits & TS_INDEX_MASK; }
static uint32_t ts_gen(JceTerrainHandle h)
{
    return (h.bits >> TS_INDEX_BITS) & TS_GEN_MASK;
}

typedef struct {
    char     path[TS_PATH_MAX];
    uint32_t refcount;      /* 0 = free slot */
    uint32_t generation;
} TsAsset;

typedef struct {
    uint32_t          asset_index;
    uint32_t          asset_generation;
    JceTerrainTileKey key;
    float            *heights;
    uint32_t          w, h;
    uint32_t          pins;
    uint64_t          last_use;   /* LRU stamp */
    uint32_t          generation; /* bumped on reuse, validated on unpin */
    bool              used;
} TsTile;

struct JceTerrainStore {
    JceTerrainStoreConfig cfg;
    TsAsset  *assets;
    TsTile   *tiles;
    uint64_t  clock;
    uint64_t  resident_bytes;
    uint64_t  evictions;
    uint64_t  loads;
};

void jce_terrain_store_config_init(JceTerrainStoreConfig *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->max_assets         = 64u;
    cfg->max_resident_tiles = 64u;
    cfg->max_resident_bytes = 64ull * 1024ull * 1024ull;   /* 64 MiB */
}

JceTerrainStore *jce_terrain_store_create(const JceTerrainStoreConfig *cfg)
{
    if (!cfg || !cfg->loader) return NULL;
    if (cfg->max_assets == 0u || cfg->max_resident_tiles == 0u) return NULL;
    if (cfg->max_assets > TS_INDEX_MASK) return NULL;
    /* A zero byte budget is a configuration error, not "unlimited": read as
     * unlimited it would silently disable eviction on the 512 MB profile. */
    if (cfg->max_resident_bytes == 0ull) return NULL;

    JceTerrainStore *s = (JceTerrainStore *)JCE_CALLOC(1, sizeof *s);
    if (!s) return NULL;
    s->cfg    = *cfg;
    s->assets = (TsAsset *)JCE_CALLOC(cfg->max_assets, sizeof(TsAsset));
    s->tiles  = (TsTile *)JCE_CALLOC(cfg->max_resident_tiles, sizeof(TsTile));
    if (!s->assets || !s->tiles) {
        JCE_FREE(s->assets); JCE_FREE(s->tiles); JCE_FREE(s);
        return NULL;
    }
    /* Start generations at 1 so a zeroed handle is never accidentally valid. */
    for (uint32_t i = 0; i < cfg->max_assets; i++) s->assets[i].generation = 1u;
    for (uint32_t i = 0; i < cfg->max_resident_tiles; i++)
        s->tiles[i].generation = 1u;
    return s;
}

uint32_t jce_terrain_store_pinned_tiles(const JceTerrainStore *s)
{
    if (!s) return 0u;
    uint32_t n = 0u;
    for (uint32_t i = 0; i < s->cfg.max_resident_tiles; i++)
        if (s->tiles[i].used && s->tiles[i].pins > 0u) n++;
    return n;
}

bool jce_terrain_store_destroy(JceTerrainStore *s)
{
    if (!s) return true;
    /* Refuse while a borrowed view could still be read.  Freeing here would
     * hand the consumer a dangling pointer, which is far worse than leaking
     * until it unpins. */
    if (jce_terrain_store_pinned_tiles(s) > 0u) return false;

    for (uint32_t i = 0; i < s->cfg.max_resident_tiles; i++)
        JCE_FREE(s->tiles[i].heights);
    JCE_FREE(s->tiles);
    JCE_FREE(s->assets);
    JCE_FREE(s);
    return true;
}

/* ── Assets ────────────────────────────────────────────────────────── */

JceTerrainHandle jce_terrain_store_acquire(JceTerrainStore *s,
                                           const char *virtual_path)
{
    if (!s || !virtual_path || !virtual_path[0])
        return JCE_TERRAIN_HANDLE_INVALID;
    if (strlen(virtual_path) >= TS_PATH_MAX)
        return JCE_TERRAIN_HANDLE_INVALID;

    /* Dedup: the same canonical path is one asset shared by every consumer.
     * This is the whole point -- three independent loads is the defect. */
    for (uint32_t i = 0; i < s->cfg.max_assets; i++) {
        if (s->assets[i].refcount > 0u &&
            strcmp(s->assets[i].path, virtual_path) == 0) {
            s->assets[i].refcount++;
            return ts_pack(i, s->assets[i].generation);
        }
    }
    for (uint32_t i = 0; i < s->cfg.max_assets; i++) {
        if (s->assets[i].refcount == 0u) {
            memset(s->assets[i].path, 0, TS_PATH_MAX);
            strncpy(s->assets[i].path, virtual_path, TS_PATH_MAX - 1u);
            s->assets[i].refcount = 1u;
            return ts_pack(i, s->assets[i].generation);
        }
    }
    return JCE_TERRAIN_HANDLE_INVALID;
}

bool jce_terrain_store_is_valid(const JceTerrainStore *s, JceTerrainHandle h)
{
    if (!s || h.bits == JCE_TERRAIN_HANDLE_INVALID.bits) return false;
    const uint32_t i = ts_index(h);
    if (i >= s->cfg.max_assets) return false;
    return s->assets[i].refcount > 0u &&
           (s->assets[i].generation & TS_GEN_MASK) == ts_gen(h);
}

static void ts_drop_tiles_of(JceTerrainStore *s, uint32_t asset_index,
                             uint32_t asset_generation)
{
    for (uint32_t t = 0; t < s->cfg.max_resident_tiles; t++) {
        TsTile *ti = &s->tiles[t];
        if (!ti->used || ti->pins > 0u) continue;
        if (ti->asset_index != asset_index) continue;
        if (ti->asset_generation != asset_generation) continue;
        s->resident_bytes -= (uint64_t)ti->w * ti->h * sizeof(float);
        JCE_FREE(ti->heights);
        ti->heights = NULL;
        ti->used = false;
        ti->generation = (ti->generation + 1u) & TS_GEN_MASK;
        if (ti->generation == 0u) ti->generation = 1u;
    }
}

void jce_terrain_store_release(JceTerrainStore *s, JceTerrainHandle h)
{
    if (!jce_terrain_store_is_valid(s, h)) return;
    const uint32_t i = ts_index(h);
    if (--s->assets[i].refcount == 0u) {
        ts_drop_tiles_of(s, i, s->assets[i].generation);
        /* Bump on release so a handle kept across a reacquire of this slot
         * fails closed rather than aliasing the new asset. */
        s->assets[i].generation = (s->assets[i].generation + 1u) & TS_GEN_MASK;
        if (s->assets[i].generation == 0u) s->assets[i].generation = 1u;
        s->assets[i].path[0] = '\0';
    }
}

/* ── Tiles ─────────────────────────────────────────────────────────── */

static TsTile *ts_find_resident(JceTerrainStore *s, uint32_t ai, uint32_t ag,
                                JceTerrainTileKey k)
{
    for (uint32_t t = 0; t < s->cfg.max_resident_tiles; t++) {
        TsTile *ti = &s->tiles[t];
        if (ti->used && ti->asset_index == ai && ti->asset_generation == ag &&
            ti->key.x == k.x && ti->key.z == k.z)
            return ti;
    }
    return NULL;
}

/* Free one slot: prefer an unused one, else evict the least-recently-used
 * UNPINNED tile.  Pinned tiles are skipped unconditionally, which is what
 * makes a borrowed view safe. */
static TsTile *ts_take_slot(JceTerrainStore *s)
{
    for (uint32_t t = 0; t < s->cfg.max_resident_tiles; t++)
        if (!s->tiles[t].used) return &s->tiles[t];

    TsTile *victim = NULL;
    for (uint32_t t = 0; t < s->cfg.max_resident_tiles; t++) {
        TsTile *ti = &s->tiles[t];
        if (!ti->used || ti->pins > 0u) continue;
        if (!victim || ti->last_use < victim->last_use) victim = ti;
    }
    if (!victim) return NULL;   /* everything is pinned */

    s->resident_bytes -= (uint64_t)victim->w * victim->h * sizeof(float);
    JCE_FREE(victim->heights);
    victim->heights = NULL;
    victim->used = false;
    victim->generation = (victim->generation + 1u) & TS_GEN_MASK;
    if (victim->generation == 0u) victim->generation = 1u;
    s->evictions++;
    return victim;
}

/* Evict unpinned LRU tiles until the byte budget is satisfied.  Best-effort:
 * if everything left is pinned the budget is exceeded, which is reported
 * through the counters rather than by dropping a live view. */
static void ts_enforce_bytes(JceTerrainStore *s)
{
    while (s->resident_bytes > s->cfg.max_resident_bytes) {
        TsTile *victim = NULL;
        for (uint32_t t = 0; t < s->cfg.max_resident_tiles; t++) {
            TsTile *ti = &s->tiles[t];
            if (!ti->used || ti->pins > 0u) continue;
            if (!victim || ti->last_use < victim->last_use) victim = ti;
        }
        if (!victim) break;
        s->resident_bytes -= (uint64_t)victim->w * victim->h * sizeof(float);
        JCE_FREE(victim->heights);
        victim->heights = NULL;
        victim->used = false;
        victim->generation = (victim->generation + 1u) & TS_GEN_MASK;
        if (victim->generation == 0u) victim->generation = 1u;
        s->evictions++;
    }
}

bool jce_terrain_store_pin_tile(JceTerrainStore *s, JceTerrainHandle h,
                                JceTerrainTileKey key,
                                JceTerrainTileView *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!jce_terrain_store_is_valid(s, h)) return false;

    const uint32_t ai = ts_index(h);
    const uint32_t ag = s->assets[ai].generation;

    TsTile *ti = ts_find_resident(s, ai, ag, key);
    if (!ti) {
        float   *heights = NULL;
        uint32_t w = 0u, hh = 0u;
        if (!s->cfg.loader(s->cfg.loader_ctx, s->assets[ai].path, key,
                           &heights, &w, &hh))
            return false;
        if (!heights || w < 2u || hh < 2u) { JCE_FREE(heights); return false; }

        ti = ts_take_slot(s);
        if (!ti) { JCE_FREE(heights); return false; }   /* all slots pinned */

        ti->asset_index      = ai;
        ti->asset_generation = ag;
        ti->key              = key;
        ti->heights          = heights;
        ti->w                = w;
        ti->h                = hh;
        ti->pins             = 0u;
        ti->used             = true;
        s->resident_bytes += (uint64_t)w * hh * sizeof(float);
        s->loads++;
    }

    ti->pins++;
    ti->last_use = ++s->clock;

    out->key           = ti->key;
    out->sample_width  = ti->w;
    out->sample_height = ti->h;
    out->heights       = ti->heights;
    out->_slot         = (uint32_t)(ti - s->tiles);
    out->_generation   = ti->generation;

    /* Enforce the byte budget AFTER pinning, so the tile just handed out is
     * never the one evicted to make room for itself. */
    ts_enforce_bytes(s);
    return true;
}

void jce_terrain_store_unpin_tile(JceTerrainStore *s, JceTerrainTileView *view)
{
    if (!s || !view || !view->heights) return;
    if (view->_slot >= s->cfg.max_resident_tiles) return;

    TsTile *ti = &s->tiles[view->_slot];
    /* Validate the generation: unpinning a view whose slot was recycled would
     * decrement somebody else's pin count and let a live tile be evicted. */
    if (!ti->used || ti->generation != view->_generation) {
        memset(view, 0, sizeof *view);
        return;
    }
    if (ti->pins > 0u) ti->pins--;
    memset(view, 0, sizeof *view);
}

uint32_t jce_terrain_store_resident_tiles(const JceTerrainStore *s)
{
    if (!s) return 0u;
    uint32_t n = 0u;
    for (uint32_t i = 0; i < s->cfg.max_resident_tiles; i++)
        if (s->tiles[i].used) n++;
    return n;
}
uint64_t jce_terrain_store_resident_bytes(const JceTerrainStore *s)
{
    return s ? s->resident_bytes : 0ull;
}
uint64_t jce_terrain_store_evictions(const JceTerrainStore *s)
{
    return s ? s->evictions : 0ull;
}
uint64_t jce_terrain_store_loads(const JceTerrainStore *s)
{
    return s ? s->loads : 0ull;
}
