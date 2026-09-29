/* test_jce_forwardplus_upload.c
 *
 * Validation for the Forward+ clustered-lighting GPU PACKING (ROUND A) — the
 * pure, headless function (jce_forwardplus_pack) that flattens a finished
 * jce_light_cluster build into the EXACT RGBA32F texel arrays the three GPU
 * data textures (grid / index / params) will hold.  No GPU / bgfx device is
 * needed: this exercises only the CPU build + the packing math, asserting the
 * bit-for-bit bytes a future fs_pbr.sc (ROUND B) will sample.
 *
 * Reuses the jce_light_cluster identity-camera harness (see
 * test_jce_light_cluster.c): identity view/proj => view space == world space,
 * +Z forward into [near, far], tan_half = 1.
 */

#include <jce/renderer/jce_forwardplus.h>
#include <jce/renderer/jce_light_cluster.h>
#include <jce/os/core/jce_math.h>

#include "unity.h"

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Shared identity-camera cluster builder ──────────────────────────────── */

static JceLightCluster *make_cluster(uint32_t cx, uint32_t cy, uint32_t cz,
                                     uint32_t maxpc)
{
    JceLightClusterDesc d = { cx, cy, cz, 256u, maxpc };
    JceLightCluster *lc = jce_light_cluster_create(&d);
    TEST_ASSERT_NOT_NULL(lc);
    jce_mat4 I = jce_m4_identity();
    jce_light_cluster_set_camera(lc, &I, &I, 1.0f, 100.0f);
    return lc;
}

/* ── alloc/free shape ────────────────────────────────────────────────────── */

static void test_packed_alloc_shape(void)
{
    JceForwardPlusPacked p;
    TEST_ASSERT_TRUE(jce_forwardplus_packed_alloc(&p, 8, 8, 16, 16, 3));
    TEST_ASSERT_EQUAL_UINT32(8u * 8u * 16u, p.grid_texels);
    TEST_ASSERT_EQUAL_UINT32(8u * 8u * 16u * 16u, p.index_texels);
    TEST_ASSERT_EQUAL_UINT32(3u * JCE_FP_TEXELS_PER_LIGHT, p.param_texels);
    TEST_ASSERT_EQUAL_UINT32(3u, p.light_count);
    TEST_ASSERT_NOT_NULL(p.grid);
    TEST_ASSERT_NOT_NULL(p.index);
    TEST_ASSERT_NOT_NULL(p.params);
    jce_forwardplus_packed_free(&p);
    TEST_ASSERT_NULL(p.grid);

    /* Bad args. */
    JceForwardPlusPacked q;
    TEST_ASSERT_FALSE(jce_forwardplus_packed_alloc(&q, 0, 8, 16, 16, 3));
    TEST_ASSERT_FALSE(jce_forwardplus_packed_alloc(NULL, 8, 8, 16, 16, 3));
}

/* ── shape-mismatch guard ────────────────────────────────────────────────── */

static void test_pack_shape_mismatch_rejected(void)
{
    JceLightCluster *lc = make_cluster(4, 4, 4, 8);
    TEST_ASSERT_TRUE(jce_light_cluster_build(lc, NULL, 0));
    JceLightClusterResult r = jce_light_cluster_get_result(lc);

    JceForwardPlusPacked p;
    /* Allocated for a DIFFERENT grid (8x8x16) than the result (4x4x4). */
    TEST_ASSERT_TRUE(jce_forwardplus_packed_alloc(&p, 8, 8, 16, 8, 0));
    TEST_ASSERT_FALSE(jce_forwardplus_pack(&p, &r, NULL, NULL, 0, 1.0f, 100.0f));

    jce_forwardplus_packed_free(&p);
    jce_light_cluster_destroy(lc);
}

/* ── light params are packed bit-exact (texel-by-texel) ──────────────────── */

