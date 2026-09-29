/*
 * test_jce_horizon.c -- unit tests for jce_horizon.h (L3 scene/terrain).
 *
 * Assertions are on properties of the bake -- coverage of the sweep, ordering
 * between shadowed and open cells, unit length, range, monotonicity under
 * added occluders -- so re-tuning the weighting survives while a broken sweep
 * (missed cells, wrong hull, wrong cone centre) fails.
 */

#include "unity.h"

#include "jce_horizon.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define TDIM   65
#define TCELLS (TDIM * TDIM)

static float g_h[TCELLS];
static float g_h2[TCELLS];
static float g_vis[TCELLS];
static float g_vis2[TCELLS];
static float g_bent[TCELLS * 3];
static float g_bent2[TCELLS * 3];

/* Integer hash: reproducible terrain without touching rand() or float trig. */
static uint32_t wang_hash(uint32_t x)
{
    x = (x ^ 61u) ^ (x >> 16);
    x *= 9u;
    x = x ^ (x >> 4);
    x *= 0x27d4eb2du;
    x = x ^ (x >> 15);
    return x;
}

static void fill_const(float *dst, uint32_t n, float v)
{
    uint32_t i;
    for (i = 0; i < n; ++i) dst[i] = v;
}

static void fill_hashed(float *dst, uint32_t n, uint32_t seed)
{
    uint32_t i;
    for (i = 0; i < n; ++i)
        dst[i] = (float)(wang_hash(i + seed) & 1023u) * (1.0f / 64.0f);
}

static JceHorizonDesc desc_for(const float *heights, uint32_t w, uint32_t h,
                               uint32_t dirs)
{
    JceHorizonDesc d;
    d.heights     = heights;
    d.w           = w;
    d.h           = h;
    d.cell_size_x = 1.0f;
    d.cell_size_z = 1.0f;
    d.directions  = dirs;
    return d;
}

static float bent_len(const float *bent, uint32_t cell)
{
    const float *n = &bent[cell * 3u];
    return sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
}

/* ------------------------------------------------------------------ */

/*
 * Flat field: nothing can occlude anything, so every cell must come out at
 * exactly 1 -- and that makes this the coverage gate for the sheared sweep
 * lines.  Each direction contributes 1/N to a cell it visits, so a cell the
 * line family skips lands at (N-1)/N and shows up here; there is no per-cell
 * visit counter that could paper over it (the module has no scratch to hold
 * one).  Swept over grid shapes, cell aspects and direction counts because
 * the line family is a partition only if the a-range covers every line, and
 * a too-narrow range drops cells on some shapes and not others.
 */
static void test_flat_field_sees_the_whole_sky(void)
{
    static const uint32_t dims[]    = { 1, 2, 3, 7, 16, 17, 31 };
    static const float    aspect[][2] = { {1.0f,1.0f}, {2.0f,0.5f}, {0.1f,10.0f} };
    static const uint32_t dirs[]    = { 4, 5, 8, 16 };
    uint32_t wi, hi, ai, di, i;
    float vmin = 2.0f, vmax = -1.0f, ymin = 2.0f, horiz_max = 0.0f, len_err = 0.0f;

    fill_const(g_h, TCELLS, 7.5f); /* non-zero: catches an absolute-height assumption */

    for (wi = 0; wi < sizeof dims / sizeof dims[0]; ++wi)
    for (hi = 0; hi < sizeof dims / sizeof dims[0]; ++hi)
    for (ai = 0; ai < sizeof aspect / sizeof aspect[0]; ++ai)
    for (di = 0; di < sizeof dirs / sizeof dirs[0]; ++di) {
        uint32_t n = dims[wi] * dims[hi];
        JceHorizonDesc d = desc_for(g_h, dims[wi], dims[hi], dirs[di]);
        d.cell_size_x = aspect[ai][0];
        d.cell_size_z = aspect[ai][1];
        TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis, g_bent));

        for (i = 0; i < n; ++i) {
            const float *nb = &g_bent[i * 3u];
            float hxz = sqrtf(nb[0] * nb[0] + nb[2] * nb[2]);
            if (g_vis[i] < vmin) vmin = g_vis[i];
            if (g_vis[i] > vmax) vmax = g_vis[i];
            if (nb[1] < ymin)    ymin = nb[1];
            if (hxz > horiz_max) horiz_max = hxz;
            if (fabsf(bent_len(g_bent, i) - 1.0f) > len_err)
                len_err = fabsf(bent_len(g_bent, i) - 1.0f);
        }
    }

    TEST_ASSERT_FLOAT_WITHIN(1.0e-4f, 1.0f, vmin);
    TEST_ASSERT_FLOAT_WITHIN(1.0e-4f, 1.0f, vmax);
    TEST_ASSERT_TRUE(ymin > 0.999f);        /* bent normal is straight up   */
    TEST_ASSERT_TRUE(horiz_max < 1.0e-3f);  /* azimuths cancel              */
    TEST_ASSERT_TRUE(len_err < 1.0e-4f);
}

