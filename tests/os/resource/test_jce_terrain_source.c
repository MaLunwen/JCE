/*
 * test_jce_terrain_source.c
 *
 * End-to-end: cooked v3 bytes -> source -> store -> a pinned tile view.
 *
 * This is the seam the whole terrain line rests on.  The codec and the store
 * are each tested in isolation elsewhere; what matters here is that they
 * compose -- that heights written by the cooker are the heights a consumer
 * eventually reads, through the residency layer, with corruption still caught
 * on the way.
 */

#include "jce_terrain_source.h"
#include "jce_terrain_format.h"
#include "jce_terrain_store.h"

#include <jce/os/core/jce_alloc.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define SX 9u
#define SZ 7u
#define BASE  (-10.0f)
#define RANGE 40.0f

static uint8_t *g_file;
static size_t   g_file_size;
static float    g_src[SX * SZ];

static void cook(void)
{
    for (uint32_t z = 0; z < SZ; z++)
        for (uint32_t x = 0; x < SX; x++)
            g_src[z * SX + x] =
                BASE + RANGE * ((float)(x + z * 2u) / (float)(SX + SZ * 2u));

    g_file_size = jce_terrain_write_size_height_only(SX, SZ);
    g_file = (uint8_t *)malloc(g_file_size);
    TEST_ASSERT_NOT_NULL(g_file);
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK,
        jce_terrain_write_height_only(g_file, g_file_size, g_src, SX, SZ,
                                      64.0f, 64.0f, BASE, RANGE,
                                      JCE_TERRAIN_HEIGHT_R16_UNORM,
                                      JCE_TERRAIN_DIAG_ZIGZAG, NULL));
}

static void uncook(void) { free(g_file); g_file = NULL; }

/* ── 1. A valid file opens and reports its shape ───────────────────── */

static void test_open_reports_shape(void)
{
    cook();
    JceTerrainSource *s = jce_terrain_source_open_memory(g_file, g_file_size);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_UINT32(1u, jce_terrain_source_tiles_x(s));
    TEST_ASSERT_EQUAL_UINT32(1u, jce_terrain_source_tiles_z(s));
    TEST_ASSERT_EQUAL_FLOAT(BASE, jce_terrain_source_base_height(s));
    TEST_ASSERT_EQUAL_FLOAT(RANGE, jce_terrain_source_height_range(s));
    /* The diagonal rule must survive the round trip: renderer, raycast, hole
     * mesh and collider all key off it. */
    TEST_ASSERT_EQUAL_UINT8(JCE_TERRAIN_DIAG_ZIGZAG,
                            jce_terrain_source_diagonal_rule(s));
    jce_terrain_source_close(s);
    uncook();
}

/* ── 2. A corrupt file fails at OPEN, not at first pin ─────────────── */

static void test_corrupt_file_fails_at_open(void)
{
    cook();
    g_file[16] ^= 0x01u;   /* flip a dimension: header hash must catch it */
    TEST_ASSERT_NULL(jce_terrain_source_open_memory(g_file, g_file_size));
    uncook();

    /* Truncation likewise. */
    cook();
    TEST_ASSERT_NULL(jce_terrain_source_open_memory(g_file, g_file_size / 2u));
    uncook();
}

/* ── 3. THE SEAM: cooked heights reach a pinned view intact ────────── */

static void test_end_to_end_through_the_store(void)
{
    cook();
    JceTerrainSource *src = jce_terrain_source_open_memory(g_file, g_file_size);
    TEST_ASSERT_NOT_NULL(src);

    JceTerrainStoreConfig cfg;
    jce_terrain_store_config_init(&cfg);
    cfg.max_assets         = 2u;
    cfg.max_resident_tiles = 4u;
    cfg.max_resident_bytes = 1u << 20;
    cfg.loader             = jce_terrain_source_load_tile;
    cfg.loader_ctx         = src;

    JceTerrainStore *store = jce_terrain_store_create(&cfg);
    TEST_ASSERT_NOT_NULL(store);

    JceTerrainHandle h = jce_terrain_store_acquire(store, "terrains/t.bin");
    TEST_ASSERT_TRUE(jce_terrain_store_is_valid(store, h));

    JceTerrainTileKey k = { 0u, 0u };
    JceTerrainTileView v;
    TEST_ASSERT_TRUE(jce_terrain_store_pin_tile(store, h, k, &v));
    TEST_ASSERT_EQUAL_UINT32(SX, v.sample_width);
    TEST_ASSERT_EQUAL_UINT32(SZ, v.sample_height);
    TEST_ASSERT_NOT_NULL(v.heights);

    /* Heights arrive as LOCAL-SPACE Y, never as normalised storage values. */
    const float tol = RANGE / 65535.0f * 1.5f;
    for (uint32_t i = 0; i < SX * SZ; i++)
        TEST_ASSERT_FLOAT_WITHIN(tol, g_src[i], v.heights[i]);

    jce_terrain_store_unpin_tile(store, &v);
    jce_terrain_store_release(store, h);
    TEST_ASSERT_TRUE(jce_terrain_store_destroy(store));
    jce_terrain_source_close(src);
    uncook();
}