static void test_pack_light_params_exact(void)
{
    JceLightCluster *lc = make_cluster(8, 8, 16, 16);

    /* Two lights: a point and a spot, both in front of the camera. */
    JceLightProxy lights[2];
    lights[0] = (JceLightProxy){ { 1.0f, 2.0f, 10.0f }, 3.0f, 0u };
    lights[1] = (JceLightProxy){ { -1.0f, 0.0f, 20.0f }, 5.0f, 1u };

    JceForwardPlusLightParam params[2];
    memset(params, 0, sizeof(params));
    params[0].color          = jce_v3(0.25f, 0.5f, 0.75f);
    params[0].intensity      = 2.0f;
    params[0].type           = 0.0f;             /* point */
    params[0].spot_dir       = jce_v3(0, 0, 0);
    params[0].inner_cone_cos = 1.0f;
    params[0].outer_cone_cos = -1.0f;
    params[1].color          = jce_v3(1.0f, 0.1f, 0.2f);
    params[1].intensity      = 4.0f;
    params[1].type           = 1.0f;             /* spot */
    params[1].spot_dir       = jce_v3(0, 0, 1);
    params[1].inner_cone_cos = 0.9f;
    params[1].outer_cone_cos = 0.7f;

    TEST_ASSERT_TRUE(jce_light_cluster_build(lc, lights, 2));
    JceLightClusterResult r = jce_light_cluster_get_result(lc);

    JceForwardPlusPacked p;
    TEST_ASSERT_TRUE(jce_forwardplus_packed_alloc(&p, 8, 8, 16, 16, 2));
    TEST_ASSERT_TRUE(jce_forwardplus_pack(&p, &r, lights, params, 2,
                                          1.0f, 100.0f));

    /* Light 0 — 4 texels @ base 0. */
    const float *t0 = p.params + 0 * JCE_FP_TEXELS_PER_LIGHT * 4;
    TEST_ASSERT_EQUAL_FLOAT(1.0f,  t0[0]);  /* pos.x  */
    TEST_ASSERT_EQUAL_FLOAT(2.0f,  t0[1]);  /* pos.y  */
    TEST_ASSERT_EQUAL_FLOAT(10.0f, t0[2]);  /* pos.z  */
    TEST_ASSERT_EQUAL_FLOAT(3.0f,  t0[3]);  /* range  */
    TEST_ASSERT_EQUAL_FLOAT(0.25f, t0[4]);  /* color.r*/
    TEST_ASSERT_EQUAL_FLOAT(0.5f,  t0[5]);
    TEST_ASSERT_EQUAL_FLOAT(0.75f, t0[6]);
    TEST_ASSERT_EQUAL_FLOAT(2.0f,  t0[7]);  /* intensity */
    TEST_ASSERT_EQUAL_FLOAT(0.0f,  t0[8]);  /* type = point */
    TEST_ASSERT_EQUAL_FLOAT(0.0f,  t0[9]);  /* spotDir = 0 */
    TEST_ASSERT_EQUAL_FLOAT(0.0f,  t0[10]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f,  t0[11]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f,  t0[12]); /* innerCos */
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, t0[13]); /* outerCos */

    /* Light 1 — 4 texels @ base 4. */
    const float *t1 = p.params + 1 * JCE_FP_TEXELS_PER_LIGHT * 4;
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, t1[0]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f,  t1[1]);
    TEST_ASSERT_EQUAL_FLOAT(20.0f, t1[2]);
    TEST_ASSERT_EQUAL_FLOAT(5.0f,  t1[3]);  /* range */
    TEST_ASSERT_EQUAL_FLOAT(1.0f,  t1[4]);
    TEST_ASSERT_EQUAL_FLOAT(0.1f,  t1[5]);
    TEST_ASSERT_EQUAL_FLOAT(0.2f,  t1[6]);
    TEST_ASSERT_EQUAL_FLOAT(4.0f,  t1[7]);  /* intensity */
    TEST_ASSERT_EQUAL_FLOAT(1.0f,  t1[8]);  /* type = spot */
    TEST_ASSERT_EQUAL_FLOAT(0.0f,  t1[9]);  /* spotDir.x */
    TEST_ASSERT_EQUAL_FLOAT(0.0f,  t1[10]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f,  t1[11]); /* spotDir.z */
    TEST_ASSERT_EQUAL_FLOAT(0.9f,  t1[12]);
    TEST_ASSERT_EQUAL_FLOAT(0.7f,  t1[13]);

    jce_forwardplus_packed_free(&p);
    jce_light_cluster_destroy(lc);
}

/* ── grid offset/count + index list mirror the cluster result exactly ────── */

