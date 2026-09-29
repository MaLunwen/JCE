/*
 * test_jce_physics2d_layers.c — the 2D collision layer matrix reaches Box2D.
 *
 * The 32x32 2D grid in Project Settings was editable, persisted and shipped,
 * and reached NOTHING.  Measured before this unit:
 *
 *   b2Filter / categoryBits / maskBits in jce_physics2d.c   0 occurrences
 *   JceBody2DDesc                                           no layer field
 *   JceRigidBody2DComponent                                 no physics_layer
 *   jce_editor_play.cpp                                     forwarded gravity2d only
 *   jce_build_manager.cpp:595                               wrote the 3D matrix
 *                                                           into the build; no
 *                                                           2D counterpart
 *
 * An author set "Player does not collide with Pickups", saved it, shipped it,
 * and every 2D body collided with every other one.  Nothing errored -- it
 * looks like level design rather than a bug, which is the same failure mode
 * Collider2D.is_trigger had before it was wired.
 *
 * WHY THIS IS A GAP AND NOT A DECISION: the 3D sibling has been wired since
 * P3-C.2, and there is no comment anywhere claiming the 2D side was
 * deliberate.  A no-caller finding has to be read at the CALL SITE before it
 * is called debt -- the asset-thumbnail subsystem has zero callers too, and
 * there the source says "disabled by user request".  Here it says nothing.
 *
 * THE PROBE IS THE THING A LAYER IS FOR: drop a dynamic box onto a static
 * platform and ask whether it was stopped.  Both directions are asserted, so
 * a build that filtered everything out fails as loudly as one that filtered
 * nothing.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory.
 */

#include "unity.h"

#include <jce/middleware/physics/jce_physics2d.h>
#include <jce/middleware/physics/jce_physics_layers.h>

#include <stdbool.h>
#include <string.h>

void setUp(void)    { jce_physics_layer_matrix_reset_default(); }
void tearDown(void) { jce_physics_layer_matrix_reset_default(); }

#define STEPS 120
#define DT    (1.0f / 60.0f)

enum { L_PLATFORM = 1u, L_FALLER = 2u };

static JcePhysics2D *make_world(void)
{
    JcePhysics2DDesc wd;
    memset(&wd, 0, sizeof(wd));
    wd.gravity.x  = 0.0f;
    wd.gravity.y  = -9.81f;
    wd.max_bodies = 16u;
    return jce_physics2d_create(&wd);
}

/* Static platform on L_PLATFORM at y=0, dynamic box on L_FALLER from y=3.
 * `extra_shape` exercises the two-step construction path: a second box added
 * through jce_physics2d_body_add_box AFTER creation, which must inherit the
 * body's layer rather than silently landing on Default.
 * Returns the faller's y after STEPS ticks. */
static float drop_with_layers(bool collide, bool extra_shape)
{
    jce_physics2d_set_layer_collides(L_PLATFORM, L_FALLER, collide);

    JcePhysics2D *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    JceBody2DDesc pd;
    memset(&pd, 0, sizeof(pd));
    pd.type = JCE_BODY_STATIC;
    pd.shape = JCE_SHAPE2D_BOX;
    pd.position.x = 0.0f; pd.position.y = 0.0f;
    pd.half_extents.x = 4.0f; pd.half_extents.y = 0.5f;
    pd.physics_layer = L_PLATFORM;
    (void)jce_physics2d_body_create(w, &pd);

    JceBody2DDesc fd;
    memset(&fd, 0, sizeof(fd));
    fd.type = JCE_BODY_DYNAMIC;
    fd.shape = JCE_SHAPE2D_BOX;
    fd.position.x = 0.0f; fd.position.y = 3.0f;
    fd.half_extents.x = 0.25f; fd.half_extents.y = 0.25f;
    fd.mass = 1.0f;
    fd.physics_layer = L_FALLER;
    JceBodyHandle faller = jce_physics2d_body_create(w, &fd);

    if (extra_shape) {
        jce_vec2 centre; centre.x = 0.0f; centre.y = 0.0f;
        jce_vec2 he;     he.x = 0.25f;    he.y = 0.25f;
        (void)jce_physics2d_body_add_box(w, faller, centre, he,
                                         0.5f, 0.0f, false);
    }

    for (int i = 0; i < STEPS; ++i)
        jce_physics2d_step(w, DT);

    jce_vec2 p; float ang = 0.0f;
    memset(&p, 0, sizeof(p));
    jce_physics2d_body_get_transform(w, faller, &p, &ang);
    jce_physics2d_destroy(w);
    return p.y;
}

/* ── 1. the control: layers that collide still stop the faller ──────── */
static void test_colliding_layers_stop_the_faller(void)
{
    const float y = drop_with_layers(true, false);
    TEST_ASSERT_TRUE_MESSAGE(y > 0.0f,
        "two layers the matrix says DO collide must behave exactly as before "
        "this unit existed -- if this fails, the filter is rejecting pairs it "
        "should pass and every other assertion here is green for the wrong "
        "reason");
}

