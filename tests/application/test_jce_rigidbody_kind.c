/*
 * test_jce_rigidbody_kind.c — one answer to "what kind of body is this".
 *
 * There were two, and they disagreed for every scene this editor has ever
 * written:
 *
 *   both 3D spawn paths ..... is_kinematic, then mass <= 0, then dynamic
 *   the soft-body mirror .... rb->body_type == JCE_BODY_DYNAMIC
 *
 * JceRigidBodyComponent.body_type is never parsed (parse_rigidbody reads no
 * "bodyType" key), never serialised, and has no inspector control -- so it is
 * 0 in every component that has ever existed, and 0 is JCE_BODY_STATIC.  The
 * mirror's `dynamic` test was therefore FALSE for every rigid body in every
 * scene, and rt_softbody_mirror_static copied every enabled BoxCollider into
 * the soft world as immovable ground: a soft body rests on a crate that is
 * about to be pushed away, and goes on resting where the crate used to be.
 *
 * WHAT IS ASSERTED HERE is the predicate itself -- the real exported
 * rt_rigidbody_kind, not a copy -- over the four authorings that reach it.
 * The mirror and both spawns now call it, so agreement between them is a
 * property of there being one function rather than something to re-measure at
 * each site.  Driving a soft body onto a crate and watching it fall would
 * need a soft world, a step loop and a tolerance; this needs none of that and
 * fails for the same reason.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include "application/jce_rt_internal.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Exactly what the editor's Add Component > Rigidbody leaves behind: a zeroed
 * component with mass 1.  body_type is 0 because nothing has ever written it. */
static JceRigidBodyComponent authored_default(void)
{
    JceRigidBodyComponent rb;
    memset(&rb, 0, sizeof(rb));
    rb.mass = 1.0f;
    return rb;
}

static void test_the_component_every_editor_scene_carries_is_dynamic(void)
{
    JceRigidBodyComponent rb = authored_default();

    /* THE DEFECT, at the one place it lived.  The old mirror asked
     * `rb->body_type == JCE_BODY_DYNAMIC`, and this component -- the one
     * behind every crate, barrel and prop in the tree -- answers no. */
    TEST_ASSERT_EQUAL_MESSAGE(JCE_BODY_STATIC, rb.body_type,
        "body_type is 0 in a component nothing has ever written, and 0 "
        "is JCE_BODY_STATIC -- that coincidence IS the bug");
    TEST_ASSERT_EQUAL_MESSAGE(JCE_BODY_DYNAMIC, rt_rigidbody_kind(&rb),
        "a rigid body with mass and no kinematic flag is DYNAMIC; if this is "
        "STATIC, every dynamic box in the scene is soft-world ground");
}

static void test_the_other_three_authorings(void)
{
    JceRigidBodyComponent rb = authored_default();

    rb.is_kinematic = true;
    TEST_ASSERT_EQUAL_MESSAGE(JCE_BODY_KINEMATIC, rt_rigidbody_kind(&rb),
        "Unity's Is Kinematic means the body is driven, not simulated");

    /* Kinematic wins over mass: a kinematic body with mass is still driven,
     * and the order mattered enough that both spawn paths already had it. */
    rb.mass = 0.0f;
    TEST_ASSERT_EQUAL(JCE_BODY_KINEMATIC, rt_rigidbody_kind(&rb));

    rb.is_kinematic = false;
    TEST_ASSERT_EQUAL_MESSAGE(JCE_BODY_STATIC, rt_rigidbody_kind(&rb),
        "zero mass is how this engine has always spelled 'immovable'");
}

static void test_no_rigidbody_is_static(void)
{
    /* A collider with no Rigidbody: a floor or a wall.  Both spawn paths
     * already reached this conclusion separately -- rt_spawn_cooked_body in
     * an `else` branch, the primitive spawn in a `!rb` guard -- and it is now
     * one place. */
    TEST_ASSERT_EQUAL_MESSAGE(JCE_BODY_STATIC, rt_rigidbody_kind(NULL),
        "no Rigidbody means static, which is what makes a floor solid");
}

static void test_auto_is_zero_and_zero_is_what_every_scene_carries(void)
{
    /* THE ENCODING IS THE FEATURE.  body_type used to be a JceBodyType, whose
     * STATIC is 0 -- the same 0 a memset'd component carries and the same 0
     * every scene ever written carries, since the field was never parsed and
     * never serialised.  Honouring that would have frozen every dynamic body
     * in the tree, which is why it stayed unwired with a recorded reason.
     *
     * JCE_RB_KIND_AUTO is 0 now, so "nobody said anything" and "the author
     * said static" are finally different bytes.  Asserting AUTO == 0 is not
     * pedantry: if it ever stops being 0, every existing scene silently
     * changes meaning. */
    JceRigidBodyComponent rb = authored_default();

    TEST_ASSERT_EQUAL_MESSAGE(0, JCE_RB_KIND_AUTO,
        "AUTO must be 0, or every scene written before this changes meaning");
    TEST_ASSERT_EQUAL_MESSAGE(JCE_RB_KIND_AUTO, rb.body_type,
        "a memset'd component must read as AUTO");
    TEST_ASSERT_EQUAL_MESSAGE(JCE_BODY_DYNAMIC, rt_rigidbody_kind(&rb),
        "AUTO must mean exactly what the engine did before: mass 1 and not "
        "kinematic is DYNAMIC");
}

