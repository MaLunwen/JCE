/*
 * test_jce_fracture.c — Self-test for the Voronoi box-shatter geometry core.
 *
 * Pure geometry: no Bullet, no flecs.  Validates the load-bearing properties
 * of jce_fracture_box:
 *   1. single seed -> 1 cell == whole box (8 verts, volume == box volume)
 *   2. N seeds     -> N cells; each convex + non-empty (vcount >= 4); vol > 0
 *   3. VOLUME CONSERVATION: sum of cell volumes ≈ box volume (cells tile box)
 *   4. determinism: same seeds -> identical cells (memcmp)
 *   5. each cell's verts satisfy the cell's half-spaces; each cell contains
 *      its seed
 *   6. robustness: n_seeds = 0 / 1 safe; NULL-safe free
 *
 * Linked against jce_core + jce_physics: jce_fracture.c lives in the physics
 * middleware layer (next to jce_collider_cook), but its only deps are math.h
 * + the engine allocator + its own header (no Bullet symbols are pulled in by
 * the fracture core itself).
 */

#include "unity.h"

#include <jce/middleware/physics/jce_fracture.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* The box used by most tests: a 2 x 3 x 4 box centred away from the origin so
 * a sign bug shows up. */
static const float BOX_MIN[3] = { -1.0f, -1.5f, -2.0f };
static const float BOX_MAX[3] = {  1.0f,  1.5f,  2.0f };
static const float BOX_VOLUME = 2.0f * 3.0f * 4.0f;   /* = 24 */

/* ── helpers ────────────────────────────────────────────────────────────── */

/* Distance from p to the nearest of the seed array; index of nearest in *idx. */
static float nearest_seed(const float p[3], const float *seeds, int n, int *idx)
{
    float best = 1e30f;
    int   bi   = -1;
    for (int i = 0; i < n; ++i) {
        float dx = p[0] - seeds[i * 3 + 0];
        float dy = p[1] - seeds[i * 3 + 1];
        float dz = p[2] - seeds[i * 3 + 2];
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best) { best = d2; bi = i; }
    }
    if (idx) *idx = bi;
    return best;
}

/* ── tests ──────────────────────────────────────────────────────────────── */

static void test_single_seed_is_whole_box(void)
{
    float seed[3] = { 0.0f, 0.0f, 0.0f };
    JceFractureResult r;
    int n = jce_fracture_box(BOX_MIN, BOX_MAX, seed, 1, &r);

    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(1, r.cell_count);
    TEST_ASSERT_NOT_NULL(r.cells);
    /* A box clipped by nothing has its 8 corners. */
    TEST_ASSERT_EQUAL_INT(8, r.cells[0].vcount);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, BOX_VOLUME, r.cells[0].volume);

    jce_fracture_free(&r);
}

static void test_zero_seeds_is_whole_box(void)
{
    /* n_seeds <= 1 -> single whole-box cell, even with NULL seeds. */
    JceFractureResult r;
    int n = jce_fracture_box(BOX_MIN, BOX_MAX, NULL, 0, &r);

    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(1, r.cell_count);
    TEST_ASSERT_EQUAL_INT(8, r.cells[0].vcount);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, BOX_VOLUME, r.cells[0].volume);

    jce_fracture_free(&r);
}

static void test_n_cells_convex_and_nonempty(void)
{
    enum { N = 8 };
    float seeds[N * 3];
    int got = jce_fracture_scatter_seeds(BOX_MIN, BOX_MAX, N, 12345u, seeds);
    TEST_ASSERT_EQUAL_INT(N, got);

    JceFractureResult r;
    int n = jce_fracture_box(BOX_MIN, BOX_MAX, seeds, N, &r);
    TEST_ASSERT_EQUAL_INT(N, n);
    TEST_ASSERT_EQUAL_INT(N, r.cell_count);

    for (int i = 0; i < r.cell_count; ++i) {
        /* convex + non-empty: a 3D convex cell needs >= 4 vertices. */
        TEST_ASSERT_TRUE_MESSAGE(r.cells[i].vcount >= 4, "cell has < 4 verts");
        TEST_ASSERT_TRUE_MESSAGE(r.cells[i].volume > 0.0f, "cell volume not > 0");
    }

    jce_fracture_free(&r);
}

static void test_volume_conservation(void)
{
    /* THE load-bearing correctness check: the cells tile the box, so the sum
     * of cell volumes equals the box volume. */
    enum { N = 12 };
    float seeds[N * 3];
    jce_fracture_scatter_seeds(BOX_MIN, BOX_MAX, N, 0xC0FFEEu, seeds);

    JceFractureResult r;
    int n = jce_fracture_box(BOX_MIN, BOX_MAX, seeds, N, &r);
    TEST_ASSERT_EQUAL_INT(N, n);

    float sum = 0.0f;
    for (int i = 0; i < r.cell_count; ++i)
        sum += r.cells[i].volume;

    /* Tolerance: epsilon merging + float output can lose a little; 0.5% of
     * the box volume is comfortable headroom while still catching real gaps. */
    TEST_ASSERT_FLOAT_WITHIN(BOX_VOLUME * 0.005f, BOX_VOLUME, sum);

    jce_fracture_free(&r);
}

