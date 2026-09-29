/*
 * test_jce_space_partition.c — Unit tests for jce_space_partition.h (L4).
 *
 * Backed by a uniform grid (current backend for BVH/octree/grid alias).
 * We test object insert/update/remove + AABB/sphere/raycast queries
 * with a tiny scene of unit cubes.
 */

#include "unity.h"

#include <jce/middleware/scene/jce_space_partition.h>
#include <jce/os/core/jce_math.h>

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static JceAABB aabb(float cx, float cy, float cz, float r)
{
    JceAABB b;
    b.min = jce_v3(cx - r, cy - r, cz - r);
    b.max = jce_v3(cx + r, cy + r, cz + r);
    return b;
}

static JceSpaceIndex *make_idx(uint32_t cap)
{
    JceSpaceConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.type         = JCE_SPACE_GRID;
    cfg.world_bounds = aabb(0.0f, 0.0f, 0.0f, 100.0f);
    cfg.max_objects  = cap;
    return jce_space_create(&cfg);
}

static bool contains(const uint32_t *arr, uint32_t n, uint32_t v)
{
    for (uint32_t i = 0; i < n; ++i) if (arr[i] == v) return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

static void test_create_destroy(void)
{
    JceSpaceIndex *idx = make_idx(64);
    TEST_ASSERT_NOT_NULL(idx);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_space_object_count(idx));
    jce_space_destroy(idx);
}

static void test_destroy_null_safe(void)
{
    jce_space_destroy(NULL);
    TEST_PASS();
}

/* ------------------------------------------------------------------ */
/* Insert / remove / count                                              */
/* ------------------------------------------------------------------ */

static void test_insert_increments_count(void)
{
    JceSpaceIndex *idx = make_idx(64);
    uint32_t h1 = jce_space_insert(idx, aabb(0, 0, 0, 0.5f), 100);
    uint32_t h2 = jce_space_insert(idx, aabb(5, 0, 0, 0.5f), 200);
    TEST_ASSERT_NOT_EQUAL(0u, h1);
    TEST_ASSERT_NOT_EQUAL(0u, h2);
    TEST_ASSERT_EQUAL_UINT32(2u, jce_space_object_count(idx));
    jce_space_destroy(idx);
}

static void test_remove_decrements_count(void)
{
    JceSpaceIndex *idx = make_idx(64);
    uint32_t h = jce_space_insert(idx, aabb(0, 0, 0, 0.5f), 1);
    jce_space_remove(idx, h);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_space_object_count(idx));
    jce_space_destroy(idx);
}

/* ------------------------------------------------------------------ */
/* AABB query                                                           */
/* ------------------------------------------------------------------ */

static void test_aabb_query_finds_overlapping_objects(void)
{
    JceSpaceIndex *idx = make_idx(64);
    (void)jce_space_insert(idx, aabb(0,  0, 0, 0.5f),  10);
    (void)jce_space_insert(idx, aabb(5,  0, 0, 0.5f),  20);
    (void)jce_space_insert(idx, aabb(20, 0, 0, 0.5f),  30);

    uint32_t out[8] = {0};
    uint32_t n = jce_space_query_aabb(idx, aabb(2.5f, 0, 0, 3.0f), out, 8);
    /* Region [-0.5..5.5] should contain user_ids 10 and 20 but not 30. */
    TEST_ASSERT_TRUE(n >= 2u);
    TEST_ASSERT_TRUE(contains(out, n, 10u));
    TEST_ASSERT_TRUE(contains(out, n, 20u));
    TEST_ASSERT_FALSE(contains(out, n, 30u));
    jce_space_destroy(idx);
}

static void test_aabb_query_empty_when_no_overlap(void)
{
    JceSpaceIndex *idx = make_idx(64);
    (void)jce_space_insert(idx, aabb(0, 0, 0, 0.5f), 10);

    uint32_t out[8] = {0};
    uint32_t n = jce_space_query_aabb(idx, aabb(50, 0, 0, 1.0f), out, 8);
    TEST_ASSERT_EQUAL_UINT32(0u, n);
    jce_space_destroy(idx);
}

/* ------------------------------------------------------------------ */
/* Sphere query                                                         */
/* ------------------------------------------------------------------ */

static void test_sphere_query_finds_near_objects(void)
{
    JceSpaceIndex *idx = make_idx(64);
    (void)jce_space_insert(idx, aabb(0, 0, 0, 0.5f), 1);
    (void)jce_space_insert(idx, aabb(3, 0, 0, 0.5f), 2);
    (void)jce_space_insert(idx, aabb(50, 0, 0, 0.5f), 3);

    uint32_t out[8] = {0};
    uint32_t n = jce_space_query_sphere(idx, jce_v3(0, 0, 0), 4.0f, out, 8);
    TEST_ASSERT_TRUE(contains(out, n, 1u));
    TEST_ASSERT_TRUE(contains(out, n, 2u));
    TEST_ASSERT_FALSE(contains(out, n, 3u));
    jce_space_destroy(idx);
}

/* ------------------------------------------------------------------ */
/* Raycast                                                              */
/* ------------------------------------------------------------------ */

