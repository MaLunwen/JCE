/*
 * test_jce_terrain_cook_tiled.c
 *
 * Multi-tile cooking is what makes the v3 directory worth having: a consumer
 * must be able to page ONE tile without reading any of the others.  The
 * properties that make that true, and which this file asserts, are:
 *
 *   - every tile decodes to the heights that were cooked into it;
 *   - adjacent tiles AGREE on their shared edge, because each stores a
 *     one-sample overlap -- without that, a bilinear sample near a boundary
 *     needs two tiles resident and the paging buys nothing;
 *   - corruption in one tile does not affect any other;
 *   - a grid the tile size cannot cover exactly is REFUSED rather than cooked
 *     with a ragged edge.
 *
 * The end-to-end case runs the cooked bytes through the real source + store,
 * because "the codec is fine and the store is fine" has never implied the two
 * compose.
 */

#include "jce_terrain_format.h"
#include "jce_terrain_source.h"
#include "jce_terrain_store.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define SAMPLES 17u          /* 16 cells */
#define TILE     4u          /* -> 4x4 tiles of 5x5 samples */
#define BASE  (-20.0f)
#define RANGE  80.0f

static float g_h[SAMPLES * SAMPLES];
static float g_r[SAMPLES * SAMPLES];
static uint8_t *g_buf;
static size_t   g_size;

static float src_height(uint32_t x, uint32_t z)
{
    /* Distinct per sample so a mis-indexed tile cannot coincidentally match. */
    return BASE + RANGE * ((float)(x * 31u + z * 7u) /
                           (float)(SAMPLES * 31u + SAMPLES * 7u));
}

static void cook(bool with_ridge)
{
    for (uint32_t z = 0; z < SAMPLES; z++)
        for (uint32_t x = 0; x < SAMPLES; x++) {
            g_h[z * SAMPLES + x] = src_height(x, z);
            g_r[z * SAMPLES + x] = -1.0f + 2.0f * ((float)x / (float)SAMPLES);
        }
    g_size = jce_terrain_write_size_tiled(SAMPLES, SAMPLES, TILE, with_ridge);
    TEST_ASSERT_TRUE(g_size > 0u);
    g_buf = (uint8_t *)malloc(g_size);
    TEST_ASSERT_NOT_NULL(g_buf);
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK,
        jce_terrain_write_tiled(g_buf, g_size, g_h,
                                with_ridge ? g_r : NULL,
                                SAMPLES, SAMPLES, TILE,
                                128.0f, 128.0f, BASE, RANGE,
                                JCE_TERRAIN_HEIGHT_R16_UNORM,
                                JCE_TERRAIN_DIAG_ZIGZAG, NULL));
}

static void uncook(void) { free(g_buf); g_buf = NULL; }

/* ── 1. The file really is many tiles ──────────────────────────────────  */

static void test_header_reports_the_tile_grid(void)
{
    cook(false);
    JceTerrainHeader h;
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK,
        jce_terrain_read_header(g_buf, g_size, &h));
    TEST_ASSERT_EQUAL_UINT32(4u, h.tile_count_x);
    TEST_ASSERT_EQUAL_UINT32(4u, h.tile_count_z);
    TEST_ASSERT_EQUAL_UINT32(16u, h.tile_count);
    TEST_ASSERT_EQUAL_UINT32(SAMPLES, h.width_samples);
    /* The diagonal rule must survive: collider, raycast and render mesh all
     * key off it, and they must agree with each other. */
    TEST_ASSERT_EQUAL_UINT8(JCE_TERRAIN_DIAG_ZIGZAG, h.diagonal_rule);
    uncook();
}

/* ── 2. Every tile decodes to what was cooked into it ──────────────────  */

static void test_every_tile_decodes_correctly(void)
{
    cook(false);
    JceTerrainHeader h;
    JceTerrainDirEntry e[16];
    jce_terrain_read_header(g_buf, g_size, &h);
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK,
        jce_terrain_read_directory(g_buf, g_size, &h, e));

    const float tol = RANGE / 65535.0f * 1.5f;
    for (uint32_t i = 0; i < h.tile_count; i++) {
        float got[(TILE + 1u) * (TILE + 1u)];
        TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK,
            jce_terrain_read_tile_heights(g_buf, g_size, &h, &e[i], got));

        const uint32_t ox = e[i].tile_x * TILE;
        const uint32_t oz = e[i].tile_z * TILE;
        for (uint32_t sz = 0; sz < TILE + 1u; sz++)
            for (uint32_t sx = 0; sx < TILE + 1u; sx++)
                TEST_ASSERT_FLOAT_WITHIN(tol, src_height(ox + sx, oz + sz),
                                         got[sz * (TILE + 1u) + sx]);
    }
    uncook();
}

