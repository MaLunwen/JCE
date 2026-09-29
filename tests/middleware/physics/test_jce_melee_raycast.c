/*
 * test_jce_melee_raycast.c — Headless reproduction of the street_demo melee bug:
 * "the player collides with the enemy but jce.raycast never hits it."
 *
 * The enemy is authored exactly like the runtime makes it (jce_runtime.c rigid-
 * body spawn): a KINEMATIC capsule, JceBodyDesc memset to 0 (so collision
 * group/mask are 0 → defaulted by jce_physics_body_create), then tagged with its
 * entity (jce_physics_body_set_entity) and assigned physics layer 0
 * (jce_physics_body_set_layer) — both done by rt_apply_body_extras.
 *
 * Each test isolates one variable so a failure pinpoints the cause:
 *   1. ray_hits_capsule_runtime_setup — the full runtime setup (set_entity +
 *      set_layer(0)).  The headline question: does a forward ray hit the enemy
 *      and return its entity id?
 *   2. ray_hits_capsule_no_setlayer  — identical but WITHOUT set_layer(0).  If
 *      this passes while #1 fails, set_layer(0) is breaking the broadphase
 *      group/mask for raycasts.
 *   3. ray_tracks_set_transform      — move the body via set_transform (what
 *      rt_push_external_transforms does for a Lua set_position) and re-cast: the
 *      ray must hit at the NEW position, not the spawn.
 *   4. ray_origin_inside_untagged    — a ray STARTING INSIDE an untagged body
 *      (the player's own capsule, entity 0) with a tagged enemy behind it: does
 *      ClosestRayResultCallback return the untagged origin body (entity 0,
 *      masking the enemy) or skip it and hit the enemy?
 *
 * Linked against jce_core + jce_physics (mirrors test_jce_constant_force).
 */

#include "unity.h"

#include <jce/middleware/physics/jce_physics.h>
#include <jce/middleware/physics/jce_physics_debug.h>   /* set/get_entity */
#include <jce/middleware/physics/jce_physics_layers.h>  /* set_layer */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static const float DT = 1.0f / 60.0f;

/* A world with gravity, like the editor Play world. */
static JcePhysicsWorld *make_world(void)
{
    JcePhysicsWorldDesc wd;
    memset(&wd, 0, sizeof(wd));
    wd.gravity        = jce_v3(0.0f, -9.81f, 0.0f);
    wd.max_bodies     = 64u;
    wd.fixed_timestep = DT;
    wd.max_sub_steps  = 4;
    return jce_physics_create(&wd);
}

/* Enemy exactly as the runtime makes it: KINEMATIC capsule, memset desc (no
 * explicit group/mask), then set_entity + (optionally) set_layer(0). */
static JceBodyHandle make_enemy(JcePhysicsWorld *w, jce_vec3 pos, uint64_t ent,
                                int call_set_layer)
{
    JceBodyDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.type         = JCE_BODY_KINEMATIC;
    bd.shape        = JCE_SHAPE_CAPSULE;
    bd.position     = pos;
    bd.rotation     = jce_q_identity();
    bd.half_extents = jce_v3(0.4f, 0.5f, 0.0f);   /* runtime: r=0.4, half-h=0.5 */
    bd.mass         = 0.0f;
    bd.friction     = 0.5f;

    JceBodyHandle b = jce_physics_body_create(w, &bd);
    jce_physics_body_set_entity(w, b, ent);                       /* rt_apply_body_extras */
    if (call_set_layer) jce_physics_body_set_layer(w, b, 0u);     /* rt_apply_body_extras */
    return b;
}

/* 1. Full runtime setup: ray must hit + return the enemy entity. */
static void test_ray_hits_capsule_runtime_setup(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    make_enemy(w, jce_v3(0.0f, 1.0f, 3.0f), 556u, 1 /* set_layer(0) */);
    jce_physics_step(w, DT);

    JceRaycastResult r = jce_physics_raycast(w, jce_v3(0.0f, 1.0f, 0.0f),
                                             jce_v3(0.0f, 0.0f, 1.0f), 8.0f);
    printf("[runtime-setup] hit=%d dist=%.2f\n", (int)r.hit, r.distance);
    TEST_ASSERT_TRUE_MESSAGE(r.hit,
        "RAY MISSED the kinematic capsule with the runtime setup (set_layer 0)");
    uint64_t he = jce_physics_body_get_entity(w, r.body);
    printf("[runtime-setup] hit_entity=%llu\n", (unsigned long long)he);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(556u, he,
        "ray hit the capsule but entity tag is wrong/zero");

    jce_physics_destroy(w);
}

