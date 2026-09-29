/*
 * test_jce_terrain_store.c
 *
 * Terrain must have ONE owner.
 *
 * Today it has three: the editor panel holds a JceTerrain*, the renderer loads
 * a second copy into a private cache, and runtime physics loads a third at
 * spawn -- which is why the physics copy never observes an editor edit.  The
 * store replaces all three.
 *
 * The tests that matter here are the ownership ones: a pinned tile must never
 * be evicted (or a borrowed view dangles), a stale handle must fail closed
 * (or it silently resolves to whoever recycled the slot), and destroy must
 * refuse while pins remain (or it frees memory a consumer is still reading).
 * Those are the failures that look like data corruption rather than like bugs.
 */

#include "jce_terrain_store.h"

#include <jce/os/core/jce_alloc.h>

#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define TW 5u
#define TH 5u

typedef struct { int loads; bool fail_key_9; } LoaderCtx;

static bool test_loader(void *ctx, const char *path, JceTerrainTileKey key,
                        float **out, uint32_t *w, uint32_t *h)
{
    LoaderCtx *lc = (LoaderCtx *)ctx;
    if (lc && lc->fail_key_9 && key.x == 9u) return false;   /* missing tile */

    /* Engine allocator: the store frees this with JCE_FREE, so a raw
     * malloc here would be a cross-allocator free. */
    float *buf = (float *)jce_malloc(sizeof(float) * TW * TH);
    if (!buf) return false;
    /* Encode the key so a test can prove it got the tile it asked for. */
    for (uint32_t i = 0; i < TW * TH; i++)
        buf[i] = (float)(key.x * 1000u + key.z) + (float)i * 0.001f;
    *out = buf; *w = TW; *h = TH;
    if (lc) lc->loads++;
    return true;
}

static JceTerrainStore *make_store(LoaderCtx *lc, uint32_t tiles,
                                   uint64_t bytes)
{
    JceTerrainStoreConfig cfg;
    jce_terrain_store_config_init(&cfg);
    cfg.max_assets         = 4u;
    cfg.max_resident_tiles = tiles;
    cfg.max_resident_bytes = bytes;
    cfg.loader             = test_loader;
    cfg.loader_ctx         = lc;
    return jce_terrain_store_create(&cfg);
}

/* ── 1. Config validation refuses nonsense ─────────────────────────── */

static void test_create_validates_config(void)
{
    JceTerrainStoreConfig cfg;
    jce_terrain_store_config_init(&cfg);
    TEST_ASSERT_NULL(jce_terrain_store_create(&cfg));   /* no loader */

    cfg.loader = test_loader;
    cfg.max_resident_bytes = 0ull;
    /* Zero must NOT be read as unlimited -- that would silently disable
     * eviction on the 512 MB profile. */
    TEST_ASSERT_NULL(jce_terrain_store_create(&cfg));

    TEST_ASSERT_NULL(jce_terrain_store_create(NULL));
}

/* ── 2. THE POINT: one path is one asset ───────────────────────────── */

static void test_same_path_is_shared(void)
{
    LoaderCtx lc = {0};
    JceTerrainStore *s = make_store(&lc, 8u, 1u << 20);

    JceTerrainHandle a = jce_terrain_store_acquire(s, "terrains/world.bin");
    JceTerrainHandle b = jce_terrain_store_acquire(s, "terrains/world.bin");
    TEST_ASSERT_EQUAL_UINT32(a.bits, b.bits);

    /* Two consumers, one load -- the whole reason the store exists. */
    JceTerrainTileKey k = { 1u, 2u };
    JceTerrainTileView va, vb;
    TEST_ASSERT_TRUE(jce_terrain_store_pin_tile(s, a, k, &va));
    TEST_ASSERT_TRUE(jce_terrain_store_pin_tile(s, b, k, &vb));
    TEST_ASSERT_EQUAL_UINT64(1u, jce_terrain_store_loads(s));
    TEST_ASSERT_EQUAL_PTR(va.heights, vb.heights);

    jce_terrain_store_unpin_tile(s, &va);
    jce_terrain_store_unpin_tile(s, &vb);
    jce_terrain_store_release(s, a);
    jce_terrain_store_release(s, b);
    TEST_ASSERT_TRUE(jce_terrain_store_destroy(s));
}

/* ── 3. A stale handle fails closed ────────────────────────────────── */

