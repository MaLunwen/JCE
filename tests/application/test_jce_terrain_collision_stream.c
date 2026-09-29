/*
 * test_jce_terrain_collision_stream.c
 *
 * Per-tile terrain collision, paged by distance.
 *
 * The gap this closes is not a quality one: tiled and procedural terrain had
 * NO collision whatsoever, because the only collider path built a single body
 * from a monolithic height grid that a streamed terrain does not have.
 *
 * So the assertions are about residency behaving like residency -- bodies
 * appear near the focus, disappear far from it, a stationary focus does NOT
 * churn them, and the cap is respected -- plus the one that proves the whole
 * thing is real: something DROPPED onto a streamed tile actually lands on it.
 */

#include "jce_terrain_collision_stream.h"

#include "resource/jce_terrain_store.h"

#include <jce/os/core/jce_alloc.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define TILES     4u
#define SPAN      5u        /* samples per tile side (4 cells + 1 overlap) */
#define TILE_SIZE 16.0f     /* world units per tile */
#define GROUND    10.0f     /* flat height everywhere, in local Y */

static int g_loads;

/* Flat ground at GROUND: a known, exact answer for a drop test. */
static bool stub_loader(void *ctx, const char *path, JceTerrainTileKey key,
                        float **out_heights, uint32_t *out_w, uint32_t *out_h)
{
    (void)ctx; (void)path;
    if (key.x >= TILES || key.z >= TILES) return false;
    float *h = (float *)jce_malloc(sizeof(float) * SPAN * SPAN);
    if (!h) return false;
    for (uint32_t i = 0; i < SPAN * SPAN; i++) h[i] = GROUND;
    *out_heights = h;
    *out_w = SPAN;
    *out_h = SPAN;
    g_loads++;
    return true;
}

static JcePhysicsWorld *make_world(void)
{
    JcePhysicsWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.gravity = jce_v3(0.0f, -9.81f, 0.0f);
    wd.fixed_timestep = 1.0f / 120.0f;
    return jce_physics_create(&wd);
}

static JceTerrainStore *make_store(JceTerrainHandle *out_h)
{
    JceTerrainStoreConfig cfg;
    jce_terrain_store_config_init(&cfg);
    cfg.max_assets         = 2u;
    cfg.max_resident_tiles = 8u;
    cfg.max_resident_bytes = 1u << 20;
    cfg.loader             = stub_loader;
    cfg.loader_ctx         = NULL;
    JceTerrainStore *st = jce_terrain_store_create(&cfg);
    if (st) *out_h = jce_terrain_store_acquire(st, "terrain/streamed.bin");
    return st;
}

static JceTerrainCollisionStreamDesc base_desc(JcePhysicsWorld *w,
                                               JceTerrainStore *st,
                                               JceTerrainHandle h)
{
    JceTerrainCollisionStreamDesc d;
    memset(&d, 0, sizeof d);
    d.world           = w;
    d.store           = st;
    d.handle          = h;
    d.virtual_path    = "terrain/streamed.bin";
    d.tiles_x         = TILES;
    d.tiles_z         = TILES;
    d.origin          = jce_v3(0.0f, 0.0f, 0.0f);
    d.tile_world_size = TILE_SIZE;
    d.min_height      = 0.0f;
    d.max_height      = 32.0f;
    d.radius          = 20.0f;
    d.friction        = 0.8f;
    d.diagonal        = JCE_HEIGHTFIELD_DIAG_FIXED;
    d.smooth_internal_edges = true;
    d.max_bodies      = 32u;
    return d;
}

/* ── 1. Bodies follow the focus point ──────────────────────────────────  */

