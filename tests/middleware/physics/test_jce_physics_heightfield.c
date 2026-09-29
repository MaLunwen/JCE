/*
 * test_jce_physics_heightfield.c
 *
 * Terrain collision as a dedicated heightfield primitive.
 *
 * Before this there was NO heightfield shape in the engine at all -- a search
 * for btHeightfieldTerrainShape found only Recast's unrelated internal use --
 * so terrain collided as generic triangle soup: the whole grid expanded to
 * W*H vertices and (W-1)*(H-1)*2 triangles in one allocation, with no chunking
 * and no LOD.  Streamed/procedural terrain got no collision whatsoever,
 * because the expander required a resident monolithic height array.
 *
 * The tests below deliberately go past "a sphere lands on it".  Three of the
 * five Bullet conventions this API encapsulates CANNOT be caught by a
 * drop-and-settle test:
 *
 *   - the AABB centring offset shows up as a constant vertical error, which
 *     reads like a bias-tuning problem;
 *   - the quad diagonal choice only matters at CELL CENTRES, because every
 *     triangulation agrees exactly at the vertices a naive test samples;
 *   - internal-edge ghost contacts only appear when something SLIDES.
 */

#include <jce/middleware/physics/jce_physics.h>

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define GRID 9
#define CELL 1.0f

static float g_flat[GRID * GRID];
static float g_ramp[GRID * GRID];

static void build_fields(void)
{
    for (int z = 0; z < GRID; z++) {
        for (int x = 0; x < GRID; x++) {
            g_flat[z * GRID + x] = 0.0f;
            /* A plane tilted in X: height is exactly known everywhere, so the
             * expected contact height at a cell CENTRE is unambiguous for any
             * triangulation -- which is what makes the centring test sharp. */
            g_ramp[z * GRID + x] = (float)x * 0.25f;
        }
    }
}

static JceHeightfieldBodyDesc base_desc(const float *heights,
                                        float min_h, float max_h)
{
    JceHeightfieldBodyDesc d;
    memset(&d, 0, sizeof d);
    d.position    = jce_v3(0.0f, 0.0f, 0.0f);
    d.rotation    = jce_q_identity();
    d.heights     = heights;
    d.samples_x   = GRID;
    d.samples_z   = GRID;
    d.cell_size_x = CELL;
    d.cell_size_z = CELL;
    d.min_height  = min_h;
    d.max_height  = max_h;
    d.diagonal    = JCE_HEIGHTFIELD_DIAG_FIXED;
    d.friction    = 0.5f;
    return d;
}

static JcePhysicsWorld *make_world(void)
{
    JcePhysicsWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.gravity = jce_v3(0.0f, -9.81f, 0.0f);
    wd.fixed_timestep = 1.0f / 120.0f;
    return jce_physics_create(&wd);
}

/* ── 1. It creates at all ──────────────────────────────────────────── */

static void test_create_succeeds(void)
{
    build_fields();
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    JceHeightfieldBodyDesc d = base_desc(g_flat, -1.0f, 1.0f);
    JceBodyHandle b = jce_physics_body_create_heightfield(w, &d);
    TEST_ASSERT_NOT_EQUAL_UINT32(JCE_BODY_INVALID.idx, b.idx);

    jce_physics_destroy(w);
}

/* ── 2. Invalid input is rejected, not silently accepted ───────────── */