static void test_an_author_can_now_say_which(void)
{
    JceRigidBodyComponent rb = authored_default();   /* mass 1, not kinematic */

    /* Each named value overrides what AUTO would have derived, or the control
     * is not a control: AUTO on this component already answers DYNAMIC, so
     * only STATIC and KINEMATIC prove anything here. */
    rb.body_type = (uint8_t)JCE_RB_KIND_STATIC;
    TEST_ASSERT_EQUAL_MESSAGE(JCE_BODY_STATIC, rt_rigidbody_kind(&rb),
        "an author who picks Static gets a static body even with mass");

    rb.body_type = (uint8_t)JCE_RB_KIND_KINEMATIC;
    TEST_ASSERT_EQUAL(JCE_BODY_KINEMATIC, rt_rigidbody_kind(&rb));

    /* DYNAMIC over zero mass: the named value beats the derivation, which is
     * the only way "I meant it" can be expressed. */
    rb.mass = 0.0f;
    rb.body_type = (uint8_t)JCE_RB_KIND_DYNAMIC;
    TEST_ASSERT_EQUAL_MESSAGE(JCE_BODY_DYNAMIC, rt_rigidbody_kind(&rb),
        "an explicit Dynamic must beat the zero-mass derivation");
}

static void test_is_kinematic_still_wins(void)
{
    /* Unity's Is Kinematic is the same override, and a component carrying both
     * is one an author edited in two places.  Taking the more restrictive
     * answer cannot make something fall through the world; taking the other
     * one can. */
    JceRigidBodyComponent rb = authored_default();
    rb.is_kinematic = true;
    rb.body_type    = (uint8_t)JCE_RB_KIND_DYNAMIC;
    TEST_ASSERT_EQUAL_MESSAGE(JCE_BODY_KINEMATIC, rt_rigidbody_kind(&rb),
        "Is Kinematic must win over an explicit Dynamic");
}

static void test_an_existing_c_caller_keeps_its_meaning(void)
{
    /* THE COMPATIBILITY PROPERTY, found the hard way.  The first version of
     * this enum numbered AUTO/STATIC/KINEMATIC/DYNAMIC 0/1/2/3, on the
     * reasoning that the field is never parsed and never serialised so no
     * FILE could carry a stale value.  That checked the data path and not the
     * code path: C callers set this through the public setter with JceBodyType
     * constants, and test_jce_headless_boot.c does exactly that -- its body
     * stopped falling, because JCE_BODY_DYNAMIC (1) had become STATIC.
     *
     * DYNAMIC and KINEMATIC therefore keep JceBodyType's numbers.  Only STATIC
     * moves, off 0 -- and 0 was the one value that could never be honoured
     * anyway, being indistinguishable from "unset". */
    TEST_ASSERT_EQUAL_MESSAGE((int)JCE_BODY_DYNAMIC, (int)JCE_RB_KIND_DYNAMIC,
        "a caller that wrote JCE_BODY_DYNAMIC must still mean DYNAMIC");
    TEST_ASSERT_EQUAL_MESSAGE((int)JCE_BODY_KINEMATIC, (int)JCE_RB_KIND_KINEMATIC,
        "a caller that wrote JCE_BODY_KINEMATIC must still mean KINEMATIC");
    TEST_ASSERT_NOT_EQUAL_MESSAGE((int)JCE_BODY_STATIC, (int)JCE_RB_KIND_STATIC,
        "STATIC must NOT be 0: 0 is 'unset', which is the reason this enum "
        "exists at all");

    /* And behaviourally: the component that test writes must still fall. */
    {
        JceRigidBodyComponent rb = authored_default();
        rb.body_type = (uint8_t)JCE_BODY_DYNAMIC;
        TEST_ASSERT_EQUAL_MESSAGE(JCE_BODY_DYNAMIC, rt_rigidbody_kind(&rb),
            "the exact component test_jce_headless_boot.c builds must still "
            "be dynamic");
    }
}

static void test_a_byte_from_a_newer_file_falls_back_to_auto(void)
{
    /* A scene written by a later build that added a fifth kind must not turn
     * into whichever of the four it happens to alias.  Falling back to the
     * derivation is the behaviour every scene already had. */
    JceRigidBodyComponent rb = authored_default();
    rb.body_type = 200;
    TEST_ASSERT_EQUAL_MESSAGE(JCE_BODY_DYNAMIC, rt_rigidbody_kind(&rb),
        "an unknown kind must fall back to AUTO, not alias onto a known one");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_component_every_editor_scene_carries_is_dynamic);
    RUN_TEST(test_the_other_three_authorings);
    RUN_TEST(test_no_rigidbody_is_static);
    RUN_TEST(test_auto_is_zero_and_zero_is_what_every_scene_carries);
    RUN_TEST(test_an_author_can_now_say_which);
    RUN_TEST(test_is_kinematic_still_wins);
    RUN_TEST(test_an_existing_c_caller_keeps_its_meaning);
    RUN_TEST(test_a_byte_from_a_newer_file_falls_back_to_auto);
    return UNITY_END();
}