static void test_residency_follows_focus(void)
{
    g_loads = 0;
    JcePhysicsWorld *w = make_world();
    JceTerrainHandle h;
    JceTerrainStore *st = make_store(&h);
    JceTerrainCollisionStreamDesc d = base_desc(w, st, h);
    JceTerrainCollisionStream *s = jce_terrain_collision_stream_create(&d);
    TEST_ASSERT_NOT_NULL(s);

    /* Nothing exists until the first update -- creation must not silently
     * spawn the whole world. */
    TEST_ASSERT_EQUAL_UINT32(0u, jce_terrain_collision_stream_active(s));

    /* Focus over tile (0,0). */
    jce_terrain_collision_stream_update(s, jce_v3(8.0f, 0.0f, 8.0f));
    TEST_ASSERT_TRUE(jce_terrain_collision_stream_active(s) > 0u);
    JceTerrainTileKey near0 = { 0u, 0u };
    TEST_ASSERT_TRUE(jce_terrain_collision_stream_has_tile(s, near0));
    /* The far corner must NOT have a body -- otherwise this is not streaming,
     * it is loading everything and calling it streaming. */
    JceTerrainTileKey far0 = { 3u, 3u };
    TEST_ASSERT_FALSE(jce_terrain_collision_stream_has_tile(s, far0));

    /* Move to the far corner: the sets must swap. */
    jce_terrain_collision_stream_update(s, jce_v3(56.0f, 0.0f, 56.0f));
    TEST_ASSERT_TRUE(jce_terrain_collision_stream_has_tile(s, far0));
    TEST_ASSERT_FALSE(jce_terrain_collision_stream_has_tile(s, near0));

    jce_terrain_collision_stream_destroy(s);
    jce_terrain_store_release(st, h);
    jce_terrain_store_destroy(st);
    jce_physics_destroy(w);
}

/* ── 2. A stationary focus does not churn colliders ────────────────────
 *
 * A stream that rebuilt every body each frame would satisfy every other
 * assertion in this file while quietly destroying and recreating physics
 * bodies 60 times a second.  Only the creation counter can tell. */

static void test_steady_focus_does_not_churn(void)
{
    g_loads = 0;
    JcePhysicsWorld *w = make_world();
    JceTerrainHandle h;
    JceTerrainStore *st = make_store(&h);
    JceTerrainCollisionStreamDesc d = base_desc(w, st, h);
    JceTerrainCollisionStream *s = jce_terrain_collision_stream_create(&d);

    const jce_vec3 focus = jce_v3(8.0f, 0.0f, 8.0f);
    jce_terrain_collision_stream_update(s, focus);
    const uint64_t after_first = jce_terrain_collision_stream_created(s);
    const uint32_t live_first  = jce_terrain_collision_stream_active(s);
    TEST_ASSERT_TRUE(after_first > 0u);

    for (int i = 0; i < 30; i++)
        jce_terrain_collision_stream_update(s, focus);

    TEST_ASSERT_EQUAL_UINT64(after_first,
                             jce_terrain_collision_stream_created(s));
    TEST_ASSERT_EQUAL_UINT32(live_first,
                             jce_terrain_collision_stream_active(s));

    jce_terrain_collision_stream_destroy(s);
    jce_terrain_store_release(st, h);
    jce_terrain_store_destroy(st);
    jce_physics_destroy(w);
}

/* ── 3. Hysteresis stops boundary flicker ──────────────────────────────
 *
 * A focus oscillating either side of the radius must not rebuild the same
 * collider over and over. */

static void test_hysteresis_prevents_boundary_flicker(void)
{
    JcePhysicsWorld *w = make_world();
    JceTerrainHandle h;
    JceTerrainStore *st = make_store(&h);
    JceTerrainCollisionStreamDesc d = base_desc(w, st, h);
    d.hysteresis = 0.5f;
    JceTerrainCollisionStream *s = jce_terrain_collision_stream_create(&d);

    /* Oscillate the focus across a tile boundary.  ONE warm-up cycle first:
     * the two positions genuinely differ in which tiles are in range (moving
     * from z=27 to z=28.5 pulls tile (1,2) inside the radius for the first
     * time), so the initial creations are legitimate work, not flicker.  An
     * earlier version of this test asserted from the first update and failed
     * on that legitimate creation -- the premise was wrong, not the code.
     *
     * What "no flicker" actually means is that once BOTH positions have been
     * visited, further oscillation creates nothing. */
    jce_terrain_collision_stream_update(s, jce_v3(8.0f, 0.0f, 27.0f));
    jce_terrain_collision_stream_update(s, jce_v3(8.0f, 0.0f, 28.5f));
    jce_terrain_collision_stream_update(s, jce_v3(8.0f, 0.0f, 27.0f));
    const uint64_t base = jce_terrain_collision_stream_created(s);
    TEST_ASSERT_TRUE(base > 0u);

    for (int i = 0; i < 10; i++) {
        jce_terrain_collision_stream_update(s, jce_v3(8.0f, 0.0f, 28.5f));
        jce_terrain_collision_stream_update(s, jce_v3(8.0f, 0.0f, 27.0f));
    }
    /* Without hysteresis a tile that sits between the two radii would be
     * destroyed on the outward step and rebuilt on every inward one. */
    TEST_ASSERT_EQUAL_UINT64(base, jce_terrain_collision_stream_created(s));

    jce_terrain_collision_stream_destroy(s);
    jce_terrain_store_release(st, h);
    jce_terrain_store_destroy(st);
    jce_physics_destroy(w);
}

