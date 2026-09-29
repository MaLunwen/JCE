/*
 * test_jce_terrain_sky_occlusion.c
 *
 * Sky ambient applied unoccluded lights the floor of a canyon exactly like open
 * ground.  jce_terrain_bake_sky_occlusion answers "how much sky can this point
 * actually see", and the answers only mean something if they are ORDERED
 * correctly: a ravine floor must come out darker than the ridge beside it.
 *
 * The unit conversion is where a mistake would hide.  Heights are stored
 * NORMALIZED 0..1 and the horizon sweep needs world Y, so forgetting the
 * max_height factor flattens every elevation angle toward zero and the whole
 * field reads "open sky" -- a plausible-looking result that is entirely wrong.
 * These tests are built to fail loudly in exactly that case.
 */

#include <jce/middleware/scene/jce_terrain.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define TW    33
#define TH    33
#define WORLD 32.0f     /* cell size 1.0 */
#define MAXH  40.0f     /* tall enough that walls genuinely occlude */

static JceTerrain *g_t;
static float      *g_vis;
static float      *g_bent;

/* A deep, straight canyon running along Z, centred on x = 16. */
static void build_canyon(void)
{
    g_t = jce_terrain_create(TW, TH, WORLD, WORLD, MAXH, 8);
    TEST_ASSERT_NOT_NULL(g_t);
    float *h = (float *)jce_terrain_heights(g_t);
    TEST_ASSERT_NOT_NULL(h);

    for (int z = 0; z < TH; z++)
        for (int x = 0; x < TW; x++)
            /* floor at 0 inside |x-16| <= 2, plateau at 1.0 (= MAXH) outside */
            h[z * TW + x] = (abs(x - 16) <= 2) ? 0.0f : 1.0f;

    g_vis  = (float *)malloc(sizeof(float) * TW * TH);
    g_bent = (float *)malloc(sizeof(float) * TW * TH * 3);
    TEST_ASSERT_NOT_NULL(g_vis);
    TEST_ASSERT_NOT_NULL(g_bent);
}

static void teardown_canyon(void)
{
    free(g_vis);  g_vis  = NULL;
    free(g_bent); g_bent = NULL;
    jce_terrain_free(g_t); g_t = NULL;
}

static float vis_at(int x, int z) { return g_vis[z * TW + x]; }

/* ── 1. The canyon floor sees less sky than the plateau ────────────────
 *
 * The ordering IS the feature.  An absolute value would be a golden number
 * nobody could justify; the ordering is what makes the lighting correct. */

static void test_canyon_floor_is_occluded(void)
{
    build_canyon();
    TEST_ASSERT_TRUE(jce_terrain_bake_sky_occlusion(g_t, 32, g_vis, g_bent));

    const float floor_v  = vis_at(16, 16);   /* canyon centre */
    const float plateau  = vis_at(2,  16);   /* open ground far from the rim */

    TEST_ASSERT_TRUE(floor_v >= 0.0f && floor_v <= 1.0f);
    TEST_ASSERT_TRUE(plateau >= 0.0f && plateau <= 1.0f);

    /* Open ground is nearly unoccluded... */
    TEST_ASSERT_TRUE(plateau > 0.85f);
    /* ...and a 40 m wall two metres away is not a subtle effect. */
    TEST_ASSERT_TRUE(floor_v < 0.5f);
    TEST_ASSERT_TRUE(plateau - floor_v > 0.35f);

    teardown_canyon();
}

/* ── 2. Visibility increases monotonically out of the canyon ───────────  */

static void test_visibility_rises_toward_the_rim(void)
{
    build_canyon();
    TEST_ASSERT_TRUE(jce_terrain_bake_sky_occlusion(g_t, 32, g_vis, NULL));

    /* Walking from the canyon centre out onto the plateau must never get
     * DARKER -- a non-monotonic result means the sweep is leaking. */
    float prev = vis_at(16, 16);
    for (int x = 16; x >= 2; x--) {
        const float v = vis_at(x, 16);
        TEST_ASSERT_TRUE(v >= prev - 1e-4f);
        prev = v;
    }
    teardown_canyon();
}

/* ── 3. The bent normal leans OUT of the canyon, and is unit length ────
 *
 * The scalar only dims the light; the bent normal fixes where it comes from.
 * On the canyon floor the visible sky is the strip overhead, so the bent normal
 * must stay near vertical -- and on a slope it must lean away from the wall. */