/*
 * Trench along Z with walls 10 units high, floor three cells wide.  The floor
 * must be markedly darker than the plateau, which is unoccluded (the trench is
 * below it) and so must stay at ~1.
 */
static void test_trench_floor_is_darker_than_the_rim(void)
{
    const uint32_t w = 33, h = 33;
    JceHorizonDesc d;
    uint32_t x, z;
    float floor_v, rim_v, edge_v;

    for (z = 0; z < h; ++z)
        for (x = 0; x < w; ++x)
            g_h[z * w + x] = (x >= 15u && x <= 17u) ? 0.0f : 10.0f;

    d = desc_for(g_h, w, h, 16);
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis, g_bent));

    floor_v = g_vis[16u * w + 16u];  /* centre of the trench floor */
    rim_v   = g_vis[16u * w + 0u];   /* plateau, far side          */
    edge_v  = g_vis[16u * w + 18u];  /* plateau, hard against the trench */

    TEST_ASSERT_TRUE(floor_v < 0.5f);
    TEST_ASSERT_TRUE(rim_v   > 0.99f);
    TEST_ASSERT_TRUE(edge_v  > 0.99f);
    TEST_ASSERT_TRUE(rim_v - floor_v > 0.3f);

    /* Symmetric trench: the bent normal must stay vertical, not lean. */
    TEST_ASSERT_TRUE(fabsf(g_bent[(16u * w + 16u) * 3u + 0u]) < 1.0e-2f);
    TEST_ASSERT_TRUE(g_bent[(16u * w + 16u) * 3u + 1u] > 0.9f);
}

/*
 * A single tall cell darkens its neighbours and leaves distant cells alone --
 * i.e. occlusion is driven by the subtended angle, not by "there exists
 * something taller somewhere".  The spike's own cell is the summit test:
 * every other sample is below its horizontal, and terrain that falls away
 * takes nothing off the sky, so it must come out fully open.
 */
static void test_isolated_spike_is_a_local_effect(void)
{
    const uint32_t w = 41, h = 41, n = w * h;
    JceHorizonDesc d;
    float near_v, far_v, summit_v;

    fill_const(g_h, n, 0.0f);
    g_h[20u * w + 20u] = 5.0f;

    d = desc_for(g_h, w, h, 16);
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis, g_bent));

    near_v   = g_vis[20u * w + 21u]; /* touching the spike */
    far_v    = g_vis[2u  * w + 2u];  /* ~25 cells away     */
    summit_v = g_vis[20u * w + 20u];

    TEST_ASSERT_TRUE(near_v < 0.98f);
    TEST_ASSERT_TRUE(far_v  > 0.99f);
    TEST_ASSERT_TRUE(far_v - near_v > 0.02f);

    TEST_ASSERT_TRUE(summit_v > 0.999f);
    TEST_ASSERT_TRUE(g_bent[(20u * w + 20u) * 3u + 1u] > 0.999f);
}

/*
 * Occlusion is geometry in world units, not in cells.  Same grid, same
 * heights, only the Z spacing changes: at 1 unit the wall is 5 units away and
 * 10 tall and swallows much of the sky; at 16 units it is 80 away, subtends
 * atan(10/80) = 7 degrees, and must barely register.  Anything that measures
 * distance in cell counts passes every square-cell test and fails here.
 */