static void test_determinism(void)
{
    enum { N = 10 };
    float seeds[N * 3];
    jce_fracture_scatter_seeds(BOX_MIN, BOX_MAX, N, 777u, seeds);

    JceFractureResult a, b;
    int na = jce_fracture_box(BOX_MIN, BOX_MAX, seeds, N, &a);
    int nb = jce_fracture_box(BOX_MIN, BOX_MAX, seeds, N, &b);
    TEST_ASSERT_EQUAL_INT(na, nb);
    TEST_ASSERT_EQUAL_INT(a.cell_count, b.cell_count);

    for (int i = 0; i < a.cell_count; ++i) {
        TEST_ASSERT_EQUAL_INT(a.cells[i].vcount, b.cells[i].vcount);
        /* Byte-for-byte identical cells: vcount, volume, centroid, and the
         * used vertex slots must match exactly. */
        TEST_ASSERT_EQUAL_FLOAT(a.cells[i].volume, b.cells[i].volume);
        TEST_ASSERT_EQUAL_MEMORY(a.cells[i].centroid, b.cells[i].centroid,
                                 sizeof(a.cells[i].centroid));
        TEST_ASSERT_EQUAL_MEMORY(a.cells[i].verts, b.cells[i].verts,
                                 (size_t)a.cells[i].vcount * 3 * sizeof(float));
    }

    /* Determinism of the scatterer itself. */
    float seeds2[N * 3];
    jce_fracture_scatter_seeds(BOX_MIN, BOX_MAX, N, 777u, seeds2);
    TEST_ASSERT_EQUAL_MEMORY(seeds, seeds2, sizeof(seeds));

    jce_fracture_free(&a);
    jce_fracture_free(&b);
}

static void test_cells_contain_seed_and_are_valid_regions(void)
{
    enum { N = 8 };
    float seeds[N * 3];
    jce_fracture_scatter_seeds(BOX_MIN, BOX_MAX, N, 42u, seeds);

    JceFractureResult r;
    jce_fracture_box(BOX_MIN, BOX_MAX, seeds, N, &r);
    TEST_ASSERT_EQUAL_INT(N, r.cell_count);

    for (int i = 0; i < r.cell_count; ++i) {
        const JceFractureCell *cell = &r.cells[i];

        /* (a) Every vertex lies inside the box (within slack). */
        for (int v = 0; v < cell->vcount; ++v) {
            for (int ax = 0; ax < 3; ++ax) {
                TEST_ASSERT_TRUE(cell->verts[v][ax] >= BOX_MIN[ax] - 1e-4f);
                TEST_ASSERT_TRUE(cell->verts[v][ax] <= BOX_MAX[ax] + 1e-4f);
            }
        }

        /* (b) Each vertex of cell i is closest to seed i (Voronoi property),
         * within slack — a vertex sits equidistant from the bisecting seeds,
         * so allow a small tolerance on the squared-distance compare. */
        for (int v = 0; v < cell->vcount; ++v) {
            float p[3] = { cell->verts[v][0], cell->verts[v][1], cell->verts[v][2] };
            /* distance to own seed */
            float dx = p[0] - seeds[i * 3 + 0];
            float dy = p[1] - seeds[i * 3 + 1];
            float dz = p[2] - seeds[i * 3 + 2];
            float dv_i = dx * dx + dy * dy + dz * dz;
            /* nearest squared dist must be within EPS of own-seed squared dist */
            float dnear = nearest_seed(p, seeds, N, NULL);
            TEST_ASSERT_TRUE(dv_i <= dnear + 1e-3f);
        }

        /* (c) The cell centroid is closest to its own seed (the centroid is
         * strictly interior, so the Voronoi membership is unambiguous). */
        int cidx;
        nearest_seed(cell->centroid, seeds, N, &cidx);
        TEST_ASSERT_EQUAL_INT(i, cidx);
    }

    jce_fracture_free(&r);
}

static void test_null_safe_free(void)
{
    /* free of NULL */
    jce_fracture_free(NULL);

    /* free of a zeroed result */
    JceFractureResult z;
    memset(&z, 0, sizeof z);
    jce_fracture_free(&z);
    TEST_ASSERT_NULL(z.cells);
    TEST_ASSERT_EQUAL_INT(0, z.cell_count);

    /* double-free is safe (idempotent) */
    float seed[3] = { 0, 0, 0 };
    JceFractureResult r;
    jce_fracture_box(BOX_MIN, BOX_MAX, seed, 1, &r);
    jce_fracture_free(&r);
    jce_fracture_free(&r);
    TEST_ASSERT_NULL(r.cells);
}

static void test_degenerate_box_rejected(void)
{
    /* An inverted / zero-extent box returns 0 and leaves out zeroed. */
    float bad_max[3] = { -1.0f, 1.5f, 2.0f };   /* x: max < min */
    JceFractureResult r;
    int n = jce_fracture_box(BOX_MIN, bad_max, NULL, 0, &r);
    TEST_ASSERT_EQUAL_INT(0, n);
    TEST_ASSERT_NULL(r.cells);
    jce_fracture_free(&r);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_single_seed_is_whole_box);
    RUN_TEST(test_zero_seeds_is_whole_box);
    RUN_TEST(test_n_cells_convex_and_nonempty);
    RUN_TEST(test_volume_conservation);
    RUN_TEST(test_determinism);
    RUN_TEST(test_cells_contain_seed_and_are_valid_regions);
    RUN_TEST(test_null_safe_free);
    RUN_TEST(test_degenerate_box_rejected);
    return UNITY_END();
}