static void test_bent_normals_are_unit_and_lean_away_from_walls(void)
{
    build_canyon();
    TEST_ASSERT_TRUE(jce_terrain_bake_sky_occlusion(g_t, 32, NULL, g_bent));

    for (int z = 0; z < TH; z++) {
        for (int x = 0; x < TW; x++) {
            const float *n = &g_bent[(z * TW + x) * 3];
            const float len = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
            TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, len);
            /* Always in the upper hemisphere, as the module promises. */
            TEST_ASSERT_TRUE(n[1] > 0.0f);
        }
    }

    /* Just inside the WEST wall (x=14, wall at x<=13): the open sky lies to the
     * east, so the bent normal must tip toward +X. */
    const float *west_side = &g_bent[(16 * TW + 14) * 3];
    TEST_ASSERT_TRUE(west_side[0] > 0.01f);

    /* Mirror: just inside the east wall it must tip toward -X. */
    const float *east_side = &g_bent[(16 * TW + 18) * 3];
    TEST_ASSERT_TRUE(east_side[0] < -0.01f);

    teardown_canyon();
}

/* ── 4. Flat terrain is unoccluded everywhere ──────────────────────────
 *
 * The control that catches the unit-conversion bug from the other side: if
 * heights were fed in normalized, a canyon would ALSO read as flat. */

static void test_flat_terrain_sees_full_sky(void)
{
    JceTerrain *t = jce_terrain_create(TW, TH, WORLD, WORLD, MAXH, 8);
    float *v = (float *)malloc(sizeof(float) * TW * TH);
    TEST_ASSERT_TRUE(jce_terrain_bake_sky_occlusion(t, 16, v, NULL));
    for (int i = 0; i < TW * TH; i++)
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, v[i]);
    free(v);
    jce_terrain_free(t);
}

/* ── 5. The sampler agrees with the baked grid at sample points ────────  */

static void test_sampler_matches_the_baked_field(void)
{
    build_canyon();
    TEST_ASSERT_TRUE(jce_terrain_bake_sky_occlusion(g_t, 32, g_vis, NULL));

    /* At exact sample positions bilinear interpolation must reproduce the
     * stored value; if it does not, the sampler and the bake disagree about
     * which cell a world point is in. */
    for (int z = 1; z < TH - 1; z += 4) {
        for (int x = 1; x < TW - 1; x += 4) {
            const float got = jce_terrain_sample_sky_visibility(
                g_t, g_vis, (float)x, (float)z);
            TEST_ASSERT_FLOAT_WITHIN(1e-3f, vis_at(x, z), got);
        }
    }

    /* Outside the terrain the honest answer is open sky -- it can only fail to
     * darken, never invent shadow. */
    TEST_ASSERT_EQUAL_FLOAT(1.0f,
        jce_terrain_sample_sky_visibility(g_t, g_vis, -50.0f, 0.0f));
    TEST_ASSERT_EQUAL_FLOAT(1.0f,
        jce_terrain_sample_sky_visibility(g_t, NULL, 4.0f, 4.0f));

    teardown_canyon();
}

/* ── 6. Refuses rather than inventing ──────────────────────────────────  */

static void test_refuses_degenerate_input(void)
{
    float v[4];
    TEST_ASSERT_FALSE(jce_terrain_bake_sky_occlusion(NULL, 16, v, NULL));

    JceTerrain *t = jce_terrain_create(TW, TH, WORLD, WORLD, MAXH, 8);
    TEST_ASSERT_FALSE(jce_terrain_bake_sky_occlusion(t, 16, NULL, NULL));

    /* Procedural terrain has no resident grid; refusing beats sweeping zeros
     * and reporting a uniformly open sky for a mountain range. */
    JceTerrain *proc = jce_terrain_create_procedural(
        TW, TH, WORLD, WORLD, MAXH, 8, 4, 1234u, 0.05f);
    if (proc) {
        float *pv = (float *)malloc(sizeof(float) * TW * TH);
        if (!jce_terrain_heights(proc))
            TEST_ASSERT_FALSE(jce_terrain_bake_sky_occlusion(proc, 16, pv, NULL));
        free(pv);
        jce_terrain_free(proc);
    }
    jce_terrain_free(t);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_canyon_floor_is_occluded);
    RUN_TEST(test_visibility_rises_toward_the_rim);
    RUN_TEST(test_bent_normals_are_unit_and_lean_away_from_walls);
    RUN_TEST(test_flat_terrain_sees_full_sky);
    RUN_TEST(test_sampler_matches_the_baked_field);
    RUN_TEST(test_refuses_degenerate_input);
    return UNITY_END();
}