static void test_stale_handle_fails_closed(void)
{
    LoaderCtx lc = {0};
    JceTerrainStore *s = make_store(&lc, 8u, 1u << 20);

    JceTerrainHandle h = jce_terrain_store_acquire(s, "a.bin");
    TEST_ASSERT_TRUE(jce_terrain_store_is_valid(s, h));
    jce_terrain_store_release(s, h);
    TEST_ASSERT_FALSE(jce_terrain_store_is_valid(s, h));

    /* Reacquire: the slot is reused, but the OLD handle must not resolve to
     * the new asset -- that is the bug that looks like data corruption. */
    JceTerrainHandle h2 = jce_terrain_store_acquire(s, "b.bin");
    TEST_ASSERT_TRUE(jce_terrain_store_is_valid(s, h2));
    TEST_ASSERT_FALSE(jce_terrain_store_is_valid(s, h));

    JceTerrainTileKey k = { 0u, 0u };
    JceTerrainTileView v;
    TEST_ASSERT_FALSE(jce_terrain_store_pin_tile(s, h, k, &v));

    jce_terrain_store_release(s, h2);
    TEST_ASSERT_TRUE(jce_terrain_store_destroy(s));
}

/* ── 4. A pinned tile is never evicted ─────────────────────────────── */

static void test_pinned_tile_survives_pressure(void)
{
    LoaderCtx lc = {0};
    /* Two slots, then ask for four distinct tiles. */
    JceTerrainStore *s = make_store(&lc, 2u, 1u << 20);
    JceTerrainHandle h = jce_terrain_store_acquire(s, "w.bin");

    JceTerrainTileKey k0 = { 0u, 0u };
    JceTerrainTileView pinned;
    TEST_ASSERT_TRUE(jce_terrain_store_pin_tile(s, h, k0, &pinned));
    const float first = pinned.heights[0];

    for (uint32_t i = 1u; i <= 4u; i++) {
        JceTerrainTileKey k = { i, 0u };
        JceTerrainTileView v;
        TEST_ASSERT_TRUE(jce_terrain_store_pin_tile(s, h, k, &v));
        jce_terrain_store_unpin_tile(s, &v);
    }

    /* The pinned view must still be readable and still be ITS tile. */
    TEST_ASSERT_NOT_NULL(pinned.heights);
    TEST_ASSERT_EQUAL_FLOAT(first, pinned.heights[0]);
    TEST_ASSERT_TRUE(jce_terrain_store_evictions(s) > 0u);

    jce_terrain_store_unpin_tile(s, &pinned);
    jce_terrain_store_release(s, h);
    TEST_ASSERT_TRUE(jce_terrain_store_destroy(s));
}

/* ── 5. Pinning everything then asking for more fails, not corrupts ── */

static void test_all_pinned_refuses_new_tile(void)
{
    LoaderCtx lc = {0};
    JceTerrainStore *s = make_store(&lc, 2u, 1u << 20);
    JceTerrainHandle h = jce_terrain_store_acquire(s, "w.bin");

    JceTerrainTileKey k0 = { 0u, 0u }, k1 = { 1u, 0u }, k2 = { 2u, 0u };
    JceTerrainTileView v0, v1, v2;
    TEST_ASSERT_TRUE(jce_terrain_store_pin_tile(s, h, k0, &v0));
    TEST_ASSERT_TRUE(jce_terrain_store_pin_tile(s, h, k1, &v1));
    /* Both slots pinned: refuse rather than evict something in use. */
    TEST_ASSERT_FALSE(jce_terrain_store_pin_tile(s, h, k2, &v2));
    TEST_ASSERT_NULL(v2.heights);

    jce_terrain_store_unpin_tile(s, &v0);
    TEST_ASSERT_TRUE(jce_terrain_store_pin_tile(s, h, k2, &v2));

    jce_terrain_store_unpin_tile(s, &v1);
    jce_terrain_store_unpin_tile(s, &v2);
    jce_terrain_store_release(s, h);
    TEST_ASSERT_TRUE(jce_terrain_store_destroy(s));
}

/* ── 6. Destroy refuses while a view is borrowed ───────────────────── */

static void test_destroy_refuses_with_pins(void)
{
    LoaderCtx lc = {0};
    JceTerrainStore *s = make_store(&lc, 4u, 1u << 20);
    JceTerrainHandle h = jce_terrain_store_acquire(s, "w.bin");

    JceTerrainTileKey k = { 3u, 4u };
    JceTerrainTileView v;
    TEST_ASSERT_TRUE(jce_terrain_store_pin_tile(s, h, k, &v));

    /* Freeing here would hand the consumer a dangling pointer. */
    TEST_ASSERT_FALSE(jce_terrain_store_destroy(s));
    TEST_ASSERT_NOT_NULL(v.heights);

    jce_terrain_store_unpin_tile(s, &v);
    jce_terrain_store_release(s, h);
    TEST_ASSERT_TRUE(jce_terrain_store_destroy(s));
}

/* ── 7. Byte budget is enforced, not merely recorded ───────────────── */