static void test_world_spacing_drives_the_geometry(void)
{
    const uint32_t w = 41, h = 41;
    JceHorizonDesc d;
    uint32_t x, z;
    float tight_v, wide_v;

    for (z = 0; z < h; ++z)
        for (x = 0; x < w; ++x)
            g_h[z * w + x] = (z >= 25u) ? 10.0f : 0.0f;

    d = desc_for(g_h, w, h, 16);
    d.cell_size_z = 1.0f;
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis, NULL));
    tight_v = g_vis[20u * w + 20u];

    d.cell_size_z = 16.0f;
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis, g_bent));
    wide_v = g_vis[20u * w + 20u];

    TEST_ASSERT_TRUE(tight_v < 0.9f);
    TEST_ASSERT_TRUE(wide_v  > 0.99f);
    TEST_ASSERT_TRUE(wide_v - tight_v > 0.1f);
    /* Bent normal tilts away from the near wall, barely from the far one. */
    TEST_ASSERT_TRUE(fabsf(g_bent[(20u * w + 20u) * 3u + 2u]) < 0.05f);
}

/*
 * The point of the bent normal: next to a cliff the sky arrives from the open
 * side, so the vector must lean away from the wall while staying in the upper
 * hemisphere.  A scalar visibility cannot express this.
 */
static void test_bent_normal_leans_away_from_a_cliff(void)
{
    const uint32_t w = 41, h = 41;
    JceHorizonDesc d;
    uint32_t x, z, cell = 20u * w + 10u; /* 15 cells in front of the wall */
    float low_x, low_y;

    for (z = 0; z < h; ++z)
        for (x = 0; x < w; ++x)
            g_h[z * w + x] = (x >= 25u) ? 8.0f : 0.0f;

    d = desc_for(g_h, w, h, 16);
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis, g_bent));
    low_x = g_bent[cell * 3u + 0u];
    low_y = g_bent[cell * 3u + 1u];

    for (z = 0; z < h; ++z)
        for (x = 0; x < w; ++x)
            g_h[z * w + x] = (x >= 25u) ? 50.0f : 0.0f;

    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis, g_bent));

    TEST_ASSERT_TRUE(g_bent[cell * 3u + 0u] < -0.1f);          /* away from +X */
    TEST_ASSERT_TRUE(g_bent[cell * 3u + 1u] >  0.5f);          /* still upward */
    TEST_ASSERT_TRUE(fabsf(g_bent[cell * 3u + 2u]) < 1.0e-2f); /* no Z bias    */
    TEST_ASSERT_FLOAT_WITHIN(1.0e-4f, 1.0f, bent_len(g_bent, cell));

    /* Taller wall, same distance: the open sky shifts further off-vertical, so
     * the lean has to grow with the occlusion rather than merely have a sign. */
    TEST_ASSERT_TRUE(g_bent[cell * 3u + 0u] < low_x);
    TEST_ASSERT_TRUE(g_bent[cell * 3u + 1u] < low_y);
    TEST_ASSERT_TRUE(low_x < -0.01f); /* the short wall already leans */
}

/* Range and unit-length contracts must hold on terrain with no structure. */
static void test_outputs_stay_in_contract_on_rough_terrain(void)
{
    const uint32_t w = TDIM, h = TDIM, n = w * h;
    JceHorizonDesc d;
    uint32_t i;
    float vmin = 2.0f, vmax = -1.0f, len_err = 0.0f, ymin = 2.0f;

    fill_hashed(g_h, n, 12345u);
    d = desc_for(g_h, w, h, 16);
    d.cell_size_x = 2.0f;   /* non-square cells: the major axis is chosen in */
    d.cell_size_z = 0.5f;   /* index space, not world space                  */
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis, g_bent));

    for (i = 0; i < n; ++i) {
        float len = bent_len(g_bent, i);
        if (g_vis[i] < vmin) vmin = g_vis[i];
        if (g_vis[i] > vmax) vmax = g_vis[i];
        if (g_bent[i * 3u + 1u] < ymin) ymin = g_bent[i * 3u + 1u];
        if (fabsf(len - 1.0f) > len_err) len_err = fabsf(len - 1.0f);
    }

    TEST_ASSERT_TRUE(vmin >= 0.0f);
    TEST_ASSERT_TRUE(vmax <= 1.0f);
    TEST_ASSERT_TRUE(vmin < vmax);      /* rough terrain must not be uniform */
    TEST_ASSERT_TRUE(len_err < 1.0e-3f);
    TEST_ASSERT_TRUE(ymin > 0.0f);      /* always in the upper hemisphere    */
}