/* ── 4. THE POINT: a body actually lands on streamed ground ────────────  */

static void test_object_rests_on_streamed_terrain(void)
{
    JcePhysicsWorld *w = make_world();
    JceTerrainHandle h;
    JceTerrainStore *st = make_store(&h);
    JceTerrainCollisionStreamDesc d = base_desc(w, st, h);
    JceTerrainCollisionStream *s = jce_terrain_collision_stream_create(&d);
    jce_terrain_collision_stream_update(s, jce_v3(8.0f, 0.0f, 8.0f));

    JceBodyDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.type     = JCE_BODY_DYNAMIC;
    bd.shape    = JCE_SHAPE_SPHERE;
    bd.half_extents = jce_v3(0.5f, 0.0f, 0.0f);   /* sphere: radius in .x */
    bd.mass     = 1.0f;
    bd.position = jce_v3(8.0f, 25.0f, 8.0f);
    bd.rotation = jce_q_identity();
    bd.friction = 0.5f;
    JceBodyHandle ball = jce_physics_body_create(w, &bd);
    TEST_ASSERT_TRUE(jce_body_valid(ball));

    for (int i = 0; i < 600; i++) jce_physics_step(w, 1.0f / 120.0f);

    jce_vec3 pos; jce_quat rot;
    jce_physics_body_get_transform(w, ball, &pos, &rot);
    /* Flat ground at 10 plus the sphere radius.  Before this module existed a
     * streamed terrain had no collider at all and the ball fell forever. */
    TEST_ASSERT_FLOAT_WITHIN(0.35f, GROUND + 0.5f, pos.y);

    jce_terrain_collision_stream_destroy(s);
    jce_terrain_store_release(st, h);
    jce_terrain_store_destroy(st);
    jce_physics_destroy(w);
}

/* ── 4b. Tiles cover their FULL extent, right up to the seam ───────────
 *
 * A tile stores one sample past its own cell range, so its cell size is the
 * tile width over (samples - 1).  Dividing by `samples` instead shrinks every
 * tile -- here from 16 units to 12.8 -- leaving a 3.2-unit strip of nothing
 * along the far edge of each one.
 *
 * A drop at a tile CENTRE cannot see that, which is exactly why the first
 * version of this file missed it: on flat ground the shrunken field still
 * answers correctly everywhere it exists.  The gap only shows up at a seam. */

static void test_ground_is_continuous_across_a_seam(void)
{
    JcePhysicsWorld *w = make_world();
    JceTerrainHandle h;
    JceTerrainStore *st = make_store(&h);
    JceTerrainCollisionStreamDesc d = base_desc(w, st, h);
    d.radius = 10000.0f;                      /* every tile resident */
    JceTerrainCollisionStream *s = jce_terrain_collision_stream_create(&d);
    jce_terrain_collision_stream_update(s, jce_v3(32.0f, 0.0f, 32.0f));

    /* Just inside the far edge of tile (0,0), which spans x in [0,16). */
    const float probe_x[3] = { 15.0f, 31.0f, 15.5f };
    for (int k = 0; k < 3; k++) {
        JceBodyDesc bd;
        memset(&bd, 0, sizeof bd);
        bd.type         = JCE_BODY_DYNAMIC;
        bd.shape        = JCE_SHAPE_SPHERE;
        bd.half_extents = jce_v3(0.5f, 0.0f, 0.0f);
        bd.mass         = 1.0f;
        bd.position     = jce_v3(probe_x[k], 25.0f, 8.0f);
        bd.rotation     = jce_q_identity();
        bd.friction     = 0.5f;
        JceBodyHandle ball = jce_physics_body_create(w, &bd);
        TEST_ASSERT_TRUE(jce_body_valid(ball));

        for (int i = 0; i < 600; i++) jce_physics_step(w, 1.0f / 120.0f);

        jce_vec3 pos; jce_quat rot;
        jce_physics_body_get_transform(w, ball, &pos, &rot);
        /* Must land, not fall through a gap the tiling invented. */
        TEST_ASSERT_FLOAT_WITHIN(0.35f, GROUND + 0.5f, pos.y);
        jce_physics_body_destroy(w, ball);
    }

    jce_terrain_collision_stream_destroy(s);
    jce_terrain_store_release(st, h);
    jce_terrain_store_destroy(st);
    jce_physics_destroy(w);
}