/* 2. Same WITHOUT set_layer(0) — isolates set_layer as the culprit. */
static void test_ray_hits_capsule_no_setlayer(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    make_enemy(w, jce_v3(0.0f, 1.0f, 3.0f), 556u, 0 /* no set_layer */);
    jce_physics_step(w, DT);

    JceRaycastResult r = jce_physics_raycast(w, jce_v3(0.0f, 1.0f, 0.0f),
                                             jce_v3(0.0f, 0.0f, 1.0f), 8.0f);
    printf("[no-setlayer] hit=%d dist=%.2f ent=%llu\n", (int)r.hit, r.distance,
           (unsigned long long)(r.hit ? jce_physics_body_get_entity(w, r.body) : 0));
    TEST_ASSERT_TRUE_MESSAGE(r.hit, "RAY MISSED even without set_layer");
    TEST_ASSERT_EQUAL_UINT64(556u, jce_physics_body_get_entity(w, r.body));

    jce_physics_destroy(w);
}

/* 3. Tracking: move via set_transform (Lua set_position bridge), hit at new pos. */
static void test_ray_tracks_set_transform(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    JceBodyHandle e = make_enemy(w, jce_v3(10.0f, 1.0f, 10.0f), 556u, 1);
    jce_physics_step(w, DT);
    /* enemy "walks" to (0,1,3) */
    jce_physics_body_set_transform(w, e, jce_v3(0.0f, 1.0f, 3.0f), jce_q_identity());
    jce_physics_step(w, DT);

    JceRaycastResult r = jce_physics_raycast(w, jce_v3(0.0f, 1.0f, 0.0f),
                                             jce_v3(0.0f, 0.0f, 1.0f), 8.0f);
    printf("[tracks] hit=%d dist=%.2f\n", (int)r.hit, r.distance);
    TEST_ASSERT_TRUE_MESSAGE(r.hit, "RAY MISSED after set_transform (no tracking)");
    TEST_ASSERT_EQUAL_UINT64(556u, jce_physics_body_get_entity(w, r.body));

    jce_physics_destroy(w);
}

/* 4. Ray origin INSIDE an untagged body (the player's own capsule, ent 0) with a
 *    tagged enemy behind: does ClosestRay return the origin body (ent 0, masking
 *    the enemy) or skip it and hit the enemy (556)? */
static void test_ray_origin_inside_untagged(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    make_enemy(w, jce_v3(0.0f, 1.0f, 0.0f), 0u,   1);  /* "player" at origin, untagged */
    make_enemy(w, jce_v3(0.0f, 1.0f, 3.0f), 556u, 1);  /* enemy behind */
    jce_physics_step(w, DT);

    JceRaycastResult r = jce_physics_raycast(w, jce_v3(0.0f, 1.0f, 0.0f),
                                             jce_v3(0.0f, 0.0f, 1.0f), 8.0f);
    uint64_t he = r.hit ? jce_physics_body_get_entity(w, r.body) : (uint64_t)-1;
    printf("[origin-inside] hit=%d dist=%.2f ent=%llu\n", (int)r.hit, r.distance,
           (unsigned long long)he);
    TEST_ASSERT_TRUE_MESSAGE(r.hit, "ray from inside-origin hit nothing");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(556u, he,
        "SELF-HIT: ray returned the untagged origin body (ent 0), masking the enemy");

    jce_physics_destroy(w);
}

/* NOTE: the collider-center-offset DROP bug is a RUNTIME bug (rt_push_external_
 * transforms teleporting an offset collider to the bare entity origin), not a
 * physics-primitive bug — jce_physics_body_set_transform correctly puts the body
 * wherever it is told.  That bug + its fix are covered end-to-end by the runtime
 * test tests/application/test_jce_melee_offset_track.c. */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ray_hits_capsule_runtime_setup);
    RUN_TEST(test_ray_hits_capsule_no_setlayer);
    RUN_TEST(test_ray_tracks_set_transform);
    RUN_TEST(test_ray_origin_inside_untagged);
    return UNITY_END();
}
