/* test_jce_terrain_tiling.c
 *
 * Unit tests for tiled / streamed terrain (large-world #4):
 *   - a tiled terrain whose load callback serves the SAME grid values as a
 *     monolithic terrain produces identical sample_height / chunk-mesh results
 *     (proves the read-accessor tile chokepoint indexes tiles correctly,
 *     including the overlapped tile edges)
 *   - sample_splat reads through the tile path too
 *   - LRU eviction keeps the resident-tile count within budget across a full
 *     traversal, and evicted tiles re-load correctly on the next touch
 *   - create guards (tile_dim must divide the grid; load_fn required)
 *
 * Exercises the REAL terrain sample/mesh path against an in-memory tile source;
 * no editor / GPU / disk.
 */

#include <jce/middleware/scene/jce_terrain.h>

#include "unity.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ---- deterministic procedural grid oracle (per-vertex distinct) ---- */

static uint16_t oracle_r16(int gx, int gz)
{
    /* coprime mix so neighbours differ, wrapped to a 0..65535 ramp */
    unsigned m = (unsigned)((gx * 7 + gz * 13) % 257);
    return (uint16_t)((m * 65535u) / 256u);
}
static uint32_t oracle_splat(int gx, int gz)
{
    int layer = ((gx + gz) & 3);          /* 0..3 */
    return (uint32_t)0xFFu << (layer * 8); /* single fully-weighted layer */
}

/* Tile load callback: fills the (span*span) overlapped tile block for
 * tile (tx,tz) directly from the oracle — the same values a monolithic
 * import of the oracle produces, so the two terrains must agree. */
static int g_load_calls = 0;
static bool tile_load(void *ud, int tx, int tz,
                      float *h_out, uint32_t *s_out, int span)
{
    (void)ud;
    g_load_calls++;
    int td = span - 1;
    for (int lz = 0; lz < span; ++lz)
        for (int lx = 0; lx < span; ++lx) {
            int gx = tx * td + lx, gz = tz * td + lz;
            h_out[lz * span + lx] = (float)oracle_r16(gx, gz) / 65535.0f;
            s_out[lz * span + lx] = oracle_splat(gx, gz);
        }
    return true;
}

static float vx(int i, int W, float wsx) { return (float)i / (float)(W - 1) * wsx; }
static float vz(int j, int H, float wsz) { return (float)j / (float)(H - 1) * wsz; }

/* Build the monolithic reference seeded from the same oracle. */
static JceTerrain *make_monolithic(int W, int H, float wsx, float wsz,
                                   float maxh, int cs)
{
    JceTerrain *m = jce_terrain_create(W, H, wsx, wsz, maxh, cs);
    if (!m) return NULL;
    uint16_t *src = (uint16_t *)malloc((size_t)W * H * sizeof(uint16_t));
    if (!src) { jce_terrain_free(m); return NULL; }
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i)
            src[(size_t)j * W + i] = oracle_r16(i, j);
    jce_terrain_import_heightmap_r16(m, src, W, H);
    free(src);
    return m;
}