/* ── 4c. The SAMPLER source (the production path for tiled terrain) ────
 *
 * rt_try_spawn_terrain feeds the stream from a tile-aware JceTerrain rather
 * than from the cooked store, because that is what shipped .terrain.json
 * content actually produces -- and it was the case with no collider at all.
 * The store path being correct says nothing about this one, so it gets its own
 * drop test and its own seam test. */

static int g_sample_calls;

/* FLAT within a tile, but a different level per tile.
 *
 * A ramp was tried first and was the wrong instrument: a sphere rolls, so it
 * left the terrain entirely and the test measured a fall, not a landing.  A
 * per-tile plateau still proves the tile ORIGIN mapping -- land on the wrong
 * tile and the altitude is wrong -- without inducing motion. */
static float tile_level(uint32_t tx, uint32_t tz)
{
    return 4.0f + (float)tx * 2.0f + (float)tz * 0.5f;
}

static bool plateau_sampler(void *ctx, uint32_t tx, uint32_t tz, uint32_t span,
                            float ox, float oz, float tile_ws, float *out)
{
    (void)ctx; (void)tx; (void)tz; (void)tile_ws;
    g_sample_calls++;
    /* Derive the level from the WORLD ORIGIN the stream handed us, not from
     * the tile indices.  An earlier version used the indices and was therefore
     * blind to the origin entirely: a mutation that shifted the sampler's
     * origin by a whole tile changed nothing observable, because the sampler
     * never looked at it.  A test that ignores an input cannot test it. */
    const float lvl = 4.0f + (ox / TILE_SIZE) * 2.0f + (oz / TILE_SIZE) * 0.5f;
    for (uint32_t i = 0; i < span * span; i++) out[i] = lvl;
    return true;
}

static void test_sampler_source_is_solid_and_positioned(void)
{
    g_sample_calls = 0;
    JcePhysicsWorld *w = make_world();

    JceTerrainCollisionStreamDesc d;
    memset(&d, 0, sizeof d);
    d.world           = w;
    d.sample_fn       = plateau_sampler;
    d.sample_span     = 17u;
    d.tiles_x         = TILES;
    d.tiles_z         = TILES;
    d.origin          = jce_v3(0.0f, 0.0f, 0.0f);
    d.tile_world_size = TILE_SIZE;
    d.min_height      = 0.0f;
    d.max_height      = 32.0f;
    d.radius          = 10000.0f;
    d.friction        = 0.8f;
    d.diagonal        = JCE_HEIGHTFIELD_DIAG_FIXED;
    d.smooth_internal_edges = true;
    d.max_bodies      = 32u;

    /* No store at all: the sampler must be a complete source on its own. */
    JceTerrainCollisionStream *s = jce_terrain_collision_stream_create(&d);
    TEST_ASSERT_NOT_NULL(s);
    jce_terrain_collision_stream_update(s, jce_v3(32.0f, 0.0f, 32.0f));
    TEST_ASSERT_EQUAL_UINT32(TILES * TILES,
                             jce_terrain_collision_stream_active(s));
    TEST_ASSERT_TRUE(g_sample_calls > 0);

    /* Drop at the CENTRE of several tiles.  Each plateau is at a different
     * height, so landing on the wrong tile -- a mis-mapped origin -- produces
     * the wrong altitude rather than merely "something stopped the ball". */
    const uint32_t probe[3][2] = { { 0u, 0u }, { 2u, 1u }, { 3u, 3u } };
    for (int k = 0; k < 3; k++) {
        const uint32_t tx = probe[k][0], tz = probe[k][1];
        JceBodyDesc bd;
        memset(&bd, 0, sizeof bd);
        bd.type         = JCE_BODY_DYNAMIC;
        bd.shape        = JCE_SHAPE_SPHERE;
        bd.half_extents = jce_v3(0.5f, 0.0f, 0.0f);
        bd.mass         = 1.0f;
        bd.position     = jce_v3(((float)tx + 0.5f) * TILE_SIZE, 28.0f,
                                 ((float)tz + 0.5f) * TILE_SIZE);
        bd.rotation     = jce_q_identity();
        bd.friction     = 0.9f;
        JceBodyHandle ball = jce_physics_body_create(w, &bd);

        for (int i = 0; i < 900; i++) jce_physics_step(w, 1.0f / 120.0f);

        jce_vec3 pos; jce_quat rot;
        jce_physics_body_get_transform(w, ball, &pos, &rot);
        TEST_ASSERT_FLOAT_WITHIN(0.35f, tile_level(tx, tz) + 0.5f, pos.y);
        jce_physics_body_destroy(w, ball);
    }

    jce_terrain_collision_stream_destroy(s);
    jce_physics_destroy(w);
}