static void test_raycast_hits_nearest_object(void)
{
    JceSpaceIndex *idx = make_idx(64);
    (void)jce_space_insert(idx, aabb(5,  0, 0, 0.5f), 11);
    (void)jce_space_insert(idx, aabb(10, 0, 0, 0.5f), 22);

    JceSpaceRayHit hit = {0};
    bool ok = jce_space_raycast(idx, jce_v3(0, 0, 0), jce_v3(1, 0, 0),
                                20.0f, &hit);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(11u, hit.user_id);
    TEST_ASSERT_TRUE(hit.distance > 0.0f && hit.distance < 6.0f);
    jce_space_destroy(idx);
}

static void test_raycast_miss_returns_false(void)
{
    JceSpaceIndex *idx = make_idx(64);
    (void)jce_space_insert(idx, aabb(0, 5, 0, 0.5f), 1);

    JceSpaceRayHit hit = {0};
    bool ok = jce_space_raycast(idx, jce_v3(0, 0, 0), jce_v3(1, 0, 0),
                                50.0f, &hit);
    TEST_ASSERT_FALSE(ok);
    jce_space_destroy(idx);
}

/* ------------------------------------------------------------------ */
/* Update                                                                */
/* ------------------------------------------------------------------ */

static void test_update_repositions_object(void)
{
    JceSpaceIndex *idx = make_idx(64);
    uint32_t h = jce_space_insert(idx, aabb(0, 0, 0, 0.5f), 7);

    /* Move object far away from origin. */
    jce_space_update(idx, h, aabb(40, 0, 0, 0.5f));

    uint32_t out[4] = {0};
    uint32_t n1 = jce_space_query_aabb(idx, aabb(0, 0, 0, 1.0f), out, 4);
    TEST_ASSERT_EQUAL_UINT32(0u, n1);
    uint32_t n2 = jce_space_query_aabb(idx, aabb(40, 0, 0, 1.0f), out, 4);
    TEST_ASSERT_TRUE(n2 >= 1u);
    TEST_ASSERT_TRUE(contains(out, n2, 7u));
    jce_space_destroy(idx);
}

/* ------------------------------------------------------------------ */
/* Reset preserves capacity                                             */
/* ------------------------------------------------------------------ */

static void test_reset_clears_objects(void)
{
    JceSpaceIndex *idx = make_idx(64);
    (void)jce_space_insert(idx, aabb(0, 0, 0, 0.5f), 1);
    (void)jce_space_insert(idx, aabb(2, 0, 0, 0.5f), 2);
    TEST_ASSERT_EQUAL_UINT32(2u, jce_space_object_count(idx));

    jce_space_reset(idx, NULL);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_space_object_count(idx));

    /* Index should still be usable after reset. */
    uint32_t h = jce_space_insert(idx, aabb(0, 0, 0, 0.5f), 5);
    TEST_ASSERT_NOT_EQUAL(0u, h);
    jce_space_destroy(idx);
}

/* ------------------------------------------------------------------ */
/* Extent-sized resolution + occupied-cell bookkeeping (P1 #4)          */
/* ------------------------------------------------------------------ */

static void test_extent_sized_resolution(void)
{
    /* Small world (±100 => extent 200) gets a modest grid; a large world
     * (±5000 => extent 10000) gets a much finer one — proving res scales with
     * the world instead of a hard 32. */
    JceSpaceConfig small_cfg; memset(&small_cfg, 0, sizeof(small_cfg));
    small_cfg.type = JCE_SPACE_GRID;
    small_cfg.world_bounds = aabb(0, 0, 0, 100.0f);
    JceSpaceIndex *small = jce_space_create(&small_cfg);

    JceSpaceConfig big_cfg; memset(&big_cfg, 0, sizeof(big_cfg));
    big_cfg.type = JCE_SPACE_GRID;
    big_cfg.world_bounds = aabb(0, 0, 0, 5000.0f);
    JceSpaceIndex *big = jce_space_create(&big_cfg);

    uint32_t rs[3] = {0,0,0}, rb[3] = {0,0,0};
    jce_space_resolution(small, rs);
    jce_space_resolution(big, rb);
    /* Each axis is clamped to [4,256]; the big world must be strictly finer. */
    TEST_ASSERT_TRUE(rs[0] >= 4u && rs[0] <= 256u);
    TEST_ASSERT_TRUE(rb[0] >= 4u && rb[0] <= 256u);
    TEST_ASSERT_TRUE(rb[0] > rs[0]);
    /* Total cells stays bounded (no 32k over-allocation for the tiny world). */
    TEST_ASSERT_TRUE(jce_space_cell_count(small) < jce_space_cell_count(big));
    jce_space_destroy(small);
    jce_space_destroy(big);
}

