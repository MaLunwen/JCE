/*
 * test_jce_navmesh_obstacles.c — Unit tests for the DetourTileCache dynamic
 * obstacle path (FEATURE 6.1+ slice; L3 AI).
 *
 * Bakes a *tile-cache-backed* navmesh (jce_recast_build_tiled) over a single
 * large flat floor, then exercises the REAL dtTileCache update + dtNavMeshQuery
 * round-trip (NOT a mock):
 *
 *   1. find a near-straight path A -> B across the open floor;
 *   2. drop a cylinder obstacle squarely ON that path and re-query — assert the
 *      new path DETOURS (it is longer, and no waypoint sits inside the obstacle
 *      footprint);
 *   3. remove the obstacle and re-query — assert the path collapses back to the
 *      original near-straight length;
 *   4. repeat with an axis-aligned BOX obstacle.
 *
 * Each add/remove triggers dtTileCache::update(), which re-cuts the affected
 * tiles and re-inserts them into the live dtNavMesh, so the detour is produced
 * by the genuine Recast/Detour dynamic-obstacle machinery.
 */

#include "unity.h"

#include <jce/middleware/ai/jce_navmesh_recast.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

void setUp(void)    {}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* One big flat floor, X:[0,40] Z:[0,40] at Y=0, two CCW (from +Y)     */
/* triangles.  Large enough to span several tile-cache tiles so an     */
/* obstacle in the middle only touches interior tiles.                 */
/* ------------------------------------------------------------------ */
static const float g_verts[] = {
    /* 0 */   0.0f, 0.0f,  0.0f,
    /* 1 */  40.0f, 0.0f,  0.0f,
    /* 2 */  40.0f, 0.0f, 40.0f,
    /* 3 */   0.0f, 0.0f, 40.0f,
};
static const uint32_t g_vert_count = 4;

/* CCW winding from above (+Y normal): (0,3,2) (0,2,1). */
static const uint32_t g_indices[] = {
    0, 3, 2,
    0, 2, 1,
};
static const uint32_t g_tri_count = 2;

static JceRecastNavMesh *bake_tiled(void)
{
    JceRecastConfig cfg;
    jce_recast_default_config(&cfg);
    cfg.cell_size         = 0.30f;
    cfg.cell_height       = 0.20f;
    cfg.walkable_radius   = 0.0f;   /* keep the floor full-area for the test */
    cfg.region_min_size   = 1;
    cfg.region_merge_size = 4;
    return jce_recast_build_tiled(g_verts, g_vert_count,
                                  g_indices, g_tri_count, &cfg);
}

/* Sum of segment lengths of an XZ polyline that starts at (sx,sz). */
static float path_len_xz(float sx, float sz, const float *xz, int n)
{
    float total = 0.0f;
    float px = sx, pz = sz;
    for (int i = 0; i < n; ++i) {
        float x = xz[i * 2 + 0];
        float z = xz[i * 2 + 1];
        float dx = x - px, dz = z - pz;
        total += sqrtf(dx * dx + dz * dz);
        px = x; pz = z;
    }
    return total;
}