static void test_invalid_input_rejected(void)
{
    build_fields();
    JcePhysicsWorld *w = make_world();

    /* Too few samples. */
    JceHeightfieldBodyDesc d = base_desc(g_flat, -1.0f, 1.0f);
    d.samples_x = 1;
    TEST_ASSERT_EQUAL_UINT32(JCE_BODY_INVALID.idx,
                             jce_physics_body_create_heightfield(w, &d).idx);

    /* Non-positive cell size. */
    d = base_desc(g_flat, -1.0f, 1.0f);
    d.cell_size_x = 0.0f;
    TEST_ASSERT_EQUAL_UINT32(JCE_BODY_INVALID.idx,
                             jce_physics_body_create_heightfield(w, &d).idx);

    /* Bounds that do not contain the data would silently corrupt the AABB. */
    d = base_desc(g_ramp, 0.0f, 0.5f);   /* ramp reaches 2.0 */
    TEST_ASSERT_EQUAL_UINT32(JCE_BODY_INVALID.idx,
                             jce_physics_body_create_heightfield(w, &d).idx);

    /* Inverted bounds. */
    d = base_desc(g_flat, 1.0f, -1.0f);
    TEST_ASSERT_EQUAL_UINT32(JCE_BODY_INVALID.idx,
                             jce_physics_body_create_heightfield(w, &d).idx);

    /* NULL samples. */
    d = base_desc(NULL, -1.0f, 1.0f);
    TEST_ASSERT_EQUAL_UINT32(JCE_BODY_INVALID.idx,
                             jce_physics_body_create_heightfield(w, &d).idx);

    jce_physics_destroy(w);
}

/* ── 3. THE CENTRING CONVENTION ────────────────────────────────────── */

static float drop_and_settle(JcePhysicsWorld *w, float x, float z, float from_y)
{
    JceBodyDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.type         = JCE_BODY_DYNAMIC;
    bd.shape        = JCE_SHAPE_SPHERE;
    bd.position     = jce_v3(x, from_y, z);
    bd.rotation     = jce_q_identity();
    bd.half_extents = jce_v3(0.25f, 0.0f, 0.0f);
    bd.mass         = 1.0f;
    bd.friction     = 0.5f;

    JceBodyHandle ball = jce_physics_body_create(w, &bd);
    for (int i = 0; i < 400; i++) jce_physics_step(w, 1.0f / 120.0f);

    jce_vec3 p; jce_quat r;
    jce_physics_body_get_transform(w, ball, &p, &r);
    return p.y;
}

static void test_surface_is_where_the_data_says(void)
{
    build_fields();
    JcePhysicsWorld *w = make_world();

    /* Flat field at height 0, declared range [-1, +1].  Bullet centres the
     * shape on the midpoint of its own AABB, so a naive implementation puts
     * this surface at y = -0.5 * (min+max) = 0 by luck.  The ASYMMETRIC range
     * below is what actually exposes the bug. */
    JceHeightfieldBodyDesc d = base_desc(g_flat, -3.0f, 1.0f);
    TEST_ASSERT_NOT_EQUAL_UINT32(
        JCE_BODY_INVALID.idx, jce_physics_body_create_heightfield(w, &d).idx);

    float rest = drop_and_settle(w, 4.0f, 4.0f, 6.0f);
    /* Sphere radius 0.25 resting on a surface at y=0. */
    TEST_ASSERT_FLOAT_WITHIN(0.12f, 0.25f, rest);

    jce_physics_destroy(w);
}

/* ── 4. Position places the MIN corner, not the centre ─────────────── */

static void test_position_is_min_corner(void)
{
    build_fields();
    JcePhysicsWorld *w = make_world();

    JceHeightfieldBodyDesc d = base_desc(g_flat, -1.0f, 1.0f);
    d.position = jce_v3(0.0f, 5.0f, 0.0f);   /* lift the whole field */
    TEST_ASSERT_NOT_EQUAL_UINT32(
        JCE_BODY_INVALID.idx, jce_physics_body_create_heightfield(w, &d).idx);

    /* The field spans x,z in [0, 8] because position is its MIN corner.
     * Drop at the middle of that span. */
    float rest = drop_and_settle(w, 4.0f, 4.0f, 11.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.12f, 5.25f, rest);

    jce_physics_destroy(w);
}

/* ── 5. A slope is actually sloped, sampled at CELL CENTRES ────────── */