/* ── 4. A tile that does not exist is refused, not invented ────────── */

static void test_missing_tile_refused(void)
{
    cook();
    JceTerrainSource *src = jce_terrain_source_open_memory(g_file, g_file_size);

    JceTerrainStoreConfig cfg;
    jce_terrain_store_config_init(&cfg);
    cfg.max_assets         = 2u;
    cfg.max_resident_tiles = 4u;
    cfg.max_resident_bytes = 1u << 20;
    cfg.loader             = jce_terrain_source_load_tile;
    cfg.loader_ctx         = src;
    JceTerrainStore *store = jce_terrain_store_create(&cfg);
    JceTerrainHandle h = jce_terrain_store_acquire(store, "t.bin");

    JceTerrainTileKey bad = { 5u, 5u };
    JceTerrainTileView v;
    TEST_ASSERT_FALSE(jce_terrain_store_pin_tile(store, h, bad, &v));
    TEST_ASSERT_NULL(v.heights);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_terrain_store_resident_tiles(store));

    jce_terrain_store_release(store, h);
    TEST_ASSERT_TRUE(jce_terrain_store_destroy(store));
    jce_terrain_source_close(src);
    uncook();
}

/* ── 5. Tile corruption is caught before publication ───────────────── */

static void test_tile_corruption_never_published(void)
{
    cook();
    /* Open FIRST (header+directory valid), then corrupt a height sample so the
     * failure can only surface at tile decode -- which must refuse rather than
     * hand out a half-decoded tile. */
    JceTerrainSource *src = jce_terrain_source_open_memory(g_file, g_file_size);
    TEST_ASSERT_NOT_NULL(src);

    JceTerrainHeader hdr;
    JceTerrainDirEntry e;
    jce_terrain_read_header(g_file, g_file_size, &hdr);
    jce_terrain_read_directory(g_file, g_file_size, &hdr, &e);
    g_file[e.stored_offset + JCE_TERRAIN_TILEHDR_BYTES + 2] ^= 0x40u;

    JceTerrainStoreConfig cfg;
    jce_terrain_store_config_init(&cfg);
    cfg.max_assets         = 2u;
    cfg.max_resident_tiles = 4u;
    cfg.max_resident_bytes = 1u << 20;
    cfg.loader             = jce_terrain_source_load_tile;
    cfg.loader_ctx         = src;
    JceTerrainStore *store = jce_terrain_store_create(&cfg);
    JceTerrainHandle h = jce_terrain_store_acquire(store, "t.bin");

    JceTerrainTileKey k = { 0u, 0u };
    JceTerrainTileView v;
    TEST_ASSERT_FALSE(jce_terrain_store_pin_tile(store, h, k, &v));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_terrain_store_resident_tiles(store));

    jce_terrain_store_release(store, h);
    TEST_ASSERT_TRUE(jce_terrain_store_destroy(store));
    jce_terrain_source_close(src);
    uncook();
}

/* ── 6. NULL safety ────────────────────────────────────────────────── */

static void test_null_safety(void)
{
    TEST_ASSERT_NULL(jce_terrain_source_open_memory(NULL, 0u));
    TEST_ASSERT_NULL(jce_terrain_source_open(NULL, NULL));
    jce_terrain_source_close(NULL);     /* must not crash */

    float *hp = NULL; uint32_t w = 0u, hh = 0u;
    JceTerrainTileKey k = { 0u, 0u };
    TEST_ASSERT_FALSE(jce_terrain_source_load_tile(NULL, "x", k, &hp, &w, &hh));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_terrain_source_tiles_x(NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_open_reports_shape);
    RUN_TEST(test_corrupt_file_fails_at_open);
    RUN_TEST(test_end_to_end_through_the_store);
    RUN_TEST(test_missing_tile_refused);
    RUN_TEST(test_tile_corruption_never_published);
    RUN_TEST(test_null_safety);
    return UNITY_END();
}