/*
 * Adding height can only take sky away from cells that did not move: their own
 * sample stays put while a potential occluder rises, so the horizon can only
 * climb.  Holds exactly, not statistically.
 */
static void test_raising_terrain_never_brightens_untouched_cells(void)
{
    const uint32_t w = TDIM, h = TDIM, n = w * h;
    JceHorizonDesc d;
    uint32_t x, z, i;
    int      violations = 0;

    fill_hashed(g_h, n, 777u);
    memcpy(g_h2, g_h, n * sizeof(float));
    for (z = 20; z < 26; ++z)
        for (x = 20; x < 26; ++x)
            g_h2[z * w + x] += 30.0f;

    d = desc_for(g_h, w, h, 16);
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis, NULL));
    d = desc_for(g_h2, w, h, 16);
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis2, NULL));

    for (i = 0; i < n; ++i) {
        uint32_t cx = i % w, cz = i / w;
        if (cx >= 20u && cx < 26u && cz >= 20u && cz < 26u) continue;
        if (g_vis2[i] > g_vis[i] + 1.0e-5f) ++violations;
    }
    TEST_ASSERT_EQUAL_INT(0, violations);
    TEST_ASSERT_TRUE(g_vis2[22u * w + 19u] < g_vis[22u * w + 19u]); /* did darken */
}

/* Same input, same bits -- no accumulation order or state carried between runs. */
static void test_bake_is_deterministic(void)
{
    const uint32_t w = 49, h = 37, n = w * h;
    JceHorizonDesc d;

    fill_hashed(g_h, n, 2026u);
    d = desc_for(g_h, w, h, 23);

    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis,  g_bent));
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis2, g_bent2));

    TEST_ASSERT_EQUAL_MEMORY(g_vis,  g_vis2,  n * sizeof(float));
    TEST_ASSERT_EQUAL_MEMORY(g_bent, g_bent2, n * 3u * sizeof(float));
}

/* Either output may be omitted, and omitting one must not perturb the other. */
static void test_partial_outputs_match_the_full_bake(void)
{
    const uint32_t w = 33, h = 29, n = w * h;
    JceHorizonDesc d;

    fill_hashed(g_h, n, 99u);
    d = desc_for(g_h, w, h, 16);

    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis, g_bent));

    memset(g_vis2, 0x7f, sizeof(g_vis2));
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis2, NULL));
    TEST_ASSERT_EQUAL_MEMORY(g_vis, g_vis2, n * sizeof(float));

    memset(g_bent2, 0x7f, sizeof(g_bent2));
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, NULL, g_bent2));
    TEST_ASSERT_EQUAL_MEMORY(g_bent, g_bent2, n * 3u * sizeof(float));
}

/* Direction count is clamped, not rejected, and 0 means the documented default. */
static void test_direction_count_is_clamped(void)
{
    const uint32_t w = 25, h = 25, n = w * h;
    JceHorizonDesc d;

    fill_hashed(g_h, n, 5u);

    d = desc_for(g_h, w, h, 1);
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis, NULL));
    d = desc_for(g_h, w, h, JCE_HORIZON_MIN_DIRECTIONS);
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis2, NULL));
    TEST_ASSERT_EQUAL_MEMORY(g_vis, g_vis2, n * sizeof(float));

    d = desc_for(g_h, w, h, 100000u);
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis, NULL));
    d = desc_for(g_h, w, h, JCE_HORIZON_MAX_DIRECTIONS);
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis2, NULL));
    TEST_ASSERT_EQUAL_MEMORY(g_vis, g_vis2, n * sizeof(float));

    d = desc_for(g_h, w, h, 0);
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis, NULL));
    d = desc_for(g_h, w, h, JCE_HORIZON_DEFAULT_DIRECTIONS);
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis2, NULL));
    TEST_ASSERT_EQUAL_MEMORY(g_vis, g_vis2, n * sizeof(float));
}

