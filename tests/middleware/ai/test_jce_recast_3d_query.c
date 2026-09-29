/*
 * test_jce_recast_3d_query.c — Unit tests for jce_recast_find_path_3d (L3 AI).
 *
 * Bakes a real single-tile Detour navmesh from a triangle soup that spans two
 * floor heights connected by a ramp:
 *
 *      lower platform (Y=0)  ──ramp──▶  upper platform (Y=2)
 *
 * then runs jce_recast_find_path_3d between a point on the lower floor and a
 * point on the upper floor.  The whole path goes through the live
 * dtNavMeshQuery (findNearestPoly → findPath → findStraightPath →
 * getPolyHeight).  We assert that the per-waypoint Y is the *real* navmesh
 * surface height (not the forced Y=0 of the 2D variant): the final waypoint(s)
 * on the upper platform must land near Y=2, and at least one returned waypoint
 * must be clearly non-zero.
 */

#include "unity.h"

#include <jce/middleware/ai/jce_navmesh_recast.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

void setUp(void)    {}
void tearDown(void) {}

#define EPS 0.30f   /* generous: Recast detail height vs. exact surface. */

/* ------------------------------------------------------------------ */
/* Continuous walkable strip across two floor heights.                 */
/*                                                                     */
/*   lower platform  X:[0,4]   Y=0                                     */
/*   ramp            X:[4,8]   Y:0->2  (slope ~26.5deg, < 45deg)       */
/*   upper platform  X:[8,12]  Y=2                                     */
/*   all with Z:[0,4]                                                  */
/*                                                                     */
/* Vertices are tightly-packed x,y,z floats; quads are split into two  */
/* CCW (from above, +Y normal) triangles to match the engine winding.  */
/* ------------------------------------------------------------------ */

/* 8 grid columns (X = 0,4,8,12 ... actually X = 0,4,8,12) over 2 rows  */
/* (Z = 0, 4).  We list 4 X-stations x 2 Z-rows = 8 verts.            */
static const float g_verts[] = {
    /* idx  X     Y     Z */
    /* 0 */  0.0f, 0.0f, 0.0f,
    /* 1 */  4.0f, 0.0f, 0.0f,
    /* 2 */  8.0f, 2.0f, 0.0f,
    /* 3 */ 12.0f, 2.0f, 0.0f,
    /* 4 */  0.0f, 0.0f, 4.0f,
    /* 5 */  4.0f, 0.0f, 4.0f,
    /* 6 */  8.0f, 2.0f, 4.0f,
    /* 7 */ 12.0f, 2.0f, 4.0f,
};
static const uint32_t g_vert_count = 8;

/* Three quads (lower flat, ramp, upper flat), each two triangles.
 * Row Z=0 verts: 0,1,2,3   Row Z=4 verts: 4,5,6,7.
 * Quad k spans X-stations (k, k+1):
 *   tri A = (zk, zk+1, z(k+1)+1)  tri B = (zk, z(k+1)+1, z(k+1))
 * where zk is the Z=0 vert and z(k)+4 is the Z=4 vert.  CCW from +Y. */
static const uint32_t g_indices[] = {
    /* quad 0: lower flat  (verts 0,1 / 4,5) */
    0, 4, 5,   0, 5, 1,
    /* quad 1: ramp        (verts 1,2 / 5,6) */
    1, 5, 6,   1, 6, 2,
    /* quad 2: upper flat  (verts 2,3 / 6,7) */
    2, 6, 7,   2, 7, 3,
};
static const uint32_t g_tri_count = 6;