/* Closest approach (XZ) of the polyline (incl. start) to a point. */
static float path_min_dist_to_point_xz(float sx, float sz,
                                       const float *xz, int n,
                                       float ox, float oz)
{
    float best = sqrtf((sx - ox) * (sx - ox) + (sz - oz) * (sz - oz));
    for (int i = 0; i < n; ++i) {
        float dx = xz[i * 2 + 0] - ox;
        float dz = xz[i * 2 + 1] - oz;
        float d = sqrtf(dx * dx + dz * dz);
        if (d < best) best = d;
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_build_tiled_succeeds(void)
{
    JceRecastNavMesh *nm = bake_tiled();
    TEST_ASSERT_NOT_NULL(nm);
    TEST_ASSERT_TRUE(jce_recast_has_tile_cache(nm));
    JceRecastStats st = {0};
    jce_recast_get_stats(nm, &st);
    TEST_ASSERT_TRUE(st.polygon_count > 0);
    jce_recast_destroy(nm);
}

/* The core regression: cylinder obstacle on the straight path forces a detour
 * via the REAL dtTileCache rebuild, and removing it restores the straight path. */
static void test_cylinder_obstacle_detours_then_restores(void)
{
    JceRecastNavMesh *nm = bake_tiled();
    TEST_ASSERT_NOT_NULL(nm);

    /* Straight run from left edge to right edge through the centre row. */
    const float ax = 4.0f,  az = 20.0f;
    const float bx = 36.0f, bz = 20.0f;
    /* Obstacle planted dead-centre on that line. */
    const float ox = 20.0f, oz = 20.0f;
    const float obstacle_radius = 3.0f;

    float wp[128 * 2];

    int n0 = jce_recast_find_path(nm, ax, az, bx, bz, wp, 128);
    TEST_ASSERT_TRUE_MESSAGE(n0 > 0, "no baseline path on open floor");
    float len0 = path_len_xz(ax, az, wp, n0);
    /* Baseline is essentially a straight line (~32 m). */
    TEST_ASSERT_TRUE_MESSAGE(len0 < 36.0f, "baseline path is not near-straight");

    /* Drop a cylinder obstacle ON the path. */
    JceRecastObstacleRef ob =
        jce_recast_add_obstacle(nm, ox, 0.0f, oz, obstacle_radius, 4.0f);
    TEST_ASSERT_TRUE_MESSAGE(ob != 0, "add_obstacle returned null ref");

    int n1 = jce_recast_find_path(nm, ax, az, bx, bz, wp, 128);
    TEST_ASSERT_TRUE_MESSAGE(n1 > 0, "no path after obstacle (should detour)");
    float len1 = path_len_xz(ax, az, wp, n1);

    /* The detour must be measurably longer than the straight baseline ... */
    TEST_ASSERT_TRUE_MESSAGE(len1 > len0 + 0.5f,
        "path did not lengthen — obstacle did not affect the navmesh");
    /* ... and must keep clear of the obstacle footprint (allow a small
     * voxel-quantisation slack inside the radius). */
    float clearance =
        path_min_dist_to_point_xz(ax, az, wp, n1, ox, oz);
    TEST_ASSERT_TRUE_MESSAGE(clearance > obstacle_radius - 0.75f,
        "path passes through the obstacle footprint");

    /* Remove it: the path must collapse back toward straight. */
    TEST_ASSERT_TRUE(jce_recast_remove_obstacle(nm, ob));

    int n2 = jce_recast_find_path(nm, ax, az, bx, bz, wp, 128);
    TEST_ASSERT_TRUE_MESSAGE(n2 > 0, "no path after obstacle removal");
    float len2 = path_len_xz(ax, az, wp, n2);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.5f, len0, len2,
        "path did not return to straight after obstacle removal");

    jce_recast_destroy(nm);
}

/* Box obstacle variant: same detour-then-restore contract. */
static void test_box_obstacle_detours_then_restores(void)
{
    JceRecastNavMesh *nm = bake_tiled();
    TEST_ASSERT_NOT_NULL(nm);

    const float ax = 4.0f,  az = 20.0f;
    const float bx = 36.0f, bz = 20.0f;

    float wp[128 * 2];

    int n0 = jce_recast_find_path(nm, ax, az, bx, bz, wp, 128);
    TEST_ASSERT_TRUE(n0 > 0);
    float len0 = path_len_xz(ax, az, wp, n0);
    TEST_ASSERT_TRUE(len0 < 36.0f);

    /* AABB wall straddling the path centre: X:[18,22] Z:[16,24]. */
    JceRecastObstacleRef ob =
        jce_recast_add_box_obstacle(nm, 18.0f, -1.0f, 16.0f,
                                        22.0f,  4.0f, 24.0f);
    TEST_ASSERT_TRUE_MESSAGE(ob != 0, "add_box_obstacle returned null ref");

    int n1 = jce_recast_find_path(nm, ax, az, bx, bz, wp, 128);
    TEST_ASSERT_TRUE(n1 > 0);
    float len1 = path_len_xz(ax, az, wp, n1);
    TEST_ASSERT_TRUE_MESSAGE(len1 > len0 + 0.5f,
        "box obstacle did not lengthen the path");

    /* No waypoint may sit inside the box footprint (XZ). */
    float px = ax, pz = az;
    int inside = 0;
    for (int i = 0; i < n1; ++i) {
        float x = wp[i * 2 + 0], z = wp[i * 2 + 1];
        if (x > 18.25f && x < 21.75f && z > 16.25f && z < 23.75f) inside = 1;
        px = x; pz = z;
    }
    (void)px; (void)pz;
    TEST_ASSERT_FALSE_MESSAGE(inside, "a waypoint landed inside the box");

    TEST_ASSERT_TRUE(jce_recast_remove_obstacle(nm, ob));
    int n2 = jce_recast_find_path(nm, ax, az, bx, bz, wp, 128);
    TEST_ASSERT_TRUE(n2 > 0);
    float len2 = path_len_xz(ax, az, wp, n2);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.5f, len0, len2,
        "path did not return to straight after box removal");

    jce_recast_destroy(nm);
}

