/*
 * test_jce_water_rings.c
 *
 * Concentric-ring ocean geometry.
 *
 * The point of rings is not "fewer vertices" -- it is putting the vertices
 * where the camera is.  So the assertions are about DISTRIBUTION, plus the two
 * ways a ring mesh goes wrong: a hole under the camera, and a T-junction at a
 * ring boundary.  A T-junction on a displaced surface is a visible crack, not
 * a subtle one, because the two sides evaluate the wave at different points.
 */

#include <jce/middleware/scene/jce_water.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define RINGS 12
#define SEGS  32
#define INNER 2.0f
#define OUTER 4000.0f

/* ── 1. Counts are exact and self-consistent ───────────────────────────  */

static void test_sizes_are_exact(void)
{
    uint32_t v = 0u, i = 0u;
    TEST_ASSERT_TRUE(jce_water_ring_mesh_size(RINGS, SEGS, &v, &i));
    TEST_ASSERT_EQUAL_UINT32(1u + RINGS * SEGS, v);
    /* cap fan + one quad strip per gap */
    TEST_ASSERT_EQUAL_UINT32((SEGS + (RINGS - 1) * SEGS * 2) * 3u, i);

    /* Degenerate configurations are refused, not silently clamped: a caller
     * that asked for 2 segments wants a bug report, not a triangle. */
    TEST_ASSERT_FALSE(jce_water_ring_mesh_size(0, SEGS, &v, &i));
    TEST_ASSERT_FALSE(jce_water_ring_mesh_size(RINGS, 2, &v, &i));
    TEST_ASSERT_EQUAL_UINT32(0u, v);
}

/* ── 2. Density falls off with distance ────────────────────────────────
 *
 * This is the whole feature.  A uniform grid has constant spacing; rings must
 * have spacing that GROWS with radius, so screen-space triangle size stays
 * roughly constant from the camera out to the horizon. */

static void test_radial_spacing_grows_with_distance(void)
{
    uint32_t nv = 0u, ni = 0u;
    jce_water_ring_mesh_size(RINGS, SEGS, &nv, &ni);
    JceWaterRingVertex *verts = malloc(sizeof(JceWaterRingVertex) * nv);
    uint32_t *idx = malloc(sizeof(uint32_t) * ni);
    TEST_ASSERT_NOT_NULL(verts);
    TEST_ASSERT_TRUE(jce_water_ring_build(RINGS, SEGS, INNER, OUTER, verts, idx));

    /* Compare INTER-RING gaps only.  The centre-to-first-ring distance is the
     * cap fan, not a ring spacing, and with a small inner radius it can exceed
     * the first true gap -- an earlier version of this test folded it into the
     * sequence and failed on correct geometry. */
    float prev_gap = -1.0f;
    float prev_r = 0.0f;
    for (int r = 0; r < RINGS; ++r) {
        const float rad = verts[1u + (uint32_t)r * SEGS].radius;
        /* Strictly increasing: an out-of-order ring folds the mesh onto
         * itself, which reads as a shimmering band rather than an error. */
        TEST_ASSERT_TRUE(rad > prev_r);
        if (r > 0) {
            const float gap = rad - prev_r;
            if (prev_gap > 0.0f) TEST_ASSERT_TRUE(gap > prev_gap);
            prev_gap = gap;
        }
        prev_r = rad;
    }
    TEST_ASSERT_TRUE(prev_gap > 0.0f);   /* the loop actually compared gaps */
    /* And the outermost ring must actually reach the requested extent. */
    TEST_ASSERT_FLOAT_WITHIN(OUTER * 0.001f, OUTER, prev_r);

    free(verts);
    free(idx);
}

/* ── 3. No hole under the camera ───────────────────────────────────────  */

static void test_centre_is_capped(void)
{
    uint32_t nv = 0u, ni = 0u;
    jce_water_ring_mesh_size(RINGS, SEGS, &nv, &ni);
    JceWaterRingVertex *verts = malloc(sizeof(JceWaterRingVertex) * nv);
    uint32_t *idx = malloc(sizeof(uint32_t) * ni);
    jce_water_ring_build(RINGS, SEGS, INNER, OUTER, verts, idx);

    TEST_ASSERT_EQUAL_FLOAT(0.0f, verts[0].x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, verts[0].z);

    /* The centre vertex must be referenced exactly `segments` times -- once by
     * each cap triangle.  Zero references would leave a hole directly under
     * the camera, which is where a hole is least forgivable. */
    int centre_refs = 0;
    for (uint32_t i = 0; i < ni; ++i)
        if (idx[i] == 0u) centre_refs++;
    TEST_ASSERT_EQUAL_INT(SEGS, centre_refs);

    free(verts);
    free(idx);
}