static void test_pack_grid_and_index_match_result(void)
{
    JceLightCluster *lc = make_cluster(8, 8, 16, 16);
    JceLightProxy L = { { 0.0f, 0.0f, 10.0f }, 2.0f, 0u };
    TEST_ASSERT_TRUE(jce_light_cluster_build(lc, &L, 1));
    JceLightClusterResult r = jce_light_cluster_get_result(lc);
    TEST_ASSERT_TRUE(r.total_assignments > 0u);

    JceForwardPlusPacked p;
    TEST_ASSERT_TRUE(jce_forwardplus_packed_alloc(&p, 8, 8, 16, 16, 1));
    TEST_ASSERT_TRUE(jce_forwardplus_pack(&p, &r, &L, /*params*/
        (JceForwardPlusLightParam[1]){ { jce_v3(1,1,1), 1.0f, 0.0f,
                                          jce_v3(0,0,0), 1.0f, -1.0f } },
        1, 1.0f, 100.0f));

    uint32_t ncell = r.cells_x * r.cells_y * r.slices_z;
    uint32_t assign_seen = 0;
    for (uint32_t c = 0; c < ncell; ++c) {
        float offset = p.grid[c * 4 + 0];
        float count  = p.grid[c * 4 + 1];
        /* Grid count must equal the cluster count for this froxel. */
        TEST_ASSERT_EQUAL_UINT32(r.cell_counts[c], (uint32_t)count);
        /* Offset is the contiguous run base = c * max_per_cell. */
        TEST_ASSERT_EQUAL_UINT32(c * r.max_per_cell, (uint32_t)offset);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, p.grid[c * 4 + 2]);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, p.grid[c * 4 + 3]);

        /* Each index in [offset, offset+count) equals the cluster's index. */
        for (uint32_t k = 0; k < r.cell_counts[c]; ++k) {
            uint32_t e = (uint32_t)offset + k;
            uint32_t li_pack = (uint32_t)p.index[e * 4 + 0];
            uint32_t li_clus = r.cell_indices[c * r.max_per_cell + k];
            TEST_ASSERT_EQUAL_UINT32(li_clus, li_pack);
            /* This is our only light, so the index must be 0. */
            TEST_ASSERT_EQUAL_UINT32(0u, li_pack);
            assign_seen++;
        }
    }
    /* index_used reported == total assignments == sum of grid counts. */
    TEST_ASSERT_EQUAL_UINT32(r.total_assignments, p.index_used);
    TEST_ASSERT_EQUAL_UINT32(r.total_assignments, assign_seen);

    jce_forwardplus_packed_free(&p);
    jce_light_cluster_destroy(lc);
}

/* ── light in front lands in a froxel; light behind is excluded ──────────── */

static void test_pack_front_in_behind_out(void)
{
    /* Front light at +Z (in view). */
    {
        JceLightCluster *lc = make_cluster(8, 8, 16, 16);
        JceLightProxy L = { { 0.0f, 0.0f, 10.0f }, 2.0f, 0u };
        TEST_ASSERT_TRUE(jce_light_cluster_build(lc, &L, 1));
        JceLightClusterResult r = jce_light_cluster_get_result(lc);

        JceForwardPlusPacked p;
        TEST_ASSERT_TRUE(jce_forwardplus_packed_alloc(&p, 8, 8, 16, 16, 1));
        TEST_ASSERT_TRUE(jce_forwardplus_pack(&p, &r, &L,
            (JceForwardPlusLightParam[1]){ { jce_v3(1,1,1), 1.0f, 0.0f,
                                              jce_v3(0,0,0), 1.0f, -1.0f } },
            1, 1.0f, 100.0f));

        /* The flat index list must reference light 0 at least once, and the
         * grid must show a non-zero count somewhere. */
        TEST_ASSERT_TRUE(p.index_used > 0u);
        uint32_t ncell = p.cells_x * p.cells_y * p.slices_z;
        uint32_t total_count = 0;
        for (uint32_t c = 0; c < ncell; ++c)
            total_count += (uint32_t)p.grid[c * 4 + 1];
        TEST_ASSERT_EQUAL_UINT32(p.index_used, total_count);

        jce_forwardplus_packed_free(&p);
        jce_light_cluster_destroy(lc);
    }

    /* Behind light at -Z (behind near plane) — no assignments at all, so the
     * packed grid is entirely zero counts and the index list is unused. */
    {
        JceLightCluster *lc = make_cluster(8, 8, 16, 16);
        JceLightProxy L = { { 0.0f, 0.0f, -50.0f }, 2.0f, 0u };
        TEST_ASSERT_TRUE(jce_light_cluster_build(lc, &L, 1));
        JceLightClusterResult r = jce_light_cluster_get_result(lc);
        TEST_ASSERT_EQUAL_UINT32(0u, r.total_assignments);

        JceForwardPlusPacked p;
        TEST_ASSERT_TRUE(jce_forwardplus_packed_alloc(&p, 8, 8, 16, 16, 1));
        TEST_ASSERT_TRUE(jce_forwardplus_pack(&p, &r, &L,
            (JceForwardPlusLightParam[1]){ { jce_v3(1,1,1), 1.0f, 0.0f,
                                              jce_v3(0,0,0), 1.0f, -1.0f } },
            1, 1.0f, 100.0f));

        TEST_ASSERT_EQUAL_UINT32(0u, p.index_used);
        uint32_t ncell = p.cells_x * p.cells_y * p.slices_z;
        for (uint32_t c = 0; c < ncell; ++c) {
            TEST_ASSERT_EQUAL_FLOAT(0.0f, p.grid[c * 4 + 1]); /* count == 0 */
        }
        /* The light's params are still packed (it exists, just unassigned). */
        TEST_ASSERT_EQUAL_FLOAT(-50.0f, p.params[2]);  /* pos.z */
        TEST_ASSERT_EQUAL_FLOAT(2.0f,   p.params[3]);  /* range */

        jce_forwardplus_packed_free(&p);
        jce_light_cluster_destroy(lc);
    }
}

