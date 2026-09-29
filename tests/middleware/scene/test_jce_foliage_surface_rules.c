/*
 * test_jce_foliage_surface_rules.c
 *
 * Altitude bands and ridge/crease preference for foliage scatter.
 *
 * Slope alone cannot say where a plant grows.  These two rules add the axes an
 * artist actually reaches for -- how high it is, and whether it sits in a
 * gully or on an exposed spine -- and between them they replace most
 * hand-painted density masks with rules that survive the terrain being
 * regenerated underneath them.
 *
 * THE INVARIANT THAT MATTERS MOST is not that the rules reject the right
 * candidates; it is that they consume NO randomness.  A cooked bake and a
 * runtime scatter must agree instance for instance, so a ruled scatter has to
 * be a strict SUBSET of the unruled one at the same seed.  If a rule perturbed
 * the RNG stream, every downstream instance would move and the two would
 * silently disagree.
 */

#include <jce/middleware/scene/jce_foliage.h>
#include <jce/middleware/scene/jce_terrain.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define TDIM   65
#define TWORLD 64.0f
#define TMAXH  40.0f
#define CAP    4096

static JceTerrain *g_t;
static const jce_vec3 g_origin  = { 32.0f, 0.0f, 32.0f };
static const jce_vec3 g_origin0 = { 0.0f, 0.0f, 0.0f };

/* A ramp in X: world height runs 0 .. TMAXH across the terrain, so an altitude
 * band maps to a known slab of the area rect. */
static void make_ramp(void)
{
    g_t = jce_terrain_create(TDIM, TDIM, TWORLD, TWORLD, TMAXH, 64);
    TEST_ASSERT_NOT_NULL(g_t);
    float *h = (float *)jce_terrain_heights(g_t);
    for (int z = 0; z < TDIM; z++)
        for (int x = 0; x < TDIM; x++)
            h[z * TDIM + x] = (float)x / (float)(TDIM - 1);
}

static JceFoliageScatterParams base_params(void)
{
    JceFoliageScatterParams p;
    memset(&p, 0, sizeof p);
    p.seed          = 4242u;
    p.density       = 1.0f;
    p.area_x        = 40.0f;
    p.area_z        = 40.0f;
    p.max_slope_deg = 90.0f;     /* slope test off */
    p.scale_min     = 1.0f;
    p.scale_max     = 1.0f;
    return p;
}

/* ── 1. Rules do not consume randomness: ruled output is a SUBSET ──────  */

static void test_rules_do_not_perturb_the_rng(void)
{
    make_ramp();
    JceFoliageInstance all[CAP], ruled[CAP];
    JceFoliageScatterParams p = base_params();

    const int n_all = (int)jce_foliage_scatter(&p, g_t, &g_origin,
                                               all, CAP);
    TEST_ASSERT_TRUE(n_all > 50);

    p.height_min = 10.0f;
    p.height_max = 25.0f;
    const int n_ruled = (int)jce_foliage_scatter(&p, g_t,
                                                 &g_origin,
                                                 ruled, CAP);
    TEST_ASSERT_TRUE(n_ruled > 0);
    TEST_ASSERT_TRUE(n_ruled < n_all);

    /* Every kept instance must appear, EXACTLY, in the unruled run.  A rule
     * that drew from the RNG would shift every later candidate and this would
     * fail almost immediately. */
    for (int i = 0; i < n_ruled; i++) {
        int found = 0;
        for (int j = 0; j < n_all; j++) {
            if (all[j].pos[0] == ruled[i].pos[0] &&
                all[j].pos[2] == ruled[i].pos[2] &&
                all[j].rot_y  == ruled[i].rot_y  &&
                all[j].scale  == ruled[i].scale) { found = 1; break; }
        }
        TEST_ASSERT_TRUE(found);
    }

    /* The RIDGE rule must satisfy the same invariant, and it needs its own
     * check: an earlier version of this test only exercised the height band,
     * and a mutation that made the ridge rule draw from the RNG sailed through
     * it.  Each rule has to be proven separately -- one passing says nothing
     * about the other. */
    JceFoliageScatterParams rp = base_params();
    static float ridge[64 * 64];
    for (int i = 0; i < 64 * 64; i++) ridge[i] = ((i % 64) < 32) ? -1.0f : 1.0f;
    rp.ridge_field = ridge;
    rp.ridge_dim   = 64;
    rp.ridge_min   = -1.5f;
    rp.ridge_max   = -0.5f;

    JceFoliageInstance rout[CAP];
    const int n_r = (int)jce_foliage_scatter(&rp, g_t, &g_origin, rout, CAP);
    TEST_ASSERT_TRUE(n_r > 0);
    TEST_ASSERT_TRUE(n_r < n_all);
    for (int i = 0; i < n_r; i++) {
        int found = 0;
        for (int j = 0; j < n_all; j++) {
            if (all[j].pos[0] == rout[i].pos[0] &&
                all[j].pos[2] == rout[i].pos[2] &&
                all[j].rot_y  == rout[i].rot_y  &&
                all[j].scale  == rout[i].scale) { found = 1; break; }
        }
        TEST_ASSERT_TRUE(found);
    }
    jce_terrain_free(g_t);
}