static void test_byte_budget_evicts(void)
{
    LoaderCtx lc = {0};
    const uint64_t tile_bytes = (uint64_t)TW * TH * sizeof(float);
    /* Budget for ~3 tiles, 16 slots: the BYTE budget must bind, not the slots. */
    JceTerrainStore *s = make_store(&lc, 16u, tile_bytes * 3u);
    JceTerrainHandle h = jce_terrain_store_acquire(s, "w.bin");

    for (uint32_t i = 0; i < 10u; i++) {
        JceTerrainTileKey k = { i, 0u };
        JceTerrainTileView v;
        TEST_ASSERT_TRUE(jce_terrain_store_pin_tile(s, h, k, &v));
        jce_terrain_store_unpin_tile(s, &v);
    }
    TEST_ASSERT_TRUE(jce_terrain_store_resident_bytes(s) <= tile_bytes * 3u);
    TEST_ASSERT_TRUE(jce_terrain_store_resident_tiles(s) <= 3u);

    jce_terrain_store_release(s, h);
    TEST_ASSERT_TRUE(jce_terrain_store_destroy(s));
}

/* ── 8. A missing tile is reported, not faked ──────────────────────── */

static void test_missing_tile_fails(void)
{
    LoaderCtx lc = { 0, true };
    JceTerrainStore *s = make_store(&lc, 4u, 1u << 20);
    JceTerrainHandle h = jce_terrain_store_acquire(s, "w.bin");

    JceTerrainTileKey bad = { 9u, 0u };
    JceTerrainTileView v;
    TEST_ASSERT_FALSE(jce_terrain_store_pin_tile(s, h, bad, &v));
    TEST_ASSERT_NULL(v.heights);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_terrain_store_resident_tiles(s));

    jce_terrain_store_release(s, h);
    TEST_ASSERT_TRUE(jce_terrain_store_destroy(s));
}

/* ── 9. Releasing the last ref drops that asset's unpinned tiles ───── */

static void test_release_drops_tiles(void)
{
    LoaderCtx lc = {0};
    JceTerrainStore *s = make_store(&lc, 8u, 1u << 20);
    JceTerrainHandle h = jce_terrain_store_acquire(s, "w.bin");

    for (uint32_t i = 0; i < 3u; i++) {
        JceTerrainTileKey k = { i, 0u };
        JceTerrainTileView v;
        jce_terrain_store_pin_tile(s, h, k, &v);
        jce_terrain_store_unpin_tile(s, &v);
    }
    TEST_ASSERT_EQUAL_UINT32(3u, jce_terrain_store_resident_tiles(s));

    jce_terrain_store_release(s, h);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_terrain_store_resident_tiles(s));
    TEST_ASSERT_EQUAL_UINT64(0u, jce_terrain_store_resident_bytes(s));

    TEST_ASSERT_TRUE(jce_terrain_store_destroy(s));
}

/* ── 10. Unpinning a recycled view does not corrupt the new tenant ─── */

static void test_stale_view_unpin_is_safe(void)
{
    LoaderCtx lc = {0};
    JceTerrainStore *s = make_store(&lc, 1u, 1u << 20);
    JceTerrainHandle h = jce_terrain_store_acquire(s, "w.bin");

    JceTerrainTileKey k0 = { 0u, 0u }, k1 = { 1u, 0u };
    JceTerrainTileView v0;
    TEST_ASSERT_TRUE(jce_terrain_store_pin_tile(s, h, k0, &v0));
    jce_terrain_store_unpin_tile(s, &v0);

    /* Force the single slot to be recycled, then replay the OLD view. */
    JceTerrainTileView stale = v0;   /* copy taken before unpin cleared it */
    stale.heights = (const float *)1; /* non-null so the guard is reached  */

    JceTerrainTileView v1;
    TEST_ASSERT_TRUE(jce_terrain_store_pin_tile(s, h, k1, &v1));
    const uint32_t pinned_before = jce_terrain_store_pinned_tiles(s);

    jce_terrain_store_unpin_tile(s, &stale);
    /* The generation check must reject it, leaving v1's pin intact. */
    TEST_ASSERT_EQUAL_UINT32(pinned_before, jce_terrain_store_pinned_tiles(s));

    jce_terrain_store_unpin_tile(s, &v1);
    jce_terrain_store_release(s, h);
    TEST_ASSERT_TRUE(jce_terrain_store_destroy(s));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_validates_config);
    RUN_TEST(test_same_path_is_shared);
    RUN_TEST(test_stale_handle_fails_closed);
    RUN_TEST(test_pinned_tile_survives_pressure);
    RUN_TEST(test_all_pinned_refuses_new_tile);
    RUN_TEST(test_destroy_refuses_with_pins);
    RUN_TEST(test_byte_budget_evicts);
    RUN_TEST(test_missing_tile_fails);
    RUN_TEST(test_release_drops_tiles);
    RUN_TEST(test_stale_view_unpin_is_safe);
    return UNITY_END();
}