/* ── 2. the point of the feature ─────────────────────────────────────── */
static void test_non_colliding_layers_let_it_through(void)
{
    const float y = drop_with_layers(false, false);
    TEST_ASSERT_TRUE_MESSAGE(y < -1.0f,
        "the matrix says these two layers do NOT collide, so the faller must "
        "pass through.  Resting on the platform is the defect this unit "
        "exists for: an authored 32x32 grid that changes nothing");
}

/* ── 3. a shape added AFTER creation inherits the body's layer ───────── */
static void test_a_later_shape_inherits_the_body_layer(void)
{
    /* Box2D stores the filter per SHAPE.  jce_physics2d_body_add_box builds
     * its own b2ShapeDef, so without the body's layer recorded at creation it
     * would default to layer 0 -- which collides with everything under the
     * stock matrix -- and the faller would be stopped by a shape the author
     * never put on Default.  That defect existed in the first draft of this
     * unit and this case is what caught it. */
    const float y = drop_with_layers(false, true);
    TEST_ASSERT_TRUE_MESSAGE(y < -1.0f,
        "a second shape added through add_box must inherit the body's layer; "
        "landing on the platform means it silently defaulted to 0");
}

/* ── 4. the two matrices are independent ─────────────────────────────── */
static void test_the_2d_matrix_is_separate_from_the_3d_one(void)
{
    /* Both default to all-collide. */
    TEST_ASSERT_TRUE(jce_physics_get_layer_collides(L_PLATFORM, L_FALLER));
    TEST_ASSERT_TRUE(jce_physics2d_get_layer_collides(L_PLATFORM, L_FALLER));

    jce_physics2d_set_layer_collides(L_PLATFORM, L_FALLER, false);
    TEST_ASSERT_FALSE_MESSAGE(
        jce_physics2d_get_layer_collides(L_PLATFORM, L_FALLER),
        "the 2D set must take effect in the 2D matrix");
    TEST_ASSERT_TRUE_MESSAGE(
        jce_physics_get_layer_collides(L_PLATFORM, L_FALLER),
        "...and must NOT touch the 3D matrix.  They are separate because "
        "JceProjectSettings authors both and the editor draws both grids; "
        "one shared matrix would delete a distinction the authoring surface "
        "already exposes");

    /* And the other direction, so this cannot pass by the 2D setter being a
     * no-op that leaves 3D alone for the wrong reason. */
    jce_physics_layer_matrix_reset_default();
    jce_physics_set_layer_collides(L_PLATFORM, L_FALLER, false);
    TEST_ASSERT_FALSE(jce_physics_get_layer_collides(L_PLATFORM, L_FALLER));
    TEST_ASSERT_TRUE_MESSAGE(
        jce_physics2d_get_layer_collides(L_PLATFORM, L_FALLER),
        "a 3D set must not touch the 2D matrix either");
}

/* ── 5. the ship path: JSON carries the 2D matrix ────────────────────── */
static void test_json_carries_the_2d_matrix(void)
{
    /* Row 1 with only bit 0 set: layer 1 collides with layer 0 and nothing
     * else.  Hand-written rather than round-tripped through save_json so the
     * assertion is about the FORMAT the shipped runtime reads, not about this
     * process agreeing with itself. */
    static const char DOC[] =
        "{\"$schema\":\"jce.physlayers.v1\","
        "\"matrix2d\":[4294967295,1,4294967295]}";
    TEST_ASSERT_TRUE(jce_physics_layer_matrix_load_json_mem(DOC, 0));
    TEST_ASSERT_TRUE(jce_physics2d_get_layer_collides(1u, 0u));
    TEST_ASSERT_FALSE_MESSAGE(jce_physics2d_get_layer_collides(1u, 2u),
        "the matrix2d rows must reach the 2D matrix -- without this the "
        "authored grid works in Editor Play (which pushes it straight into "
        "the cache) and reverts to all-collide in every packaged build");
}

/* ── 6. ABSENT is not EMPTY ──────────────────────────────────────────── */
static void test_a_document_without_matrix2d_stays_permissive(void)
{
    /* A file written before 2D was wired has no "matrix2d".  The right
     * reading is "this project never expressed a 2D matrix" -- the
     * all-collide default -- NOT "every 2D layer collides with nothing",
     * which is what zeroing on absence would mean and would drop every 2D
     * body through the world on the first load of an old project. */
    static const char OLD_DOC[] =
        "{\"$schema\":\"jce.physlayers.v1\",\"matrix\":[4294967295,1]}";
    TEST_ASSERT_TRUE(jce_physics_layer_matrix_load_json_mem(OLD_DOC, 0));
    TEST_ASSERT_TRUE_MESSAGE(jce_physics2d_get_layer_collides(1u, 2u),
        "an older document must leave the 2D matrix permissive");
    /* ...while the 3D rows it DOES carry still land. */
    TEST_ASSERT_FALSE(jce_physics_get_layer_collides(1u, 2u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_colliding_layers_stop_the_faller);
    RUN_TEST(test_non_colliding_layers_let_it_through);
    RUN_TEST(test_a_later_shape_inherits_the_body_layer);
    RUN_TEST(test_the_2d_matrix_is_separate_from_the_3d_one);
    RUN_TEST(test_json_carries_the_2d_matrix);
    RUN_TEST(test_a_document_without_matrix2d_stays_permissive);
    return UNITY_END();
}