/* ── 4. Watertight: every interior edge is shared by exactly two triangles ──
 *
 * This is the T-junction check.  In a closed annulus every edge is either on
 * the outer boundary (used once) or interior (used exactly twice).  An edge
 * used once INSIDE the mesh is a crack. */

typedef struct { uint32_t a, b, n; } EdgeRec;

static void edge_add(EdgeRec *tab, uint32_t cap, uint32_t a, uint32_t b)
{
    const uint32_t lo = (a < b) ? a : b, hi = (a < b) ? b : a;
    uint32_t h = (lo * 73856093u ^ hi * 19349663u) % cap;
    for (uint32_t probe = 0; probe < cap; ++probe) {
        EdgeRec *e = &tab[h];
        if (e->n == 0u) { e->a = lo; e->b = hi; e->n = 1u; return; }
        if (e->a == lo && e->b == hi) { e->n++; return; }
        h = (h + 1u) % cap;
    }
}

static void test_mesh_is_watertight(void)
{
    uint32_t nv = 0u, ni = 0u;
    jce_water_ring_mesh_size(RINGS, SEGS, &nv, &ni);
    JceWaterRingVertex *verts = malloc(sizeof(JceWaterRingVertex) * nv);
    uint32_t *idx = malloc(sizeof(uint32_t) * ni);
    jce_water_ring_build(RINGS, SEGS, INNER, OUTER, verts, idx);

    const uint32_t cap = ni * 4u;
    EdgeRec *tab = calloc(cap, sizeof(EdgeRec));
    TEST_ASSERT_NOT_NULL(tab);
    for (uint32_t t = 0; t < ni; t += 3u) {
        edge_add(tab, cap, idx[t + 0], idx[t + 1]);
        edge_add(tab, cap, idx[t + 1], idx[t + 2]);
        edge_add(tab, cap, idx[t + 2], idx[t + 0]);
    }

    /* Exactly `segments` edges may be used once: the outer rim. */
    uint32_t once = 0u, twice = 0u, more = 0u;
    for (uint32_t i = 0; i < cap; ++i) {
        if (tab[i].n == 0u) continue;
        if (tab[i].n == 1u) once++;
        else if (tab[i].n == 2u) twice++;
        else more++;
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)SEGS, once);
    TEST_ASSERT_EQUAL_UINT32(0u, more);
    TEST_ASSERT_TRUE(twice > 0u);

    free(tab);
    free(verts);
    free(idx);
}

/* ── 5. Rings beat a uniform grid where it matters ─────────────────────
 *
 * The shipped grid is 64x64 quads = 8450 vertices spread uniformly over the
 * whole extent.  Rings must cover the same extent with no more vertices while
 * putting far finer detail near the camera -- that trade IS the feature. */

static void test_rings_are_finer_near_the_camera(void)
{
    uint32_t nv = 0u, ni = 0u;
    jce_water_ring_mesh_size(RINGS, SEGS, &nv, &ni);
    TEST_ASSERT_TRUE(nv <= 65u * 65u);       /* no more than the 64x64 grid */

    JceWaterRingVertex *verts = malloc(sizeof(JceWaterRingVertex) * nv);
    uint32_t *idx = malloc(sizeof(uint32_t) * ni);
    jce_water_ring_build(RINGS, SEGS, INNER, OUTER, verts, idx);

    /* A uniform grid spanning 2*OUTER with 64 quads has ~125 m spacing.  The
     * innermost ring must be dramatically finer than that. */
    const float grid_spacing = (2.0f * OUTER) / 64.0f;
    TEST_ASSERT_TRUE(verts[1].radius < grid_spacing * 0.1f);

    free(verts);
    free(idx);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sizes_are_exact);
    RUN_TEST(test_radial_spacing_grows_with_distance);
    RUN_TEST(test_centre_is_capped);
    RUN_TEST(test_mesh_is_watertight);
    RUN_TEST(test_rings_are_finer_near_the_camera);
    return UNITY_END();
}