/* Degenerate input is refused, and the smallest legal fields still bake. */
static void test_degenerate_input_is_safe(void)
{
    JceHorizonDesc d;
    uint32_t i;

    fill_const(g_h, TCELLS, 1.0f);

    TEST_ASSERT_FALSE(jce_horizon_bake(NULL, g_vis, g_bent));

    d = desc_for(NULL, 8, 8, 16);
    TEST_ASSERT_FALSE(jce_horizon_bake(&d, g_vis, g_bent));

    d = desc_for(g_h, 8, 8, 16);
    TEST_ASSERT_FALSE(jce_horizon_bake(&d, NULL, NULL));

    d = desc_for(g_h, 0, 8, 16);
    TEST_ASSERT_FALSE(jce_horizon_bake(&d, g_vis, g_bent));
    d = desc_for(g_h, 8, 0, 16);
    TEST_ASSERT_FALSE(jce_horizon_bake(&d, g_vis, g_bent));

    /* Rejected before any write: the buffers here are sized for 8x8. */
    d = desc_for(g_h, JCE_HORIZON_MAX_DIM + 1u, 8, 16);
    TEST_ASSERT_FALSE(jce_horizon_bake(&d, g_vis, g_bent));

    d = desc_for(g_h, 8, 8, 16); d.cell_size_x = 0.0f;
    TEST_ASSERT_FALSE(jce_horizon_bake(&d, g_vis, g_bent));
    d = desc_for(g_h, 8, 8, 16); d.cell_size_z = -1.0f;
    TEST_ASSERT_FALSE(jce_horizon_bake(&d, g_vis, g_bent));
    d = desc_for(g_h, 8, 8, 16); d.cell_size_x = (float)sqrt(-1.0);
    TEST_ASSERT_FALSE(jce_horizon_bake(&d, g_vis, g_bent));

    /* 1x1 and single-row fields exercise sweeps whose lines hold one sample. */
    d = desc_for(g_h, 1, 1, 16);
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis, g_bent));
    TEST_ASSERT_FLOAT_WITHIN(1.0e-4f, 1.0f, g_vis[0]);
    TEST_ASSERT_FLOAT_WITHIN(1.0e-4f, 1.0f, g_bent[1]);

    fill_hashed(g_h, 9, 3u);
    d = desc_for(g_h, 1, 9, 16);
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis, g_bent));
    for (i = 0; i < 9u; ++i) {
        TEST_ASSERT_TRUE(g_vis[i] >= 0.0f && g_vis[i] <= 1.0f);
        TEST_ASSERT_FLOAT_WITHIN(1.0e-3f, 1.0f, bent_len(g_bent, i));
    }
    d = desc_for(g_h, 9, 1, 16);
    TEST_ASSERT_TRUE(jce_horizon_bake(&d, g_vis, g_bent));
    for (i = 0; i < 9u; ++i) {
        TEST_ASSERT_TRUE(g_vis[i] >= 0.0f && g_vis[i] <= 1.0f);
        TEST_ASSERT_FLOAT_WITHIN(1.0e-3f, 1.0f, bent_len(g_bent, i));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_flat_field_sees_the_whole_sky);
    RUN_TEST(test_trench_floor_is_darker_than_the_rim);
    RUN_TEST(test_isolated_spike_is_a_local_effect);
    RUN_TEST(test_world_spacing_drives_the_geometry);
    RUN_TEST(test_bent_normal_leans_away_from_a_cliff);
    RUN_TEST(test_outputs_stay_in_contract_on_rough_terrain);
    RUN_TEST(test_raising_terrain_never_brightens_untouched_cells);
    RUN_TEST(test_bake_is_deterministic);
    RUN_TEST(test_partial_outputs_match_the_full_bake);
    RUN_TEST(test_direction_count_is_clamped);
    RUN_TEST(test_degenerate_input_is_safe);
    return UNITY_END();
}