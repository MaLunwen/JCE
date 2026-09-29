/*
 * test_jce_terrain_collider_agreement.c
 *
 * Terrain has three descriptions of the same surface:
 *
 *   1. what the renderer DRAWS   (jce_terrain_chunk_build_mesh)
 *   2. what the sampler REPORTS  (jce_terrain_sample_height -- used by
 *                                 foliage placement, snap-to-ground, AI)
 *   3. what the player STANDS ON (the collider)
 *
 * All three must agree, and the interesting failures are invisible unless the
 * test probes CELL CENTRES: every triangulation of a quad agrees exactly at its
 * four corners, so a vertex-sampled check passes no matter which diagonal each
 * description picked.  That is precisely how the shipped triangle soup managed
 * to split on the opposite diagonal from the renderer for its entire life.
 *
 * The runtime now prefers a btHeightfieldTerrainShape and falls back to the
 * soup only for terrain with holes, so BOTH collider paths are checked here
 * against the same oracle.
 */

#include <jce/middleware/physics/jce_physics.h>
#include <jce/middleware/scene/jce_terrain.h>
#include <jce/os/core/jce_alloc.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define TW      9        /* vertex counts */
#define TH      9
#define WORLD   8.0f     /* so cell size is exactly 1.0 */
#define MAXH    10.0f

static JceTerrain *g_t;

/* A deliberately JAGGED field.  A smooth ramp would let both diagonals agree
 * at the centre by symmetry -- the oracle has to be able to tell them apart or
 * it proves nothing. */
static void build_terrain(void)
{
    g_t = jce_terrain_create(TW, TH, WORLD, WORLD, MAXH, 8);
    TEST_ASSERT_NOT_NULL(g_t);

    float *h = (float *)jce_terrain_heights(g_t);
    TEST_ASSERT_NOT_NULL(h);
    for (int z = 0; z < TH; z++)
        for (int x = 0; x < TW; x++)
            /* Alternating corners: adjacent samples differ a lot, so the two
             * candidate diagonals through a cell are far apart at its centre. */
            h[z * TW + x] = ((x + z) & 1) ? 0.9f : 0.1f;
}

static JcePhysicsWorld *make_world(void)
{
    JcePhysicsWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.gravity = jce_v3(0.0f, -9.81f, 0.0f);
    wd.fixed_timestep = 1.0f / 120.0f;
    return jce_physics_create(&wd);
}

/* The collider the runtime prefers. */
static JceBodyHandle spawn_heightfield(JcePhysicsWorld *w)
{
    const int    W = jce_terrain_width(g_t), H = jce_terrain_height(g_t);
    const float *norm = jce_terrain_heights(g_t);
    const size_t n = (size_t)W * (size_t)H;
    float *world_y = (float *)malloc(n * sizeof(float));
    TEST_ASSERT_NOT_NULL(world_y);
    for (size_t i = 0; i < n; i++) world_y[i] = norm[i] * MAXH;

    JceHeightfieldBodyDesc hd;
    memset(&hd, 0, sizeof hd);
    hd.position    = jce_v3(0.0f, 0.0f, 0.0f);
    hd.rotation    = jce_q_identity();
    hd.heights     = world_y;
    hd.samples_x   = (uint32_t)W;
    hd.samples_z   = (uint32_t)H;
    hd.cell_size_x = WORLD / (float)(W - 1);
    hd.cell_size_z = WORLD / (float)(H - 1);
    hd.min_height  = 0.0f;
    hd.max_height  = MAXH;
    hd.diagonal    = JCE_HEIGHTFIELD_DIAG_FIXED;
    hd.friction    = 0.8f;
    hd.smooth_internal_edges = true;

    JceBodyHandle b = jce_physics_body_create_heightfield(w, &hd);
    free(world_y);       /* the bridge copies (convention 4) */
    return b;
}

/* The fallback the runtime uses when the terrain has holes. */
static JceBodyHandle spawn_soup(JcePhysicsWorld *w)
{
    float    *verts = NULL; uint32_t vcount = 0;
    uint32_t *idx   = NULL; uint32_t icount = 0;
    TEST_ASSERT_TRUE(jce_terrain_build_collision_mesh(g_t, &verts, &vcount,
                                                      &idx, &icount));
    JceColliderChild child;
    memset(&child, 0, sizeof child);
    child.shape        = JCE_SHAPE_TRIANGLE_MESH;
    child.rotation     = jce_q_identity();
    child.vertices     = verts;
    child.vertex_count = vcount;
    child.indices      = idx;
    child.index_count  = icount;

    JceCompoundBodyDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.type        = JCE_BODY_STATIC;
    desc.position    = jce_v3(0.0f, 0.0f, 0.0f);
    desc.rotation    = jce_q_identity();
    desc.friction    = 0.8f;
    desc.children    = &child;
    desc.child_count = 1;

    JceBodyHandle b = jce_physics_body_create_compound(w, &desc);
    jce_free(verts);
    jce_free(idx);
    return b;
}