/* ── 2. The altitude band is respected ─────────────────────────────────  */

static void test_height_band_is_enforced(void)
{
    make_ramp();
    JceFoliageInstance out[CAP];
    JceFoliageScatterParams p = base_params();
    p.height_min = 12.0f;
    p.height_max = 22.0f;

    const int n = (int)jce_foliage_scatter(&p, g_t, &g_origin,
                                           out, CAP);
    TEST_ASSERT_TRUE(n > 0);
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(out[i].pos[1] >= 12.0f - 1e-3f);
        TEST_ASSERT_TRUE(out[i].pos[1] <= 22.0f + 1e-3f);
    }
    jce_terrain_free(g_t);
}

/* ── 3. min >= max disables the band rather than rejecting everything ──
 *
 * A zeroed params struct must keep the historical behaviour.  If an unset band
 * meant [0,0] every scatter in the engine would silently return nothing. */

static void test_zeroed_band_disables(void)
{
    make_ramp();
    JceFoliageInstance a[CAP], b[CAP];
    JceFoliageScatterParams p = base_params();
    const int n_a = (int)jce_foliage_scatter(&p, g_t, &g_origin,
                                             a, CAP);

    p.height_min = 5.0f;      /* min > max: still disabled */
    p.height_max = 5.0f;
    const int n_b = (int)jce_foliage_scatter(&p, g_t, &g_origin,
                                             b, CAP);
    TEST_ASSERT_EQUAL_INT(n_a, n_b);
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(JceFoliageInstance) * (size_t)n_a);
    jce_terrain_free(g_t);
}

/* ── 4. Ridge preference selects creases vs ridges ─────────────────────  */

#define RDIM 32

static void test_ridge_band_selects_creases(void)
{
    make_ramp();
    /* A ridge field that is -1 on the left half and +1 on the right, in the
     * scatter rect's own UV space (mask_world_size == 0). */
    static float ridge[RDIM * RDIM];
    for (int z = 0; z < RDIM; z++)
        for (int x = 0; x < RDIM; x++)
            ridge[z * RDIM + x] = (x < RDIM / 2) ? -1.0f : 1.0f;

    JceFoliageInstance out[CAP];
    JceFoliageScatterParams p = base_params();
    p.ridge_field = ridge;
    p.ridge_dim   = RDIM;
    /* Creases only. */
    p.ridge_min   = -1.5f;
    p.ridge_max   = -0.5f;

    const int n = (int)jce_foliage_scatter(&p, g_t, &g_origin,
                                           out, CAP);
    TEST_ASSERT_TRUE(n > 0);
    /* Every survivor must be in the left half of the rect (centre 32, width 40
     * => left half is x < 32). */
    for (int i = 0; i < n; i++)
        TEST_ASSERT_TRUE(out[i].pos[0] < 32.0f + 1e-3f);

    /* And asking for ridges instead must select the complementary half. */
    p.ridge_min = 0.5f;
    p.ridge_max = 1.5f;
    const int m = (int)jce_foliage_scatter(&p, g_t, &g_origin,
                                           out, CAP);
    TEST_ASSERT_TRUE(m > 0);
    for (int i = 0; i < m; i++)
        TEST_ASSERT_TRUE(out[i].pos[0] > 32.0f - 1e-3f);

    jce_terrain_free(g_t);
}