static void test_occupied_cells_track_objects(void)
{
    /* World ±100, 8³ grid => 25-unit cells with boundaries at ...,-25,0,25,...
     * Place small cubes at cell CENTRES (offset 12.5 from a boundary) so each
     * sits cleanly inside ONE cell — then occupied count == object count. */
    JceSpaceIndex *idx = make_idx(64);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_space_occupied_cell_count(idx));

    uint32_t h1 = jce_space_insert(idx, aabb(-37.5f, 12.5f, 12.5f, 0.5f), 1);
    uint32_t h2 = jce_space_insert(idx, aabb( 37.5f, 12.5f, 12.5f, 0.5f), 2);
    TEST_ASSERT_EQUAL_UINT32(2u, jce_space_occupied_cell_count(idx));

    /* Removing one frees its cell back out of the occupied list. */
    jce_space_remove(idx, h1);
    TEST_ASSERT_EQUAL_UINT32(1u, jce_space_occupied_cell_count(idx));
    jce_space_remove(idx, h2);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_space_occupied_cell_count(idx));
    jce_space_destroy(idx);
}

static void test_set_user_id_remaps_query_payload(void)
{
    JceSpaceIndex *idx = make_idx(64);
    uint32_t h = jce_space_insert(idx, aabb(0, 0, 0, 0.5f), 100);
    jce_space_set_user_id(idx, h, 777);

    uint32_t out[4] = {0};
    uint32_t n = jce_space_query_aabb(idx, aabb(0, 0, 0, 1.0f), out, 4);
    TEST_ASSERT_TRUE(n >= 1u);
    TEST_ASSERT_TRUE(contains(out, n, 777u));
    TEST_ASSERT_FALSE(contains(out, n, 100u));
    jce_space_destroy(idx);
}

/* Persistent churn: repeated insert/update/remove must not corrupt the
 * occupied-cell list or leak objects (mirrors the cull broad-phase lifecycle). */
static void test_persistent_churn_stable(void)
{
    JceSpaceIndex *idx = make_idx(256);
    uint32_t h[32];
    for (int i = 0; i < 32; i++)
        h[i] = jce_space_insert(idx, aabb((float)(i - 16) * 5.0f, 0, 0, 0.5f),
                                (uint32_t)i);
    TEST_ASSERT_EQUAL_UINT32(32u, jce_space_object_count(idx));

    /* Move them all to one cell-interior point (12.5 = a cell centre, so the
     * small cube doesn't straddle a 25-unit boundary), then fan back out.  The
     * occupied count must collapse then re-expand with no double-count/underflow. */
    for (int i = 0; i < 32; i++)
        jce_space_update(idx, h[i], aabb(12.5f, 12.5f, 12.5f, 0.5f));
    TEST_ASSERT_EQUAL_UINT32(1u, jce_space_occupied_cell_count(idx));
    for (int i = 0; i < 32; i++)
        jce_space_update(idx, h[i], aabb((float)(i - 16) * 5.0f, 0, 0, 0.5f));
    TEST_ASSERT_TRUE(jce_space_occupied_cell_count(idx) > 1u);

    /* Remove half; survivors still query correctly. */
    for (int i = 0; i < 32; i += 2) jce_space_remove(idx, h[i]);
    TEST_ASSERT_EQUAL_UINT32(16u, jce_space_object_count(idx));
    uint32_t out[64] = {0};
    uint32_t n = jce_space_query_aabb(idx, aabb(0, 0, 0, 200.0f), out, 64);
    TEST_ASSERT_EQUAL_UINT32(16u, n);
    jce_space_destroy(idx);
}

/* Reset that changes the world extent must re-derive resolution and stay
 * usable (the cull path resets with new bounds when the world grows). */
static void test_reset_resizes_grid(void)
{
    JceSpaceIndex *idx = make_idx(64);
    uint32_t small_cells = jce_space_cell_count(idx);

    JceAABB big = aabb(0, 0, 0, 5000.0f);
    jce_space_reset(idx, &big);
    TEST_ASSERT_TRUE(jce_space_cell_count(idx) > small_cells);

    uint32_t h = jce_space_insert(idx, aabb(2000, 0, 0, 0.5f), 9);
    TEST_ASSERT_NOT_EQUAL(0u, h);
    uint32_t out[4] = {0};
    uint32_t n = jce_space_query_aabb(idx, aabb(2000, 0, 0, 1.0f), out, 4);
    TEST_ASSERT_TRUE(contains(out, n, 9u));
    jce_space_destroy(idx);
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_destroy);
    RUN_TEST(test_destroy_null_safe);
    RUN_TEST(test_insert_increments_count);
    RUN_TEST(test_remove_decrements_count);
    RUN_TEST(test_aabb_query_finds_overlapping_objects);
    RUN_TEST(test_aabb_query_empty_when_no_overlap);
    RUN_TEST(test_sphere_query_finds_near_objects);
    RUN_TEST(test_raycast_hits_nearest_object);
    RUN_TEST(test_raycast_miss_returns_false);
    RUN_TEST(test_update_repositions_object);
    RUN_TEST(test_reset_clears_objects);
    RUN_TEST(test_extent_sized_resolution);
    RUN_TEST(test_occupied_cells_track_objects);
    RUN_TEST(test_set_user_id_remaps_query_payload);
    RUN_TEST(test_persistent_churn_stable);
    RUN_TEST(test_reset_resizes_grid);
    return UNITY_END();
}
