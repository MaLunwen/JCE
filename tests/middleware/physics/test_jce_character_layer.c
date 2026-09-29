/*
 * test_jce_character_layer.c
 *
 * The character capsule must obey the 32-slot Physics Layer Collision Matrix.
 *
 * Until 2026-08-31 it did not, and the reason was an ALIASING bug rather than a
 * missing feature.  The capsule was added to the world with Bullet's own filter
 * constants:
 *
 *     addRigidBody(body, btBroadphaseProxy::CharacterFilter,          // == 32
 *                        btBroadphaseProxy::StaticFilter |            // == 2
 *                        btBroadphaseProxy::DefaultFilter);           // == 1
 *
 * Those values ARE JCE layer bits: 32 is layer 5, and 2|1 is "layers 1 and 0".
 * So the character silently behaved as if pinned to layer 5, colliding with
 * layers 0 and 1 and NOTHING else, whatever the designer authored.  Ground on
 * a "Terrain" layer (2..31) meant the player fell through the world.
 *
 * Why the obvious test would have missed it: the default matrix is all-ones and
 * a default scene puts everything on layer 0, so a drop-and-land test passes
 * both before and after the fix.  The defect only shows when the FLOOR IS NOT
 * ON LAYER 0 OR 1 -- which is exactly what case A does.
 *
 * The three cases are a matched set: A is the regression (a floor the matrix
 * says to collide with, on a layer the old code could not see), B is the
 * positive control on the same geometry with everything on layer 0 (so a
 * failure in A cannot be blamed on the rig), and C is the negative control --
 * the matrix is told NOT to collide, and the capsule must then fall through
 * the very floor it rested on in B.  Without C, "the capsule stopped" would
 * also be satisfied by a build that ignores the matrix and collides always.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 * The tracked contract for this defect is
 * tools/lint/check_physics_layer_filters.py.
 */

#include <jce/middleware/physics/jce_physics.h>
#include <jce/middleware/physics/jce_physics_layers.h>

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define FLOOR_LAYER  7u    /* deliberately outside Bullet's Default|Static */
#define CHAR_LAYER   0u

/* Capsule centre starts here; the floor top is at y = 0. */
#define START_Y      3.0f
#define CAPSULE_H    1.8f
#define CAPSULE_R    0.3f

/* Settled height of the capsule CENTRE when resting on a floor whose top face
 * is y = 0: half the capsule plus a little solver penetration slack. */
#define RESTING_Y    (0.5f * CAPSULE_H)

static JcePhysicsWorld *make_world(void)
{
    JcePhysicsWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.gravity        = jce_v3(0.0f, -9.81f, 0.0f);
    wd.max_bodies     = 64;
    wd.fixed_timestep = 1.0f / 60.0f;
    wd.max_sub_steps  = 4;
    return jce_physics_create(&wd);
}

/* A wide static box whose TOP face sits at y = 0, on `layer`. */
static JceBodyHandle make_floor(JcePhysicsWorld *w, uint32_t layer)
{
    JceBodyDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.type         = JCE_BODY_STATIC;
    bd.shape        = JCE_SHAPE_BOX;
    bd.position     = jce_v3(0.0f, -1.0f, 0.0f);
    bd.rotation     = jce_q_identity();
    bd.half_extents = jce_v3(20.0f, 1.0f, 20.0f);
    bd.mass         = 0.0f;
    bd.friction     = 0.8f;

    JceBodyHandle b = jce_physics_body_create(w, &bd);
    TEST_ASSERT_TRUE(jce_body_valid(b));
    /* Same call the runtime makes for every authored rigid body. */
    jce_physics_body_set_layer(w, b, layer);
    return b;
}

static JceCharacterHandle make_character(JcePhysicsWorld *w, uint32_t layer)
{
    JceCharacterDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.position    = jce_v3(0.0f, START_Y, 0.0f);
    cd.radius      = CAPSULE_R;
    cd.height      = CAPSULE_H;
    cd.step_height = 0.35f;
    cd.max_slope_deg = 50.0f;
    cd.gravity     = 9.81f;
    cd.jump_speed  = 5.0f;
    cd.layer       = layer;

    JceCharacterHandle ch = jce_physics_character_create(w, &cd);
    TEST_ASSERT_TRUE(jce_character_valid(ch));
    return ch;
}

/* Step long enough to fall 3 m and settle (2 s at 1/60). */
static float settle(JcePhysicsWorld *w, JceCharacterHandle ch)
{
    jce_vec3 p = jce_v3(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 120; ++i) {
        jce_physics_character_move(w, ch, jce_v3(0.0f, 0.0f, 0.0f), 1.0f / 60.0f);
        jce_physics_step(w, 1.0f / 60.0f);
    }
    jce_physics_character_get_position(w, ch, &p);
    return p.y;
}

/* ── A: the regression.  Floor on layer 7, matrix says collide. ─────────── */
static void test_character_collides_with_a_high_layer(void)
{
    jce_physics_layer_matrix_reset_default();   /* all pairs collide */
    TEST_ASSERT_TRUE(jce_physics_get_layer_collides(CHAR_LAYER, FLOOR_LAYER));

    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);
    make_floor(w, FLOOR_LAYER);
    JceCharacterHandle ch = make_character(w, CHAR_LAYER);

    float y = settle(w, ch);
    jce_physics_destroy(w);

    /* Before the fix the capsule's mask was layers {0,1}, so a floor on layer
     * 7 was invisible and this y was around -15 (still falling). */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
        0.15f, RESTING_Y, y,
        "character fell through a floor the collision matrix says it collides "
        "with -- its broadphase mask is not coming from the layer matrix");
}

/* ── B: positive control.  Same rig, everything on layer 0. ─────────────── */
static void test_character_rests_on_a_layer_zero_floor(void)
{
    jce_physics_layer_matrix_reset_default();

    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);
    make_floor(w, 0u);
    JceCharacterHandle ch = make_character(w, CHAR_LAYER);

    float y = settle(w, ch);
    jce_physics_destroy(w);

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
        0.15f, RESTING_Y, y,
        "the rig itself is broken: the capsule does not rest even on a "
        "layer-0 floor, so case A proves nothing");
}

/* ── C: negative control.  Matrix says DO NOT collide. ──────────────────── */
static void test_character_passes_through_a_disabled_pair(void)
{
    jce_physics_layer_matrix_reset_default();
    jce_physics_set_layer_collides(CHAR_LAYER, FLOOR_LAYER, false);
    TEST_ASSERT_FALSE(jce_physics_get_layer_collides(CHAR_LAYER, FLOOR_LAYER));

    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);
    make_floor(w, FLOOR_LAYER);
    JceCharacterHandle ch = make_character(w, CHAR_LAYER);

    float y = settle(w, ch);
    jce_physics_layer_matrix_reset_default();   /* process-wide cache */
    jce_physics_destroy(w);

    /* Two seconds of free fall from y=3 is about -16; anything at or above the
     * resting height means the capsule collided with a pair the designer
     * switched OFF. */
    TEST_ASSERT_TRUE_MESSAGE(
        y < -1.0f,
        "character collided with a floor whose layer pair is disabled in the "
        "matrix -- the capsule ignores the matrix and collides unconditionally");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_character_collides_with_a_high_layer);
    RUN_TEST(test_character_rests_on_a_layer_zero_floor);
    RUN_TEST(test_character_passes_through_a_disabled_pair);
    return UNITY_END();
}