/* ── 5. A ridge rule needs no terrain at all ───────────────────────────
 *
 * The ridge field is independent data, so the rule must work for a scatter
 * with no terrain -- and must reject before the (absent) height fetch. */

static void test_ridge_band_without_terrain(void)
{
    static float ridge[RDIM * RDIM];
    for (int i = 0; i < RDIM * RDIM; i++) ridge[i] = 1.0f;   /* all ridge */

    JceFoliageInstance out[CAP];
    JceFoliageScatterParams p = base_params();
    p.ridge_field = ridge;
    p.ridge_dim   = RDIM;
    p.ridge_min   = -1.0f;
    p.ridge_max   = -0.5f;     /* creases only -- nothing qualifies */

    TEST_ASSERT_EQUAL_UINT32(0u,
        jce_foliage_scatter(&p, NULL, &g_origin0, out, CAP));

    p.ridge_min = 0.5f;
    p.ridge_max = 1.5f;        /* now everything qualifies */
    TEST_ASSERT_TRUE(jce_foliage_scatter(&p, NULL, &g_origin0,
                                         out, CAP) > 0u);
}

/* ── 6. Rules compose with the slope test ──────────────────────────────  */

static void test_rules_compose_with_slope(void)
{
    make_ramp();
    JceFoliageInstance out[CAP];
    JceFoliageScatterParams p = base_params();
    p.max_slope_deg = 40.0f;
    p.height_min    = 10.0f;
    p.height_max    = 30.0f;
    p.want_normals  = true;

    const int n = (int)jce_foliage_scatter(&p, g_t, &g_origin,
                                           out, CAP);
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(out[i].pos[1] >= 10.0f - 1e-3f);
        TEST_ASSERT_TRUE(out[i].pos[1] <= 30.0f + 1e-3f);
        /* Normals must still be unit length with the extra rejections in play. */
        const float len = sqrtf(out[i].normal[0] * out[i].normal[0] +
                                out[i].normal[1] * out[i].normal[1] +
                                out[i].normal[2] * out[i].normal[2]);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, len);
    }
    jce_terrain_free(g_t);
}

/* ── 7. Mask and ridge share ONE mapping, with a stated V convention ───
 *
 * These two fields used to map world space with two separate copies of the
 * arithmetic, and the copies had already drifted -- the mask flipped V and the
 * ridge did not, so the same world point addressed two Z-MIRRORED cells.  An
 * artist painting both over one square would have found them disagreeing with
 * nothing to explain it.
 *
 * They now share one function.  The flip REMAINS, because it is a genuine
 * difference of provenance: a painted mask arrives in image order, the ridge
 * channel is produced row-major in +Z.  This pins both halves -- the geometry
 * is shared, and the flip is the only difference.
 *
 * Everything is centred on the ORIGIN here.  A first version scattered a rect
 * at (32,32) over a square spanning [-32,32]; they barely overlapped, the
 * scatter returned nothing, and the test was measuring an empty set. */

