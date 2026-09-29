/*
 * test_jce_navmesh_astar_scratch.c — rank-6 verification for the grid A*
 * (jce_navmesh.c) persistent-scratch + generation-stamp rewrite.
 *
 * Asserts:
 *   1. A wall forces a correct detour: path reaches the goal, every waypoint is
 *      on a walkable cell, and the path goes AROUND the wall (not through it).
 *   2. Determinism across queries: query #2..#N produce byte-identical paths to
 *      query #1 (the generation-token reset must not corrupt state).
 *   3. Zero per-query heap churn: after a warm-up call (which grows the scratch
 *      once), 2000 further find_path calls allocate ~0 (the old code did 4-5
 *      CALLOC + a full-grid memset PER query).
 *   4. No-path case returns 0; a straight open run returns a short path.
 */

#include "unity.h"

#include <jce/middleware/ai/jce_navmesh.h>
#include <jce/os/core/jce_allocator.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define GX 64
#define GZ 64

/* Build a {"result":{...}} navmesh JSON with a caller-filled walkable bitmap. */
static JceNavMesh *build_grid(const char *bits /* GX*GZ chars */)
{
    static char json[GX * GZ + 256];
    int off = snprintf(json, sizeof json,
        "{\"result\":{\"gx\":%d,\"gz\":%d,\"cell\":1.0,"
        "\"originX\":0,\"originZ\":0,\"bits\":\"", GX, GZ);
    memcpy(json + off, bits, GX * GZ);
    off += GX * GZ;
    off += snprintf(json + off, sizeof(json) - off, "\"}}");
    return jce_navmesh_load_text(json, (size_t)off);
}

/* world centre of cell (cx,cz): origin 0, cell 1 -> (cx+0.5, cz+0.5). */
static void fill_all_walkable(char *bits) { memset(bits, '1', GX * GZ); }

static int waypoint_cell(float w) { return (int)(w); }  /* origin 0, cell 1 */

static void test_wall_forces_detour_and_is_deterministic(void)
{
    char bits[GX * GZ];
    fill_all_walkable(bits);
    /* Vertical wall at x=32 for z in [0,60]; gap at z in [61,63] to route around. */
    for (int z = 0; z < 61; ++z) bits[z * GX + 32] = '0';

    JceNavMesh *nm = build_grid(bits);
    TEST_ASSERT_NOT_NULL(nm);

    float p1[GX * GZ * 2];
    int n1 = jce_navmesh_find_path(nm, 5.5f, 32.5f, 60.5f, 32.5f, p1, GX * GZ);
    TEST_ASSERT_TRUE_MESSAGE(n1 > 0, "no detour path found around the wall");

    /* Last point is the exact goal; every waypoint sits on a walkable cell and
     * never on the wall column (x cell 32, z<61). */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.5f, p1[(n1 - 1) * 2 + 0]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 32.5f, p1[(n1 - 1) * 2 + 1]);
    for (int i = 0; i < n1; ++i) {
        int cx = waypoint_cell(p1[i * 2 + 0]);
        int cz = waypoint_cell(p1[i * 2 + 1]);
        TEST_ASSERT_TRUE_MESSAGE(cx >= 0 && cx < GX && cz >= 0 && cz < GZ,
                                 "waypoint off-grid");
        bool on_wall = (cx == 32 && cz < 61);
        TEST_ASSERT_FALSE_MESSAGE(on_wall, "waypoint sits inside the wall");
    }

    /* Determinism: re-running the SAME query many times must reproduce the path
     * byte-for-byte (the per-query token reset must not leak state). */
    for (int q = 0; q < 50; ++q) {
        float p2[GX * GZ * 2];
        int n2 = jce_navmesh_find_path(nm, 5.5f, 32.5f, 60.5f, 32.5f, p2, GX * GZ);
        TEST_ASSERT_EQUAL_INT_MESSAGE(n1, n2, "path length changed across queries");
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(p1, p2, (size_t)n1 * 2 * sizeof(float),
                                         "path diverged across queries");
    }

    jce_navmesh_free(nm);
}

static void test_zero_per_query_allocations(void)
{
    char bits[GX * GZ];
    fill_all_walkable(bits);
    JceNavMesh *nm = build_grid(bits);
    TEST_ASSERT_NOT_NULL(nm);

    float p[GX * GZ * 2];
    /* Warm-up: first call grows the persistent scratch once. */
    (void)jce_navmesh_find_path(nm, 1.5f, 1.5f, 62.5f, 62.5f, p, GX * GZ);

    uint64_t a0 = 0, b0 = 0;
    jce_alloc_frame_delta(&a0, &b0);            /* reset the delta baseline */
    const int N = 2000;
    for (int i = 0; i < N; ++i)
        (void)jce_navmesh_find_path(nm, 1.5f, 1.5f, 62.5f, 62.5f, p, GX * GZ);
    uint64_t a = 0, b = 0;
    jce_alloc_frame_delta(&a, &b);              /* allocs over the N queries */

    /* The old code allocated 4-5 buffers per query (~10000 over N). The scratch
     * rewrite should add essentially nothing after the warm-up grow. */
    TEST_ASSERT_TRUE_MESSAGE(a < (uint64_t)N,
        "per-query heap churn did not collapse (scratch not persistent)");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0, a,
        "expected exactly zero allocations across 2000 warm find_path calls");

    jce_navmesh_free(nm);
}

static void test_no_path_and_straight(void)
{
    /* Goal fully walled off: a full vertical wall at x=32 (all z) => no route. */
    char bits[GX * GZ];
    fill_all_walkable(bits);
    for (int z = 0; z < GZ; ++z) bits[z * GX + 32] = '0';
    JceNavMesh *nm = build_grid(bits);
    TEST_ASSERT_NOT_NULL(nm);
    float p[GX * GZ * 2];
    int n = jce_navmesh_find_path(nm, 5.5f, 5.5f, 60.5f, 60.5f, p, GX * GZ);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, n, "found a path through a full wall");
    jce_navmesh_free(nm);

    /* Open grid: a straight-ish short run. */
    char open[GX * GZ];
    fill_all_walkable(open);
    JceNavMesh *nm2 = build_grid(open);
    TEST_ASSERT_NOT_NULL(nm2);
    int n2 = jce_navmesh_find_path(nm2, 1.5f, 1.5f, 10.5f, 1.5f, p, GX * GZ);
    TEST_ASSERT_TRUE_MESSAGE(n2 > 0, "no path on open grid");
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.5f, p[(n2 - 1) * 2 + 0]);
    jce_navmesh_free(nm2);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_wall_forces_detour_and_is_deterministic);
    RUN_TEST(test_zero_per_query_allocations);
    RUN_TEST(test_no_path_and_straight);
    return UNITY_END();
}