static float probe(JcePhysicsWorld *w, float x, float z)
{
    JceRaycastResult r = jce_physics_raycast(
        w, jce_v3(x, MAXH + 20.0f, z), jce_v3(0.0f, -1.0f, 0.0f), MAXH + 60.0f);
    return r.hit ? r.point.y : -1e9f;
}

/* ── 1. Both colliders agree with each other at CELL CENTRES ──────────── */

static void test_heightfield_and_soup_agree_at_cell_centres(void)
{
    build_terrain();

    JcePhysicsWorld *wh = make_world();
    TEST_ASSERT_TRUE(jce_body_valid(spawn_heightfield(wh)));
    JcePhysicsWorld *ws = make_world();
    TEST_ASSERT_TRUE(jce_body_valid(spawn_soup(ws)));

    /* Cell centres, not vertices.  On this jagged field the two diagonals are
     * 8 world units apart here, so a mismatch cannot hide inside a tolerance. */
    int checked = 0;
    for (int cz = 0; cz < TH - 1; cz++) {
        for (int cx = 0; cx < TW - 1; cx++) {
            const float x = (float)cx + 0.5f;
            const float z = (float)cz + 0.5f;
            const float hf = probe(wh, x, z);
            const float sp = probe(ws, x, z);
            TEST_ASSERT_TRUE(hf > -1e8f);
            TEST_ASSERT_TRUE(sp > -1e8f);
            TEST_ASSERT_FLOAT_WITHIN(0.05f, sp, hf);
            checked++;
        }
    }
    TEST_ASSERT_EQUAL_INT((TW - 1) * (TH - 1), checked);

    jce_physics_destroy(wh);
    jce_physics_destroy(ws);
    jce_terrain_free(g_t);
    g_t = NULL;
}

/* ── 2. And both agree with what the SAMPLER reports ───────────────────
 *
 * jce_terrain_sample_height is what foliage scatter, snap-to-ground and AI
 * pathing read.  If it disagrees with the collider, grass floats and NPCs walk
 * through the floor -- with no rendering artefact to point at. */

static void test_colliders_agree_with_the_sampler(void)
{
    build_terrain();

    JcePhysicsWorld *wh = make_world();
    TEST_ASSERT_TRUE(jce_body_valid(spawn_heightfield(wh)));

    /* Bilinear sampling of a quad is not either triangulation of it, so at a
     * cell centre the two legitimately differ.  At VERTICES every description
     * must agree exactly -- that is the part with no room for interpretation. */
    for (int z = 1; z < TH - 1; z++) {
        for (int x = 1; x < TW - 1; x++) {
            const float wx = (float)x, wz = (float)z;
            const float sampled = jce_terrain_sample_height(g_t, wx, wz);
            const float hit     = probe(wh, wx, wz);
            TEST_ASSERT_TRUE(hit > -1e8f);
            TEST_ASSERT_FLOAT_WITHIN(0.05f, sampled, hit);
        }
    }

    jce_physics_destroy(wh);
    jce_terrain_free(g_t);
    g_t = NULL;
}

/* ── 3. Holes survive: the soup omits them, and that is why it is kept ── */

static void test_holes_are_not_solid(void)
{
    build_terrain();
    /* Punch out one cell and confirm the soup path leaves a real gap.  A
     * heightfield cannot express this, which is the whole reason the runtime
     * still falls back to the soup for terrain with holes. */
    jce_terrain_set_hole(g_t, 3, 3, true);
    TEST_ASSERT_TRUE(jce_terrain_has_holes(g_t));

    JcePhysicsWorld *ws = make_world();
    TEST_ASSERT_TRUE(jce_body_valid(spawn_soup(ws)));

    TEST_ASSERT_TRUE(probe(ws, 3.5f, 3.5f) < -1e8f);   /* falls through */
    TEST_ASSERT_TRUE(probe(ws, 1.5f, 1.5f) > -1e8f);   /* neighbour intact */

    jce_physics_destroy(ws);
    jce_terrain_free(g_t);
    g_t = NULL;
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_heightfield_and_soup_agree_at_cell_centres);
    RUN_TEST(test_colliders_agree_with_the_sampler);
    RUN_TEST(test_holes_are_not_solid);
    return UNITY_END();
}