static void test_mask_and_ridge_share_one_mapping(void)
{
    make_ramp();

    /* One narrow band of +1 near row 0, i.e. at LOW v. */
    static float band[RDIM * RDIM];
    for (int z = 0; z < RDIM; z++)
        for (int x = 0; x < RDIM; x++)
            band[z * RDIM + x] = (z < RDIM / 4) ? 1.0f : 0.0f;

    const jce_vec3 origin = { 0.0f, 0.0f, 0.0f };

    /* As a RIDGE field (no flip): low v is low world Z, so survivors sit at
     * NEGATIVE world Z. */
    JceFoliageScatterParams p = base_params();
    p.area_x = 60.0f; p.area_z = 60.0f;
    p.mask_world_size = 64.0f;
    p.ridge_field = band;
    p.ridge_dim   = RDIM;
    p.ridge_min   = 0.5f;
    p.ridge_max   = 1.5f;

    JceFoliageInstance ro[CAP];
    const int nr = (int)jce_foliage_scatter(&p, NULL, &origin, ro, CAP);
    TEST_ASSERT_TRUE(nr > 10);
    double sum_r = 0.0;
    for (int i = 0; i < nr; i++) sum_r += ro[i].pos[2];
    const double mean_r = sum_r / (double)nr;

    /* The SAME field as a density mask (values 1 / 0, so it keeps or rejects
     * outright).  The mask flips V, so low v is HIGH world Z and survivors
     * must sit at POSITIVE world Z. */
    JceFoliageScatterParams m = base_params();
    m.area_x = 60.0f; m.area_z = 60.0f;
    m.mask_world_size = 64.0f;
    m.density_mask = band;
    m.mask_dim     = RDIM;

    JceFoliageInstance mo[CAP];
    const int nm = (int)jce_foliage_scatter(&m, NULL, &origin, mo, CAP);
    TEST_ASSERT_TRUE(nm > 10);
    double sum_m = 0.0;
    for (int i = 0; i < nm; i++) sum_m += mo[i].pos[2];
    const double mean_m = sum_m / (double)nm;

    /* Opposite sides, by construction of the flip. */
    TEST_ASSERT_TRUE(mean_r < -5.0);
    TEST_ASSERT_TRUE(mean_m >  5.0);
    /* And mirror images of each other about the square's centre: same
     * magnitude, opposite sign.  That is what "one shared mapping plus one
     * flip" means, as opposed to two unrelated mappings. */
    TEST_ASSERT_TRUE(fabs(fabs(mean_r) - fabs(mean_m)) < 3.0);

    /* ── And the world square really is WORLD space ────────────────────
     *
     * mask_world_size exists so a field shared by several scatter rects stays
     * pinned to the same world texture.  Moving the rect must therefore move
     * which part of the field it sees.  With the rect centred on the square,
     * world-space and rect-local UV happen to coincide -- so a mutation that
     * ignored mask_world_size entirely survived until this offset probe
     * existed. */
    static float centre_band[RDIM * RDIM];
    for (int z = 0; z < RDIM; z++)
        for (int x = 0; x < RDIM; x++)
            centre_band[z * RDIM + x] =
                (z >= RDIM * 3 / 8 && z < RDIM * 5 / 8) ? 1.0f : 0.0f;

    JceFoliageScatterParams c = base_params();
    c.area_x = 60.0f; c.area_z = 60.0f;
    c.mask_world_size = 64.0f;
    c.ridge_field = centre_band;      /* world z in roughly [-8, +8] */
    c.ridge_dim   = RDIM;
    c.ridge_min   = 0.5f;
    c.ridge_max   = 1.5f;

    /* A CENTRAL band, so shifting the rect cannot simply clip it off the edge
     * -- an earlier attempt used the edge band and the shift moved the rect
     * clean off it, leaving nothing to compare. */
    JceFoliageInstance c0[CAP], c1[CAP];
    const jce_vec3 shifted = { 0.0f, 0.0f, 20.0f };
    const int n0 = (int)jce_foliage_scatter(&c, NULL, &origin,  c0, CAP);
    const int n1 = (int)jce_foliage_scatter(&c, NULL, &shifted, c1, CAP);
    TEST_ASSERT_TRUE(n0 > 10);
    TEST_ASSERT_TRUE(n1 > 10);

    double s0 = 0.0, s1 = 0.0;
    for (int i = 0; i < n0; i++) s0 += c0[i].pos[2];
    for (int i = 0; i < n1; i++) s1 += c1[i].pos[2];
    const double m0 = s0 / (double)n0, m1 = s1 / (double)n1;

    /* World-space UV pins the band to the WORLD, so moving the rect leaves it
     * where it was.  Rect-local UV would carry it along, putting m1 near
     * m0 + 20.  A mutation ignoring mask_world_size survived until this probe
     * existed, because with the rect centred on the square the two mappings
     * coincide exactly. */
    TEST_ASSERT_TRUE(fabs(m1 - m0) < 6.0);
    TEST_ASSERT_TRUE(fabs(m1 - (m0 + 20.0)) > 10.0);

    jce_terrain_free(g_t);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rules_do_not_perturb_the_rng);
    RUN_TEST(test_height_band_is_enforced);
    RUN_TEST(test_zeroed_band_disables);
    RUN_TEST(test_ridge_band_selects_creases);
    RUN_TEST(test_ridge_band_without_terrain);
    RUN_TEST(test_rules_compose_with_slope);
    RUN_TEST(test_mask_and_ridge_share_one_mapping);
    return UNITY_END();
}