/* ── zero lights packs a valid all-empty grid (no crash, no assignments) ─── */

static void test_pack_zero_lights(void)
{
    JceLightCluster *lc = make_cluster(4, 4, 8, 8);
    TEST_ASSERT_TRUE(jce_light_cluster_build(lc, NULL, 0));
    JceLightClusterResult r = jce_light_cluster_get_result(lc);

    JceForwardPlusPacked p;
    TEST_ASSERT_TRUE(jce_forwardplus_packed_alloc(&p, 4, 4, 8, 8, 0));
    TEST_ASSERT_TRUE(jce_forwardplus_pack(&p, &r, NULL, NULL, 0, 1.0f, 100.0f));

    TEST_ASSERT_EQUAL_UINT32(0u, p.index_used);
    TEST_ASSERT_EQUAL_UINT32(0u, p.light_count);
    uint32_t ncell = 4u * 4u * 8u;
    for (uint32_t c = 0; c < ncell; ++c) {
        TEST_ASSERT_EQUAL_FLOAT(0.0f, p.grid[c * 4 + 1]);          /* count */
        TEST_ASSERT_EQUAL_UINT32(c * 8u, (uint32_t)p.grid[c * 4]); /* offset */
    }
    TEST_ASSERT_EQUAL_FLOAT(1.0f,   p.z_near);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, p.z_far);

    jce_forwardplus_packed_free(&p);
    jce_light_cluster_destroy(lc);
}

/* ── combined SINGLE-texture layout: regions placed at computed base rows ──
 *
 * The Round-B shader samples ONE RGBA32F texture; the three logical regions
 * (grid / index / lights) are stacked by ROWS.  This asserts:
 *   - region base rows = computed prefix sums of region row counts,
 *   - total height == sum of the three region row counts,
 *   - reading region_base_row + local element offset returns the SAME bytes
 *     as the existing separate-array (grid/index/lights) asserts. */

static float comb_texel(const JceForwardPlusCombined *c,
                        uint32_t base_row, uint32_t e, uint32_t ch)
{
    uint32_t tx  = e % c->width;
    uint32_t row = base_row + e / c->width;     /* absolute texel row */
    uint32_t idx = (row * c->width + tx) * 4u + ch;
    return c->data[idx];
}