/* ── 3. THE POINT OF THE OVERLAP: neighbours agree on the shared edge ──
 *
 * Each tile stores one sample past its own cell range.  If that overlap were
 * dropped, interpolating anywhere near a boundary would need the neighbouring
 * tile resident too -- which is precisely the cost the tiling exists to avoid. */

static void test_adjacent_tiles_agree_on_the_seam(void)
{
    cook(false);
    JceTerrainHeader h;
    JceTerrainDirEntry e[16];
    jce_terrain_read_header(g_buf, g_size, &h);
    jce_terrain_read_directory(g_buf, g_size, &h, e);

    const uint32_t span = TILE + 1u;
    float a[(TILE + 1u) * (TILE + 1u)], b[(TILE + 1u) * (TILE + 1u)];

    /* Tile (0,0) and its right neighbour (1,0). */
    const JceTerrainDirEntry *ea = NULL, *eb = NULL;
    for (uint32_t i = 0; i < h.tile_count; i++) {
        if (e[i].tile_x == 0u && e[i].tile_z == 0u) ea = &e[i];
        if (e[i].tile_x == 1u && e[i].tile_z == 0u) eb = &e[i];
    }
    TEST_ASSERT_NOT_NULL(ea);
    TEST_ASSERT_NOT_NULL(eb);
    jce_terrain_read_tile_heights(g_buf, g_size, &h, ea, a);
    jce_terrain_read_tile_heights(g_buf, g_size, &h, eb, b);

    /* a's LAST column is b's FIRST column, sample for sample. */
    for (uint32_t sz = 0; sz < span; sz++)
        TEST_ASSERT_EQUAL_FLOAT(a[sz * span + (span - 1u)], b[sz * span + 0u]);

    uncook();
}

/* ── 4. Corruption stays contained ─────────────────────────────────────  */

static void test_corruption_is_local_to_one_tile(void)
{
    cook(false);
    JceTerrainHeader h;
    JceTerrainDirEntry e[16];
    jce_terrain_read_header(g_buf, g_size, &h);
    jce_terrain_read_directory(g_buf, g_size, &h, e);

    /* Damage tile index 5 only. */
    g_buf[e[5].stored_offset + JCE_TERRAIN_TILEHDR_BYTES + 3u] ^= 0x20u;

    float got[(TILE + 1u) * (TILE + 1u)];
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_ERR_TILE_HASH,
        jce_terrain_read_tile_heights(g_buf, g_size, &h, &e[5], got));

    /* Every other tile still decodes -- one bad tile must not condemn the
     * file, or streaming has no way to survive a partial fault. */
    for (uint32_t i = 0; i < h.tile_count; i++) {
        if (i == 5u) continue;
        TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK,
            jce_terrain_read_tile_heights(g_buf, g_size, &h, &e[i], got));
    }
    uncook();
}

/* ── 5. Ridge is tiled the same way ────────────────────────────────────  */

static void test_ridge_is_tiled_too(void)
{
    cook(true);
    JceTerrainHeader h;
    JceTerrainDirEntry e[16];
    jce_terrain_read_header(g_buf, g_size, &h);
    TEST_ASSERT_TRUE((h.channel_mask & JCE_TERRAIN_CH_RIDGE) != 0u);
    jce_terrain_read_directory(g_buf, g_size, &h, e);

    const float tol = 2.0f / 65535.0f * 1.5f;
    for (uint32_t i = 0; i < h.tile_count; i++) {
        float got[(TILE + 1u) * (TILE + 1u)];
        TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_OK,
            jce_terrain_read_tile_ridge(g_buf, g_size, &h, &e[i], got));
        const uint32_t ox = e[i].tile_x * TILE;
        for (uint32_t sx = 0; sx < TILE + 1u; sx++) {
            const float want = -1.0f + 2.0f * ((float)(ox + sx) / (float)SAMPLES);
            TEST_ASSERT_FLOAT_WITHIN(tol, want, got[sx]);
        }
    }
    uncook();
}