static void test_slope_height_at_cell_centres(void)
{
    build_fields();
    JcePhysicsWorld *w = make_world();

    JceHeightfieldBodyDesc d = base_desc(g_ramp, 0.0f, 2.0f);
    TEST_ASSERT_NOT_EQUAL_UINT32(
        JCE_BODY_INVALID.idx, jce_physics_body_create_heightfield(w, &d).idx);

    /* Raycast rather than a dropped sphere: on a slope a sphere ROLLS, so a
     * settle test measures where it ended up, not the surface under the point
     * asked about.  A downward ray measures the geometry directly.
     *
     * Sampled at CELL CENTRES on purpose -- every triangulation agrees exactly
     * at the vertices, so a vertex-sampled oracle cannot detect a quad-diagonal
     * mismatch at all. */
    const float down_len = 20.0f;
    for (int i = 0; i < 7; i++) {
        const float x = 0.5f + (float)i;          /* cell centres 0.5 .. 6.5 */
        JceRaycastResult r = jce_physics_raycast(
            w, jce_v3(x, 10.0f, 4.5f), jce_v3(0.0f, -1.0f, 0.0f), down_len);
        TEST_ASSERT_TRUE(r.hit);
        /* Ramp is exactly 0.25 per unit of X. */
        TEST_ASSERT_FLOAT_WITHIN(0.06f, x * 0.25f, r.point.y);
    }

    jce_physics_destroy(w);
}

/* ── 6. Every diagonal rule produces a usable surface ──────────────── */

static void test_all_diagonal_modes_work(void)
{
    build_fields();
    const JceHeightfieldDiagonal modes[3] = {
        JCE_HEIGHTFIELD_DIAG_FIXED,
        JCE_HEIGHTFIELD_DIAG_DIAMOND,
        JCE_HEIGHTFIELD_DIAG_ZIGZAG,
    };
    for (int i = 0; i < 3; i++) {
        JcePhysicsWorld *w = make_world();
        JceHeightfieldBodyDesc d = base_desc(g_flat, -1.0f, 1.0f);
        d.diagonal = modes[i];
        TEST_ASSERT_NOT_EQUAL_UINT32(
            JCE_BODY_INVALID.idx,
            jce_physics_body_create_heightfield(w, &d).idx);

        float rest = drop_and_settle(w, 4.5f, 4.5f, 6.0f);
        TEST_ASSERT_FLOAT_WITHIN(0.12f, 0.25f, rest);
        jce_physics_destroy(w);
    }
}

/* ── 7. THE ROLL TEST -- ghost internal edges ──────────────────────── */

static void test_sliding_across_cells_is_smooth(void)
{
    build_fields();
    JcePhysicsWorld *w = make_world();

    JceHeightfieldBodyDesc d = base_desc(g_flat, -1.0f, 1.0f);
    d.smooth_internal_edges = true;
    d.friction = 0.0f;
    TEST_ASSERT_NOT_EQUAL_UINT32(
        JCE_BODY_INVALID.idx, jce_physics_body_create_heightfield(w, &d).idx);

    JceBodyDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.type         = JCE_BODY_DYNAMIC;
    bd.shape        = JCE_SHAPE_SPHERE;
    bd.position     = jce_v3(1.0f, 0.30f, 4.0f);
    bd.rotation     = jce_q_identity();
    bd.half_extents = jce_v3(0.25f, 0.0f, 0.0f);
    bd.mass         = 1.0f;
    bd.friction     = 0.0f;
    JceBodyHandle ball = jce_physics_body_create(w, &bd);

    /* Let it settle, then push it across several cell boundaries.  On a FLAT
     * field a body must not gain upward velocity from the shared diagonal of
     * each cell; that upward kick is the classic ghost-collision defect and a
     * drop-and-settle test never sees it. */
    for (int i = 0; i < 120; i++) jce_physics_step(w, 1.0f / 120.0f);
    jce_physics_body_set_velocity(w, ball, jce_v3(3.0f, 0.0f, 0.0f));

    float worst_up = 0.0f;
    for (int i = 0; i < 240; i++) {
        jce_physics_step(w, 1.0f / 120.0f);
        jce_vec3 v = jce_physics_body_get_velocity(w, ball);
        if (v.y > worst_up) worst_up = v.y;
    }

    /* Gravity means the body is always being pulled down; any sustained
     * UPWARD velocity on flat ground came from a ghost edge. */
    TEST_ASSERT_TRUE(worst_up < 1.0f);

    jce_physics_destroy(w);
}

/* ── 8. Creation and destruction do not leak slots ─────────────────── */