/* A stream with neither source is refused rather than silently inert. */
static void test_stream_requires_a_source(void)
{
    JcePhysicsWorld *w = make_world();
    JceTerrainCollisionStreamDesc d;
    memset(&d, 0, sizeof d);
    d.world           = w;
    d.tiles_x         = 1u;
    d.tiles_z         = 1u;
    d.tile_world_size = 16.0f;
    TEST_ASSERT_NULL(jce_terrain_collision_stream_create(&d));
    jce_physics_destroy(w);
}

/* ── 5. The body cap is respected ──────────────────────────────────────  */

static void test_body_cap_is_respected(void)
{
    JcePhysicsWorld *w = make_world();
    JceTerrainHandle h;
    JceTerrainStore *st = make_store(&h);
    JceTerrainCollisionStreamDesc d = base_desc(w, st, h);
    d.radius     = 10000.0f;    /* everything is "near" */
    d.max_bodies = 3u;
    JceTerrainCollisionStream *s = jce_terrain_collision_stream_create(&d);

    jce_terrain_collision_stream_update(s, jce_v3(32.0f, 0.0f, 32.0f));
    TEST_ASSERT_EQUAL_UINT32(3u, jce_terrain_collision_stream_active(s));

    jce_terrain_collision_stream_destroy(s);
    jce_terrain_store_release(st, h);
    jce_terrain_store_destroy(st);
    jce_physics_destroy(w);
}

/* ── 6. Missing tiles and NULL safety ──────────────────────────────────  */

static void test_missing_tiles_and_nulls(void)
{
    TEST_ASSERT_NULL(jce_terrain_collision_stream_create(NULL));
    jce_terrain_collision_stream_destroy(NULL);
    jce_terrain_collision_stream_update(NULL, jce_v3(0, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_terrain_collision_stream_active(NULL));
    TEST_ASSERT_EQUAL_UINT64(0u, jce_terrain_collision_stream_created(NULL));

    JcePhysicsWorld *w = make_world();
    JceTerrainHandle h;
    JceTerrainStore *st = make_store(&h);

    /* A grid larger than the loader supplies: the out-of-range tiles must
     * simply get no body, not a fabricated flat one. */
    JceTerrainCollisionStreamDesc d = base_desc(w, st, h);
    d.tiles_x = TILES + 2u;
    d.tiles_z = TILES + 2u;
    d.radius  = 10000.0f;
    JceTerrainCollisionStream *s = jce_terrain_collision_stream_create(&d);
    jce_terrain_collision_stream_update(s, jce_v3(32.0f, 0.0f, 32.0f));

    JceTerrainTileKey bad = { TILES + 1u, TILES + 1u };
    TEST_ASSERT_FALSE(jce_terrain_collision_stream_has_tile(s, bad));
    TEST_ASSERT_EQUAL_UINT32(TILES * TILES,
                             jce_terrain_collision_stream_active(s));

    jce_terrain_collision_stream_destroy(s);
    jce_terrain_store_release(st, h);
    jce_terrain_store_destroy(st);
    jce_physics_destroy(w);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_residency_follows_focus);
    RUN_TEST(test_steady_focus_does_not_churn);
    RUN_TEST(test_hysteresis_prevents_boundary_flicker);
    RUN_TEST(test_object_rests_on_streamed_terrain);
    RUN_TEST(test_ground_is_continuous_across_a_seam);
    RUN_TEST(test_sampler_source_is_solid_and_positioned);
    RUN_TEST(test_stream_requires_a_source);
    RUN_TEST(test_body_cap_is_respected);
    RUN_TEST(test_missing_tiles_and_nulls);
    return UNITY_END();
}
