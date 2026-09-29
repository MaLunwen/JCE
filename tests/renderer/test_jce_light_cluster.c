/* test_jce_light_cluster.c
 *
 * Validation for the clustered-light-culling core (jce_light_cluster) — the
 * CPU foundation of Forward+ (roadmap 1.1).  The module shipped untested and
 * unwired; these tests lock in its froxel-assignment correctness before the
 * renderer/shader is wired to consume it.
 */

#include <jce/renderer/jce_light_cluster.h>
#include <jce/os/core/jce_math.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* Identity view/proj (p[0]=p[5]=1 → 90° fov, tan_half=1); view space == world
 * space, with +Z forward into [near,far]. */
static JceLightCluster *make_cluster(uint32_t cx, uint32_t cy, uint32_t cz, uint32_t maxpc)
{
    JceLightClusterDesc d = { cx, cy, cz, 64u, maxpc };
    JceLightCluster *lc = jce_light_cluster_create(&d);
    TEST_ASSERT_NOT_NULL(lc);
    jce_mat4 I = jce_m4_identity();
    jce_light_cluster_set_camera(lc, &I, &I, 1.0f, 100.0f);
    return lc;
}

static void test_create_validation(void)
{
    JceLightClusterDesc bad = { 0, 4, 4, 64, 8 };
    TEST_ASSERT_NULL(jce_light_cluster_create(&bad));
    TEST_ASSERT_NULL(jce_light_cluster_create(NULL));
    JceLightClusterDesc ok = { 4, 4, 4, 64, 8 };
    JceLightCluster *lc = jce_light_cluster_create(&ok);
    TEST_ASSERT_NOT_NULL(lc);
    jce_light_cluster_destroy(lc);
}

static void test_slice_monotonic(void)
{
    JceLightCluster *lc = make_cluster(4, 4, 16, 8);
    TEST_ASSERT_EQUAL_UINT32(0u,  jce_light_cluster_slice_for_view_z(lc, 0.5f));   /* <= near */
    TEST_ASSERT_EQUAL_UINT32(15u, jce_light_cluster_slice_for_view_z(lc, 200.0f)); /* >= far  */
    uint32_t prev = 0;
    for (float z = 1.0f; z <= 100.0f; z += 1.0f) {
        uint32_t s = jce_light_cluster_slice_for_view_z(lc, z);
        TEST_ASSERT_TRUE(s >= prev);
        TEST_ASSERT_TRUE(s < 16u);
        prev = s;
    }
    jce_light_cluster_destroy(lc);
}

static void test_build_guards(void)
{
    JceLightClusterDesc ok = { 4, 4, 4, 64, 8 };
    JceLightCluster *lc = jce_light_cluster_create(&ok);
    TEST_ASSERT_FALSE(jce_light_cluster_build(lc, NULL, 0));  /* no camera yet */
    jce_mat4 I = jce_m4_identity();
    jce_light_cluster_set_camera(lc, &I, &I, 1.0f, 100.0f);
    TEST_ASSERT_TRUE(jce_light_cluster_build(lc, NULL, 0));   /* 0 lights ok */
    JceLightClusterResult r = jce_light_cluster_get_result(lc);
    TEST_ASSERT_EQUAL_UINT32(0u, r.total_lights_in);
    TEST_ASSERT_EQUAL_UINT32(0u, r.total_assignments);
    jce_light_cluster_destroy(lc);
}

static void test_light_in_front_assigned(void)
{
    JceLightCluster *lc = make_cluster(8, 8, 16, 16);
    JceLightProxy L = { { 0.0f, 0.0f, 10.0f }, 2.0f, 777u };
    TEST_ASSERT_TRUE(jce_light_cluster_build(lc, &L, 1));
    JceLightClusterResult r = jce_light_cluster_get_result(lc);
    TEST_ASSERT_EQUAL_UINT32(1u, r.total_lights_in);
    TEST_ASSERT_TRUE(r.total_assignments > 0u);
    bool found = false;
    uint32_t cells = r.cells_x * r.cells_y * r.slices_z;
    for (uint32_t c = 0; c < cells && !found; ++c)
        for (uint32_t k = 0; k < r.cell_counts[c]; ++k)
            if (r.cell_indices[c * r.max_per_cell + k] == 0u) { found = true; break; }
    TEST_ASSERT_TRUE(found);
    jce_light_cluster_destroy(lc);
}