static void test_pack_combined_single_texture(void)
{
    JceLightCluster *lc = make_cluster(8, 8, 16, 16);

    /* Same two lights (point + spot) as test_pack_light_params_exact so we can
     * cross-check the 4 light-param texels read through the LIGHTS region. */
    JceLightProxy lights[2];
    lights[0] = (JceLightProxy){ { 1.0f, 2.0f, 10.0f }, 3.0f, 0u };
    lights[1] = (JceLightProxy){ { -1.0f, 0.0f, 20.0f }, 5.0f, 1u };

    JceForwardPlusLightParam params[2];
    memset(params, 0, sizeof(params));
    params[0].color          = jce_v3(0.25f, 0.5f, 0.75f);
    params[0].intensity      = 2.0f;
    params[0].type           = 0.0f;             /* point */
    params[0].spot_dir       = jce_v3(0, 0, 0);
    params[0].inner_cone_cos = 1.0f;
    params[0].outer_cone_cos = -1.0f;
    params[1].color          = jce_v3(1.0f, 0.1f, 0.2f);
    params[1].intensity      = 4.0f;
    params[1].type           = 1.0f;             /* spot */
    params[1].spot_dir       = jce_v3(0, 0, 1);
    params[1].inner_cone_cos = 0.9f;
    params[1].outer_cone_cos = 0.7f;

    TEST_ASSERT_TRUE(jce_light_cluster_build(lc, lights, 2));
    JceLightClusterResult r = jce_light_cluster_get_result(lc);

    JceForwardPlusPacked p;
    TEST_ASSERT_TRUE(jce_forwardplus_packed_alloc(&p, 8, 8, 16, 16, 2));
    TEST_ASSERT_TRUE(jce_forwardplus_pack(&p, &r, lights, params, 2,
                                          1.0f, 100.0f));

    JceForwardPlusCombined c;
    TEST_ASSERT_TRUE(jce_forwardplus_pack_combined(&c, &p));

    /* Width is the fixed data-texture width. */
    TEST_ASSERT_EQUAL_UINT32(JCE_FP_TEX_WIDTH, c.width);

    /* Region row counts = ceil(texels / W); lights region uses >=1 texel. */
    uint32_t W           = JCE_FP_TEX_WIDTH;
    uint32_t grid_rows   = (p.grid_texels  + W - 1u) / W;
    uint32_t index_rows  = (p.index_texels + W - 1u) / W;
    uint32_t pt          = p.param_texels ? p.param_texels : 1u;
    uint32_t lights_rows = (pt + W - 1u) / W;
    if (grid_rows   == 0) grid_rows   = 1;
    if (index_rows  == 0) index_rows  = 1;
    if (lights_rows == 0) lights_rows = 1;

    /* Base rows are the prefix sums; total == their sum. */
    TEST_ASSERT_EQUAL_UINT32(0u,                      c.grid_base_row);
    TEST_ASSERT_EQUAL_UINT32(grid_rows,               c.index_base_row);
    TEST_ASSERT_EQUAL_UINT32(grid_rows + index_rows,  c.lights_base_row);
    TEST_ASSERT_EQUAL_UINT32(grid_rows,               c.grid_rows);
    TEST_ASSERT_EQUAL_UINT32(index_rows,              c.index_rows);
    TEST_ASSERT_EQUAL_UINT32(lights_rows,             c.lights_rows);
    TEST_ASSERT_EQUAL_UINT32(grid_rows + index_rows + lights_rows,
                             c.total_rows);

    /* ── LIGHTS region: region base + local element offset == separate array.
     * For light li the base element is li * JCE_FP_TEXELS_PER_LIGHT; each of
     * the 4 texels carries 4 channels. */
    for (uint32_t li = 0; li < 2; ++li) {
        const float *t = p.params + (size_t)li * JCE_FP_TEXELS_PER_LIGHT * 4u;
        for (uint32_t tx = 0; tx < JCE_FP_TEXELS_PER_LIGHT; ++tx) {
            uint32_t e = li * JCE_FP_TEXELS_PER_LIGHT + tx;  /* in-region elem */
            for (uint32_t ch = 0; ch < 4u; ++ch) {
                TEST_ASSERT_EQUAL_FLOAT(
                    t[tx * 4u + ch],
                    comb_texel(&c, c.lights_base_row, e, ch));
            }
        }
    }
    /* Spot-check a couple of well-known light values through the region. */
    TEST_ASSERT_EQUAL_FLOAT(10.0f, comb_texel(&c, c.lights_base_row, 0, 2)); /* L0 pos.z */
    TEST_ASSERT_EQUAL_FLOAT(3.0f,  comb_texel(&c, c.lights_base_row, 0, 3)); /* L0 range */
    TEST_ASSERT_EQUAL_FLOAT(1.0f,  comb_texel(&c, c.lights_base_row,
                                              1 * JCE_FP_TEXELS_PER_LIGHT + 2, 0)); /* L1 type=spot */

    /* ── GRID region: every froxel's (offset,count) read through the grid base
     * row equals the separate grid array (and the cluster result). */
    uint32_t ncell = r.cells_x * r.cells_y * r.slices_z;
    for (uint32_t cc = 0; cc < ncell; ++cc) {
        TEST_ASSERT_EQUAL_FLOAT(p.grid[cc * 4u + 0u],
                                comb_texel(&c, c.grid_base_row, cc, 0)); /* offset */
        TEST_ASSERT_EQUAL_FLOAT(p.grid[cc * 4u + 1u],
                                comb_texel(&c, c.grid_base_row, cc, 1)); /* count  */
        /* count matches cluster result. */
        TEST_ASSERT_EQUAL_UINT32(r.cell_counts[cc],
                                 (uint32_t)comb_texel(&c, c.grid_base_row, cc, 1));

        /* ── INDEX region: the light indices for this froxel read through the
         * index base row equal the separate index array. */
        uint32_t offset = (uint32_t)p.grid[cc * 4u + 0u];
        for (uint32_t k = 0; k < r.cell_counts[cc]; ++k) {
            uint32_t e = offset + k;
            TEST_ASSERT_EQUAL_FLOAT(p.index[e * 4u + 0u],
                                    comb_texel(&c, c.index_base_row, e, 0));
            TEST_ASSERT_EQUAL_UINT32(r.cell_indices[cc * r.max_per_cell + k],
                                     (uint32_t)comb_texel(&c, c.index_base_row, e, 0));
        }
    }

    jce_forwardplus_combined_free(&c);
    TEST_ASSERT_NULL(c.data);
    TEST_ASSERT_EQUAL_UINT32(0u, c.total_rows);

    jce_forwardplus_packed_free(&p);
    jce_light_cluster_destroy(lc);
}