static void test_tiled_matches_monolithic(void)
{
    const int   W = 257, H = 257, TD = 64, CS = 64;
    const float WSX = 1024.0f, WSZ = 1024.0f, MAXH = 80.0f;

    JceTerrain *mono = make_monolithic(W, H, WSX, WSZ, MAXH, CS);
    TEST_ASSERT_NOT_NULL(mono);

    /* generous budget: no eviction, isolate correctness from paging */
    JceTerrain *tile = jce_terrain_create_tiled(W, H, WSX, WSZ, MAXH, CS,
                                                TD, 0, tile_load, NULL);
    TEST_ASSERT_NOT_NULL(tile);

    /* (a) sample_height agreement across vertices AND interpolated midpoints,
     * including tile-boundary columns/rows (multiples of TD) where the
     * overlapped-tile indexing must line up. */
    for (int j = 0; j < H; j += 7) {
        for (int i = 0; i < W; i += 7) {
            float wxv = vx(i, W, WSX), wzv = vz(j, H, WSZ);
            TEST_ASSERT_FLOAT_WITHIN(1.0e-4f,
                jce_terrain_sample_height(mono, wxv, wzv),
                jce_terrain_sample_height(tile, wxv, wzv));
        }
    }
    /* explicit boundary + midpoint probes */
    const float wstep_x = WSX / (float)(W - 1);
    const float wstep_z = WSZ / (float)(H - 1);
    for (int k = 0; k < 6; ++k) {
        float wxv = (float)(TD * (k % 4)) * wstep_x + 0.5f * wstep_x; /* mid-cell at a tile edge */
        float wzv = (float)(TD * (k % 4)) * wstep_z + 0.37f * wstep_z;
        TEST_ASSERT_FLOAT_WITHIN(1.0e-4f,
            jce_terrain_sample_height(mono, wxv, wzv),
            jce_terrain_sample_height(tile, wxv, wzv));
    }

    /* (b) chunk-mesh agreement (a separate consumer of the accessor + normals) */
    int vc = 0, ic = 0;
    jce_terrain_chunk_mesh_size(mono, 1, 1, 0, &vc, &ic);
    TEST_ASSERT_TRUE(vc > 0);
    JceTerrainVertex *vm = (JceTerrainVertex *)malloc((size_t)vc * sizeof *vm);
    JceTerrainVertex *vt = (JceTerrainVertex *)malloc((size_t)vc * sizeof *vt);
    uint32_t *im = (uint32_t *)malloc((size_t)ic * sizeof *im);
    uint32_t *it = (uint32_t *)malloc((size_t)ic * sizeof *it);
    TEST_ASSERT_NOT_NULL(vm); TEST_ASSERT_NOT_NULL(vt);
    int ov = 0, oi = 0, ov2 = 0, oi2 = 0;
    jce_terrain_chunk_build_mesh(mono, 1, 1, 0, vm, vc, im, ic, &ov, &oi);
    jce_terrain_chunk_build_mesh(tile, 1, 1, 0, vt, vc, it, ic, &ov2, &oi2);
    TEST_ASSERT_EQUAL_INT(ov, ov2);
    for (int v = 0; v < ov; ++v) {
        TEST_ASSERT_FLOAT_WITHIN(1.0e-3f, vm[v].py, vt[v].py);  /* height */
        TEST_ASSERT_FLOAT_WITHIN(1.0e-3f, vm[v].ny, vt[v].ny);  /* normal */
    }
    free(vm); free(vt); free(im); free(it);

    /* (c) splat reads through the tile path: at an exact vertex the bilinear
     * collapses to that vertex's single-layer weight. */
    for (int k = 0; k < 8; ++k) {
        int i = (k * 31) % W, j = (k * 17) % H;
        float ws[4] = {0,0,0,0};
        jce_terrain_sample_splat(tile, vx(i, W, WSX), vz(j, H, WSZ), ws);
        int layer = ((i + j) & 3);
        TEST_ASSERT_FLOAT_WITHIN(1.0e-3f, 1.0f, ws[layer]);
    }

    jce_terrain_free(mono);
    jce_terrain_free(tile);
}