static JceRecastNavMesh *bake(void)
{
    JceRecastConfig cfg;
    jce_recast_default_config(&cfg);
    /* Finer voxels so the small 4x4 platforms survive region filtering and
     * the ramp connects to both flats. */
    cfg.cell_size       = 0.15f;
    cfg.cell_height     = 0.10f;
    cfg.walkable_radius = 0.0f;   /* don't erode away these narrow strips */
    cfg.region_min_size = 1;
    cfg.region_merge_size = 4;
    return jce_recast_build(g_verts, g_vert_count,
                            g_indices, g_tri_count, &cfg);
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_bake_succeeds(void)
{
    JceRecastNavMesh *nm = bake();
    TEST_ASSERT_NOT_NULL(nm);
    JceRecastStats st = {0};
    jce_recast_get_stats(nm, &st);
    TEST_ASSERT_TRUE(st.polygon_count > 0);
    jce_recast_destroy(nm);
}

/* The core regression: waypoints on the upper floor must carry real Y. */
static void test_find_path_3d_emits_real_y(void)
{
    JceRecastNavMesh *nm = bake();
    TEST_ASSERT_NOT_NULL(nm);

    float out[64 * 3];
    /* Lower floor (Y=0) -> upper floor (Y=2). */
    int n = jce_recast_find_path_3d(nm,
                                    1.0f, 0.0f, 2.0f,    /* start on lower */
                                    11.0f, 2.0f, 2.0f,   /* goal on upper  */
                                    out, 64);
    TEST_ASSERT_TRUE(n > 0);

    /* Not every waypoint may be on the top, but the path must climb: at
     * least one returned waypoint Y must be clearly non-zero (would be
     * exactly 0 with the broken Y=0 behaviour). */
    int found_high = 0;
    float max_y = -1e9f;
    for (int i = 0; i < n; ++i) {
        float y = out[i * 3 + 1];
        if (y > max_y) max_y = y;
        if (y > 0.5f) found_high = 1;
        /* Y must be a finite, navmesh-plausible height in [-eps, 2+eps]. */
        TEST_ASSERT_TRUE(y > -EPS);
        TEST_ASSERT_TRUE(y < 2.0f + EPS);
    }
    TEST_ASSERT_TRUE_MESSAGE(found_high,
        "no waypoint climbed off Y=0 — 3D height resolution did not run");

    /* The final waypoint is the goal on the upper platform: Y ~ 2. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, out[(n - 1) * 3 + 1]);
    /* And the highest point reached should be near the top floor. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, max_y);

    jce_recast_destroy(nm);
}

/* Same-floor path on the lower platform stays near Y=0 (sanity that we
 * are reading the surface, not just always returning the goal Y). */
static void test_find_path_3d_lower_floor_stays_low(void)
{
    JceRecastNavMesh *nm = bake();
    TEST_ASSERT_NOT_NULL(nm);

    float out[32 * 3];
    int n = jce_recast_find_path_3d(nm,
                                    0.5f, 0.0f, 1.0f,
                                    3.5f, 0.0f, 3.0f,
                                    out, 32);
    TEST_ASSERT_TRUE(n > 0);
    for (int i = 0; i < n; ++i) {
        TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out[i * 3 + 1]);
    }
    jce_recast_destroy(nm);
}

/* Guards: null / zero-capacity inputs return 0 without dereferencing. */
static void test_find_path_3d_guards(void)
{
    float out[3];
    TEST_ASSERT_EQUAL_INT(0, jce_recast_find_path_3d(NULL,
                                0,0,0, 1,1,1, out, 1));
    JceRecastNavMesh *nm = bake();
    TEST_ASSERT_NOT_NULL(nm);
    TEST_ASSERT_EQUAL_INT(0, jce_recast_find_path_3d(nm,
                                0,0,0, 1,2,1, NULL, 1));
    TEST_ASSERT_EQUAL_INT(0, jce_recast_find_path_3d(nm,
                                0,0,0, 1,2,1, out, 0));
    jce_recast_destroy(nm);
}

/* ------------------------------------------------------------------ */
/* runner                                                             */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bake_succeeds);
    RUN_TEST(test_find_path_3d_emits_real_y);
    RUN_TEST(test_find_path_3d_lower_floor_stays_low);
    RUN_TEST(test_find_path_3d_guards);
    return UNITY_END();
}