/* ── 6. THE SEAM: cooked tiles reach a consumer through the real store ──  */

static void test_end_to_end_through_source_and_store(void)
{
    cook(true);
    JceTerrainSource *src = jce_terrain_source_open_memory(g_buf, g_size);
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_EQUAL_UINT32(4u, jce_terrain_source_tiles_x(src));
    TEST_ASSERT_EQUAL_UINT32(4u, jce_terrain_source_tiles_z(src));

    JceTerrainStoreConfig cfg;
    jce_terrain_store_config_init(&cfg);
    cfg.max_assets         = 2u;
    cfg.max_resident_tiles = 4u;      /* deliberately fewer than 16 */
    cfg.max_resident_bytes = 1u << 20;
    cfg.loader             = jce_terrain_source_load_tile;
    cfg.loader_ctx         = src;

    JceTerrainStore *store = jce_terrain_store_create(&cfg);
    TEST_ASSERT_NOT_NULL(store);
    JceTerrainHandle han = jce_terrain_store_acquire(store, "t/big.bin");
    TEST_ASSERT_TRUE(jce_terrain_store_is_valid(store, han));

    /* Page through every tile with a budget that cannot hold them all: this
     * is the case the whole format exists for. */
    const float tol = RANGE / 65535.0f * 1.5f;
    for (uint32_t tz = 0; tz < 4u; tz++) {
        for (uint32_t tx = 0; tx < 4u; tx++) {
            JceTerrainTileKey k = { tx, tz };
            JceTerrainTileView v;
            TEST_ASSERT_TRUE(jce_terrain_store_pin_tile(store, han, k, &v));
            TEST_ASSERT_EQUAL_UINT32(TILE + 1u, v.sample_width);
            TEST_ASSERT_FLOAT_WITHIN(tol, src_height(tx * TILE, tz * TILE),
                                     v.heights[0]);
            jce_terrain_store_unpin_tile(store, &v);
        }
    }
    TEST_ASSERT_TRUE(jce_terrain_store_resident_tiles(store) <= 4u);

    jce_terrain_store_release(store, han);
    TEST_ASSERT_TRUE(jce_terrain_store_destroy(store));
    jce_terrain_source_close(src);
    uncook();
}

/* ── 7. A grid the tile size cannot cover exactly is refused ───────────  */

static void test_indivisible_grid_refused(void)
{
    /* 17 samples = 16 cells; 5 does not divide 16. */
    TEST_ASSERT_EQUAL_UINT32(0u,
        (uint32_t)jce_terrain_write_size_tiled(SAMPLES, SAMPLES, 5u, false));

    float h[SAMPLES * SAMPLES];
    for (uint32_t i = 0; i < SAMPLES * SAMPLES; i++) h[i] = 0.0f;
    uint8_t small[256];
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_ERR_ARGS,
        jce_terrain_write_tiled(small, sizeof small, h, NULL,
                                SAMPLES, SAMPLES, 5u, 128.0f, 128.0f,
                                BASE, RANGE, JCE_TERRAIN_HEIGHT_R16_UNORM,
                                JCE_TERRAIN_DIAG_FIXED, NULL));
    /* And a zero tile size is refused rather than dividing by zero. */
    TEST_ASSERT_EQUAL_INT(JCE_TERRAIN_ERR_ARGS,
        jce_terrain_write_tiled(small, sizeof small, h, NULL,
                                SAMPLES, SAMPLES, 0u, 128.0f, 128.0f,
                                BASE, RANGE, JCE_TERRAIN_HEIGHT_R16_UNORM,
                                JCE_TERRAIN_DIAG_FIXED, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_header_reports_the_tile_grid);
    RUN_TEST(test_every_tile_decodes_correctly);
    RUN_TEST(test_adjacent_tiles_agree_on_the_seam);
    RUN_TEST(test_corruption_is_local_to_one_tile);
    RUN_TEST(test_ridge_is_tiled_too);
    RUN_TEST(test_end_to_end_through_source_and_store);
    RUN_TEST(test_indivisible_grid_refused);
    return UNITY_END();
}