static void test_eviction_respects_budget(void)
{
    const int   W = 257, H = 257, TD = 64, CS = 64;
    const float WSX = 1024.0f, WSZ = 1024.0f, MAXH = 50.0f;
    const int   BUDGET = 4;                 /* 16 tiles total, only 4 resident */

    JceTerrain *t = jce_terrain_create_tiled(W, H, WSX, WSZ, MAXH, CS,
                                             TD, BUDGET, tile_load, NULL);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_INT(0, jce_terrain_resident_tiles(t));

    /* Touch every tile (sample each tile's centre) — residency must stay capped. */
    for (int tz = 0; tz < 4; ++tz)
        for (int tx = 0; tx < 4; ++tx) {
            int gi = tx * TD + TD / 2, gj = tz * TD + TD / 2;
            (void)jce_terrain_sample_height(t, vx(gi, W, WSX), vz(gj, H, WSZ));
            TEST_ASSERT_TRUE(jce_terrain_resident_tiles(t) <= BUDGET);
        }
    TEST_ASSERT_TRUE(jce_terrain_resident_tiles(t) <= BUDGET);

    /* An evicted tile re-loads with correct data on the next touch. */
    JceTerrain *mono = make_monolithic(W, H, WSX, WSZ, MAXH, CS);
    TEST_ASSERT_NOT_NULL(mono);
    int gi = 0 * TD + TD / 2, gj = 0 * TD + TD / 2;  /* tile (0,0), long-since evicted */
    TEST_ASSERT_FLOAT_WITHIN(1.0e-4f,
        jce_terrain_sample_height(mono, vx(gi, W, WSX), vz(gj, H, WSZ)),
        jce_terrain_sample_height(t,    vx(gi, W, WSX), vz(gj, H, WSZ)));
    TEST_ASSERT_TRUE(jce_terrain_resident_tiles(t) <= BUDGET);

    jce_terrain_free(mono);
    jce_terrain_free(t);
}

static void test_create_tiled_guards(void)
{
    /* tile_dim must divide (W-1) and (H-1) */
    TEST_ASSERT_NULL(jce_terrain_create_tiled(257, 257, 100, 100, 10, 64,
                                              50 /*bad*/, 0, tile_load, NULL));
    /* load_fn required */
    TEST_ASSERT_NULL(jce_terrain_create_tiled(257, 257, 100, 100, 10, 64,
                                              64, 0, NULL, NULL));
    /* degenerate dims */
    TEST_ASSERT_NULL(jce_terrain_create_tiled(1, 1, 100, 100, 10, 64,
                                              64, 0, tile_load, NULL));
    /* a valid one frees cleanly with zero tiles touched */
    JceTerrain *ok = jce_terrain_create_tiled(129, 129, 100, 100, 10, 32,
                                              32, 8, tile_load, NULL);
    TEST_ASSERT_NOT_NULL(ok);
    TEST_ASSERT_EQUAL_INT(0, jce_terrain_resident_tiles(ok));
    jce_terrain_free(ok);
}

/* The public tile-data accessors (is_tiled / tile_grid / tile_copy) the renderer
 * uses to build per-tile splat GPU textures: tile_copy must return exactly the
 * oracle block for the tile's global coords, and reject monolithic / OOB. */