/* The static path must be untouched: a navmesh built via jce_recast_build has
 * no tile cache and the obstacle calls are graceful no-ops. */
static void test_static_navmesh_has_no_tile_cache(void)
{
    JceRecastConfig cfg;
    jce_recast_default_config(&cfg);
    cfg.cell_size = 0.30f; cfg.cell_height = 0.20f;
    cfg.walkable_radius = 0.0f; cfg.region_min_size = 1; cfg.region_merge_size = 4;
    JceRecastNavMesh *nm = jce_recast_build(g_verts, g_vert_count,
                                            g_indices, g_tri_count, &cfg);
    TEST_ASSERT_NOT_NULL(nm);
    TEST_ASSERT_FALSE(jce_recast_has_tile_cache(nm));
    /* No-op on a static navmesh. */
    TEST_ASSERT_EQUAL_UINT32(0, jce_recast_add_obstacle(nm, 20,0,20, 3,4));
    TEST_ASSERT_EQUAL_UINT32(0,
        jce_recast_add_box_obstacle(nm, 18,-1,16, 22,4,24));
    TEST_ASSERT_FALSE(jce_recast_remove_obstacle(nm, 1));
    jce_recast_destroy(nm);
}

/* Guards: null / bad inputs return 0 / false without dereferencing. */
static void test_obstacle_guards(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, jce_recast_add_obstacle(NULL, 0,0,0, 1,1));
    TEST_ASSERT_EQUAL_UINT32(0, jce_recast_add_box_obstacle(NULL, 0,0,0, 1,1,1));
    TEST_ASSERT_FALSE(jce_recast_remove_obstacle(NULL, 1));
    TEST_ASSERT_FALSE(jce_recast_has_tile_cache(NULL));

    JceRecastNavMesh *nm = bake_tiled();
    TEST_ASSERT_NOT_NULL(nm);
    /* Bad radius/height rejected. */
    TEST_ASSERT_EQUAL_UINT32(0, jce_recast_add_obstacle(nm, 20,0,20, 0.0f, 4.0f));
    TEST_ASSERT_EQUAL_UINT32(0, jce_recast_add_obstacle(nm, 20,0,20, 3.0f, 0.0f));
    /* Removing ref 0 is a no-op false. */
    TEST_ASSERT_FALSE(jce_recast_remove_obstacle(nm, 0));
    jce_recast_destroy(nm);
}

/* ------------------------------------------------------------------ */
/* runner                                                             */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_build_tiled_succeeds);
    RUN_TEST(test_cylinder_obstacle_detours_then_restores);
    RUN_TEST(test_box_obstacle_detours_then_restores);
    RUN_TEST(test_static_navmesh_has_no_tile_cache);
    RUN_TEST(test_obstacle_guards);
    return UNITY_END();
}