/* ── combined zero-light frame: lights region still gets >=1 valid row ───── */

static void test_pack_combined_zero_lights(void)
{
    JceLightCluster *lc = make_cluster(4, 4, 8, 8);
    TEST_ASSERT_TRUE(jce_light_cluster_build(lc, NULL, 0));
    JceLightClusterResult r = jce_light_cluster_get_result(lc);

    JceForwardPlusPacked p;
    TEST_ASSERT_TRUE(jce_forwardplus_packed_alloc(&p, 4, 4, 8, 8, 0));
    TEST_ASSERT_TRUE(jce_forwardplus_pack(&p, &r, NULL, NULL, 0, 1.0f, 100.0f));

    JceForwardPlusCombined c;
    TEST_ASSERT_TRUE(jce_forwardplus_pack_combined(&c, &p));

    uint32_t W          = JCE_FP_TEX_WIDTH;
    uint32_t grid_rows  = (p.grid_texels  + W - 1u) / W;
    uint32_t index_rows = (p.index_texels + W - 1u) / W;
    /* light_count == 0 → param_texels == 0 → lights region still >= 1 row. */
    TEST_ASSERT_EQUAL_UINT32(0u, p.param_texels);
    TEST_ASSERT_EQUAL_UINT32(1u, c.lights_rows);
    TEST_ASSERT_EQUAL_UINT32(grid_rows + index_rows + 1u, c.total_rows);
    TEST_ASSERT_EQUAL_UINT32(grid_rows + index_rows, c.lights_base_row);

    /* All grid counts read through the combined buffer are zero. */
    uint32_t ncell = 4u * 4u * 8u;
    for (uint32_t cc = 0; cc < ncell; ++cc)
        TEST_ASSERT_EQUAL_FLOAT(0.0f, comb_texel(&c, c.grid_base_row, cc, 1));

    jce_forwardplus_combined_free(&c);
    jce_forwardplus_packed_free(&p);
    jce_light_cluster_destroy(lc);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_packed_alloc_shape);
    RUN_TEST(test_pack_shape_mismatch_rejected);
    RUN_TEST(test_pack_light_params_exact);
    RUN_TEST(test_pack_grid_and_index_match_result);
    RUN_TEST(test_pack_front_in_behind_out);
    RUN_TEST(test_pack_zero_lights);
    RUN_TEST(test_pack_combined_single_texture);
    RUN_TEST(test_pack_combined_zero_lights);
    return UNITY_END();
}