static void test_tile_accessors(void)
{
    const int W = 129, H = 129, TD = 32, CS = 32;
    JceTerrain *t = jce_terrain_create_tiled(W, H, 512, 512, 40, CS,
                                             TD, 0, tile_load, NULL);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_TRUE(jce_terrain_is_tiled(t));

    int tx = 0, tz = 0, td = 0;
    jce_terrain_tile_grid(t, &tx, &tz, &td);
    TEST_ASSERT_EQUAL_INT((W - 1) / TD, tx);
    TEST_ASSERT_EQUAL_INT((H - 1) / TD, tz);
    TEST_ASSERT_EQUAL_INT(TD, td);

    const int span = TD + 1;
    float    *h = (float *)malloc((size_t)span * span * sizeof(float));
    uint32_t *s = (uint32_t *)malloc((size_t)span * span * sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(h); TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(jce_terrain_tile_copy(t, 1, 2, h, s));
    for (int lz = 0; lz < span; ++lz)
        for (int lx = 0; lx < span; ++lx) {
            int gx = 1 * TD + lx, gz = 2 * TD + lz;
            TEST_ASSERT_FLOAT_WITHIN(1.0e-6f,
                (float)oracle_r16(gx, gz) / 65535.0f, h[lz * span + lx]);
            TEST_ASSERT_EQUAL_UINT32(oracle_splat(gx, gz), s[lz * span + lx]);
        }

    /* out-of-range tile rejected */
    TEST_ASSERT_FALSE(jce_terrain_tile_copy(t, 99, 0, h, s));
    /* monolithic terrain: not tiled, tile_copy refused */
    JceTerrain *m = jce_terrain_create(W, H, 512, 512, 40, CS);
    TEST_ASSERT_FALSE(jce_terrain_is_tiled(m));
    TEST_ASSERT_FALSE(jce_terrain_tile_copy(m, 0, 0, h, s));

    free(h); free(s);
    jce_terrain_free(m);
    jce_terrain_free(t);
}

/* Procedural tiled terrain: streams from built-in noise (no LoadFn passed by the
 * caller), is deterministic across an eviction cycle, and stays within budget. */
static void test_procedural_tiled(void)
{
    const float MAXH = 60.0f, WS = 1024.0f;
    JceTerrain *t = jce_terrain_create_procedural(257, 257, WS, WS, MAXH,
                                                  64, 4, 7u, 0.02f);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_TRUE(jce_terrain_is_tiled(t));

    float a = jce_terrain_sample_height(t, 500.0f, 500.0f);
    TEST_ASSERT_TRUE(a >= 0.0f && a <= MAXH);

    /* touch all 16 tiles (budget 4) to force eviction */
    for (int tz = 0; tz < 4; ++tz)
        for (int tx = 0; tx < 4; ++tx) {
            int gi = tx * 64 + 32, gj = tz * 64 + 32;
            (void)jce_terrain_sample_height(t, vx(gi, 257, WS), vz(gj, 257, WS));
            TEST_ASSERT_TRUE(jce_terrain_resident_tiles(t) <= 4);
        }
    /* tile (500,500) long-since evicted → re-loads to the SAME value (deterministic) */
    float a2 = jce_terrain_sample_height(t, 500.0f, 500.0f);
    TEST_ASSERT_FLOAT_WITHIN(1.0e-5f, a, a2);

    jce_terrain_free(t);
}

/* Streaming prefetch: jce_terrain_prefetch pages in exactly the tiles within the
 * radius (with clamping), refreshes their LRU, and is a no-op on monolithic. */
static void test_prefetch_pages_in_range(void)
{
    const float WS = 1024.0f;                 /* 257² grid, tile_dim 64 -> 4x4 tiles */
    JceTerrain *t = jce_terrain_create_tiled(257, 257, WS, WS, 50, 64,
                                             64, 0, tile_load, NULL);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_INT(0, jce_terrain_resident_tiles(t));

    /* corner (world 0,0) -> centre tile (0,0); radius 100 -> rt=1 ->
     * tiles {(0,0),(1,0),(0,1),(1,1)} = 4 (negative indices clamped away). */
    jce_terrain_prefetch(t, 0.0f, 0.0f, 100.0f);
    TEST_ASSERT_EQUAL_INT(4, jce_terrain_resident_tiles(t));

    /* prefetching the same region again is idempotent (no new tiles). */
    jce_terrain_prefetch(t, 0.0f, 0.0f, 100.0f);
    TEST_ASSERT_EQUAL_INT(4, jce_terrain_resident_tiles(t));

    /* monolithic terrain: prefetch is a safe no-op. */
    JceTerrain *m = jce_terrain_create(257, 257, WS, WS, 50, 64);
    jce_terrain_prefetch(m, 0.0f, 0.0f, 100.0f);
    TEST_ASSERT_EQUAL_INT(0, jce_terrain_resident_tiles(m));

    jce_terrain_free(m);
    jce_terrain_free(t);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tiled_matches_monolithic);
    RUN_TEST(test_eviction_respects_budget);
    RUN_TEST(test_create_tiled_guards);
    RUN_TEST(test_tile_accessors);
    RUN_TEST(test_procedural_tiled);
    RUN_TEST(test_prefetch_pages_in_range);
    return UNITY_END();
}