static void test_light_behind_not_assigned(void)
{
    JceLightCluster *lc = make_cluster(8, 8, 16, 16);
    JceLightProxy L = { { 0.0f, 0.0f, -50.0f }, 2.0f, 1u };   /* behind the near plane */
    TEST_ASSERT_TRUE(jce_light_cluster_build(lc, &L, 1));
    JceLightClusterResult r = jce_light_cluster_get_result(lc);
    TEST_ASSERT_EQUAL_UINT32(0u, r.total_assignments);
    jce_light_cluster_destroy(lc);
}

static void test_huge_light_covers_many_cells(void)
{
    JceLightCluster *lc = make_cluster(8, 8, 16, 64);
    JceLightProxy L = { { 0.0f, 0.0f, 50.0f }, 1000.0f, 1u }; /* engulfs the frustum */
    TEST_ASSERT_TRUE(jce_light_cluster_build(lc, &L, 1));
    JceLightClusterResult r = jce_light_cluster_get_result(lc);
    TEST_ASSERT_TRUE(r.total_assignments > 50u);
    jce_light_cluster_destroy(lc);
}

static void test_overflow_capped(void)
{
    JceLightCluster *lc = make_cluster(2, 2, 2, 1);          /* max_per_cell = 1 */
    JceLightProxy L[4];
    for (int i = 0; i < 4; ++i) {
        L[i].position_ws = (jce_vec3){ 0.0f, 0.0f, 50.0f };
        L[i].radius = 1000.0f;
        L[i].user_id = (uint32_t)i;
    }
    TEST_ASSERT_TRUE(jce_light_cluster_build(lc, L, 4));
    JceLightClusterResult r = jce_light_cluster_get_result(lc);
    uint32_t cells = r.cells_x * r.cells_y * r.slices_z;
    for (uint32_t c = 0; c < cells; ++c)
        TEST_ASSERT_TRUE(r.cell_counts[c] <= 1u);            /* never exceeds cap */
    TEST_ASSERT_TRUE(r.overflow_cells > 0u);
    jce_light_cluster_destroy(lc);
}

static void test_determinism(void)
{
    JceLightCluster *a = make_cluster(4, 4, 8, 8);
    JceLightCluster *b = make_cluster(4, 4, 8, 8);
    JceLightProxy L = { { 1.0f, 2.0f, 20.0f }, 5.0f, 9u };
    TEST_ASSERT_TRUE(jce_light_cluster_build(a, &L, 1));
    TEST_ASSERT_TRUE(jce_light_cluster_build(b, &L, 1));
    JceLightClusterResult ra = jce_light_cluster_get_result(a);
    JceLightClusterResult rb = jce_light_cluster_get_result(b);
    uint32_t cells = ra.cells_x * ra.cells_y * ra.slices_z;
    for (uint32_t c = 0; c < cells; ++c)
        TEST_ASSERT_EQUAL_UINT32(ra.cell_counts[c], rb.cell_counts[c]);
    jce_light_cluster_destroy(a);
    jce_light_cluster_destroy(b);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_validation);
    RUN_TEST(test_slice_monotonic);
    RUN_TEST(test_build_guards);
    RUN_TEST(test_light_in_front_assigned);
    RUN_TEST(test_light_behind_not_assigned);
    RUN_TEST(test_huge_light_covers_many_cells);
    RUN_TEST(test_overflow_capped);
    RUN_TEST(test_determinism);
    return UNITY_END();
}