static void test_repeated_create_destroy(void)
{
    build_fields();
    JcePhysicsWorld *w = make_world();
    for (int i = 0; i < 64; i++) {
        JceHeightfieldBodyDesc d = base_desc(g_flat, -1.0f, 1.0f);
        JceBodyHandle b = jce_physics_body_create_heightfield(w, &d);
        TEST_ASSERT_NOT_EQUAL_UINT32(JCE_BODY_INVALID.idx, b.idx);
        jce_physics_body_destroy(w, b);
    }
    jce_physics_destroy(w);
}


/* ── 7. WHICH diagonal is FIXED?  Establish it, do not assume it ───────
 *
 * The runtime replaces terrain's triangle-soup collider with this heightfield,
 * so the split has to match what the RENDERER draws or objects rest at a
 * different height than the ground they appear to touch, across half of every
 * cell.  Every triangulation agrees at the vertices, so only a cell-CENTRE
 * probe can tell them apart -- which is why this was measured rather than
 * assumed, and why the measurement found something:
 *
 *   renderer chunk mesh (jce_terrain_chunk_build_mesh): a,c,b + b,c,d
 *                                       -> shared edge b-c = v10-v01
 *   Bullet unflipped (JCE_HEIGHTFIELD_DIAG_FIXED)       = v10-v01   AGREES
 *   legacy collision soup (jce_terrain_build_collision_mesh): v00-v11  DID NOT
 *
 * The soup had disagreed with the visible terrain since it was written.
 *
 * One quad, corners arranged so the two candidate diagonals are 10 units
 * apart at the centre:
 *
 *     (0,1)=10 ------ (1,1)=0
 *        |               |          v00-v11 diagonal  -> centre height  0
 *        |               |          v10-v01 diagonal  -> centre height 10
 *     (0,0)=0  ------ (1,0)=10
 */

static float g_probe[4];

static void test_fixed_diagonal_matches_the_triangle_soup(void)
{
    /* row-major, X fastest: (0,0) (1,0) (0,1) (1,1) */
    g_probe[0] =  0.0f;  g_probe[1] = 10.0f;
    g_probe[2] = 10.0f;  g_probe[3] =  0.0f;

    JcePhysicsWorld *w = make_world();
    JceHeightfieldBodyDesc d;
    memset(&d, 0, sizeof d);
    d.position    = jce_v3(0.0f, 0.0f, 0.0f);
    d.rotation    = jce_q_identity();
    d.heights     = g_probe;
    d.samples_x   = 2u;
    d.samples_z   = 2u;
    d.cell_size_x = 1.0f;
    d.cell_size_z = 1.0f;
    d.min_height  = 0.0f;
    d.max_height  = 10.0f;
    d.diagonal    = JCE_HEIGHTFIELD_DIAG_FIXED;
    d.friction    = 0.5f;

    JceBodyHandle b = jce_physics_body_create_heightfield(w, &d);
    TEST_ASSERT_TRUE(jce_body_valid(b));

    /* Straight down through the cell centre. */
    JceRaycastResult r = jce_physics_raycast(
        w, jce_v3(0.5f, 30.0f, 0.5f), jce_v3(0.0f, -1.0f, 0.0f), 60.0f);
    TEST_ASSERT_TRUE(r.hit);

    /* 10 == the v10-v01 anti-diagonal == what the renderer draws.  If this
     * ever reads 0, Bullet flipped its convention and the terrain collider has
     * silently stopped matching the visible ground. */
    TEST_ASSERT_FLOAT_WITHIN(0.25f, 10.0f, r.point.y);

    jce_physics_destroy(w);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_succeeds);
    RUN_TEST(test_invalid_input_rejected);
    RUN_TEST(test_surface_is_where_the_data_says);
    RUN_TEST(test_position_is_min_corner);
    RUN_TEST(test_slope_height_at_cell_centres);
    RUN_TEST(test_all_diagonal_modes_work);
    RUN_TEST(test_sliding_across_cells_is_smooth);
    RUN_TEST(test_repeated_create_destroy);
    RUN_TEST(test_fixed_diagonal_matches_the_triangle_soup);
    return UNITY_END();
}
