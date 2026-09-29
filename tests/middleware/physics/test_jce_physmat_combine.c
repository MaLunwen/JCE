/*
 * test_jce_physmat_combine.c — the authored combine mode reaches a contact.
 *
 * jce_physics_material_combine() has implemented Unity's precedence rule
 * (MAX > MULTIPLY > MIN > AVERAGE) since it shipped, and HAD NO CALLER
 * ANYWHERE: not in engine/src, editor/src, scripting or tools.  The editor
 * authors both modes, the loader round-trips them, and the one path from
 * asset into simulation -- jce_physics_body_set_material -- forwarded only
 * dynamic_friction and restitution to Bullet, whose own combine is a fixed
 * multiply.  Every authored AVERAGE/MIN/MAX/MULTIPLY was recorded, shown in
 * the Inspector, and dropped.
 *
 * TWO ASSERTIONS DOING TWO DIFFERENT JOBS, because one cannot do both:
 *
 *   the rule      is pure and pinned directly, against the table -- and
 *                 against the function itself, never a hardcoded number, so
 *                 changing the precedence cannot leave a test asserting the
 *                 old one;
 *   reaching it   is a SIMULATION measurement.  A box is launched across a
 *                 floor and the distance it slides is compared between MIN
 *                 and MAX.  The rule being right says nothing about whether
 *                 the solver ever sees it -- that is the entire defect this
 *                 file exists for.
 *
 * Headless: Bullet needs no GPU, so nothing here is forced into a capture.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory.
 */
#include "unity.h"

#include <jce/middleware/physics/jce_physics.h>
#include <jce/middleware/physics/jce_physics_material.h>

#include <math.h>
#include <stdbool.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- The rule itself ---------------------------------------------- */

static JcePhysicsMaterial mat(float friction, JcePhysicsCombine fc)
{
    JcePhysicsMaterial m;
    jce_physics_material_init_default(&m);
    m.dynamic_friction = friction;
    m.static_friction  = friction;
    m.friction_combine = fc;
    return m;
}

static void test_the_rule_is_what_unity_says(void)
{
    const float A = 0.2f, B = 0.8f;
    float f;

    JcePhysicsMaterial a = mat(A, JCE_PHYS_COMBINE_AVERAGE);
    JcePhysicsMaterial b = mat(B, JCE_PHYS_COMBINE_AVERAGE);
    jce_physics_material_combine(&a, &b, &f, NULL);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.5f * (A + B), f);

    b = mat(B, JCE_PHYS_COMBINE_MIN);
    jce_physics_material_combine(&a, &b, &f, NULL);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, A, f);        /* MIN beats AVERAGE */

    b = mat(B, JCE_PHYS_COMBINE_MULTIPLY);
    jce_physics_material_combine(&a, &b, &f, NULL);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, A * B, f);    /* MULTIPLY beats MIN */

    a = mat(A, JCE_PHYS_COMBINE_MAX);
    jce_physics_material_combine(&a, &b, &f, NULL);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, B, f);        /* MAX beats MULTIPLY */

    /* A NULL side is engine defaults, which is what a body carrying no
     * material has to look like when the other side carries one. */
    jce_physics_material_combine(&a, NULL, &f, NULL);
    TEST_ASSERT_TRUE(isfinite(f));
}

/* ---- Reaching a contact -------------------------------------------- */

/* Launch a box sideways across a floor and return how far it travelled.
 * `fc` is applied to BOTH bodies' materials; JCE_PHYS_COMBINE_AVERAGE with
 * `apply == false` means no material is set at all, which is the arm that
 * shows what the engine did before this wiring existed. */
static float slide(float fa, float fb, JcePhysicsCombine fc, bool apply)
{
    JcePhysicsWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.gravity.y  = -9.81f;
    wd.max_bodies = 16u;
    JcePhysicsWorld *w = jce_physics_create(&wd);
    TEST_ASSERT_NOT_NULL(w);

    JceBodyDesc gd;
    memset(&gd, 0, sizeof gd);
    gd.type = JCE_BODY_STATIC;
    gd.shape = JCE_SHAPE_BOX;
    gd.rotation.w = 1.0f;
    gd.position.y = -0.5f;
    gd.half_extents.x = 200.0f; gd.half_extents.y = 0.5f; gd.half_extents.z = 200.0f;
    gd.collision_group = 0xFFFFFFFFu; gd.collision_mask = 0xFFFFFFFFu;
    /* On the DESC, so the no-material arm gets these values too.  Without it
     * that arm silently falls back to JceBodyDesc's 0.5 default and compares
     * 0.5*0.5 against the other arm's 0.2*0.8 -- two variables moving, and a
     * failure that reads as a defect in the engine rather than in the
     * measurement.  It is what this case's first run actually did. */
    gd.friction = fa;
    JceBodyHandle floor_h = jce_physics_body_create(w, &gd);

    JceBodyDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.type = JCE_BODY_DYNAMIC;
    bd.shape = JCE_SHAPE_BOX;
    bd.rotation.w = 1.0f;
    bd.position.y = 0.5f;
    bd.half_extents.x = 0.5f; bd.half_extents.y = 0.5f; bd.half_extents.z = 0.5f;
    bd.mass = 1.0f;
    bd.collision_group = 0xFFFFFFFFu; bd.collision_mask = 0xFFFFFFFFu;
    bd.friction = fb;
    JceBodyHandle box_h = jce_physics_body_create(w, &bd);

    /* The material re-states the same friction, so applying one changes the
     * COMBINE and nothing else. */
    if (apply) {
        JcePhysicsMaterial mf = mat(fa, fc);
        JcePhysicsMaterial mb = mat(fb, fc);
        jce_physics_body_set_material(w, floor_h, &mf);
        jce_physics_body_set_material(w, box_h,   &mb);
    }

    /* Settle onto the floor before the push, so the first contact frames are
     * not part of the measurement. */
    for (int i = 0; i < 30; ++i) jce_physics_step(w, 1.0f / 60.0f);

    jce_vec3 push = { 12.0f, 0.0f, 0.0f };
    jce_physics_body_set_velocity(w, box_h, push);
    for (int i = 0; i < 240; ++i) jce_physics_step(w, 1.0f / 60.0f);

    jce_vec3 p = { 0.0f, 0.0f, 0.0f };
    jce_quat q = { 0.0f, 0.0f, 0.0f, 1.0f };
    jce_physics_body_get_transform(w, box_h, &p, &q);
    jce_physics_destroy(w);
    return p.x;
}

/* THE CASE THIS FILE EXISTS FOR.
 *
 * Same two friction values, same push, same everything -- only the combine
 * mode differs.  MIN(0.2, 0.8) = 0.2 and MAX(0.2, 0.8) = 0.8, a factor of
 * four in the force opposing the slide, so the distances cannot coincide
 * unless the mode reached the solver.  Before this wiring they were EQUAL:
 * both runs used Bullet's own multiply, 0.16, and the authored mode was
 * dropped at jce_physics_body_set_material. */
static void test_the_mode_reaches_the_solver(void)
{
    float lo_friction = slide(0.2f, 0.8f, JCE_PHYS_COMBINE_MIN, true);
    float hi_friction = slide(0.2f, 0.8f, JCE_PHYS_COMBINE_MAX, true);

    TEST_ASSERT_TRUE(isfinite(lo_friction) && isfinite(hi_friction));
    TEST_ASSERT_TRUE_MESSAGE(lo_friction > 0.5f,
        "the box did not move at all -- the measurement is about the "
        "DIFFERENCE between two slides, and neither arm sliding makes it "
        "vacuous");
    TEST_ASSERT_TRUE_MESSAGE(
        lo_friction > hi_friction * 1.5f,
        "MIN and MAX produced the same slide: the authored combine mode is "
        "not reaching the contact, which is the whole defect this file is "
        "about");
}

/* NEGATIVE CONTROL, and the compatibility statement in one case.
 *
 * A body with NO material must behave exactly as it did before: Bullet's own
 * fixed multiply.  MULTIPLY is the JCE mode that agrees with it, so applying
 * MULTIPLY and applying nothing have to land in the same place.  If they
 * diverge, this change moved content that nobody re-authored. */
static void test_no_material_still_means_bullets_own_combine(void)
{
    float none     = slide(0.2f, 0.8f, JCE_PHYS_COMBINE_AVERAGE, false);
    float multiply = slide(0.2f, 0.8f, JCE_PHYS_COMBINE_MULTIPLY, true);

    TEST_ASSERT_TRUE(isfinite(none) && isfinite(multiply));
    TEST_ASSERT_TRUE_MESSAGE(none > 0.5f, "control arm did not slide");
    /* Same rule, so the same distance.  A loose band because the two runs
     * reach it by different code paths -- Bullet's internal combine versus
     * ours writing the identical number into the manifold -- and float
     * accumulation over 240 steps is not bit-identical between them. */
    TEST_ASSERT_TRUE_MESSAGE(
        fabsf(none - multiply) < 0.05f * none,
        "applying MULTIPLY did not reproduce Bullet's own combine: existing "
        "content with no authored material may have changed behaviour");
}

/* AVERAGE is the DEFAULT combine, and this is the behaviour change worth
 * naming out loud: a body that carries a material now uses the authored
 * rule, so a default material no longer silently behaves as MULTIPLY.
 * AVERAGE(0.2, 0.8) = 0.5 against MULTIPLY's 0.16 -- more friction, so a
 * shorter slide. */
static void test_a_default_material_is_average_not_multiply(void)
{
    float average  = slide(0.2f, 0.8f, JCE_PHYS_COMBINE_AVERAGE, true);
    float multiply = slide(0.2f, 0.8f, JCE_PHYS_COMBINE_MULTIPLY, true);

    TEST_ASSERT_TRUE(isfinite(average) && isfinite(multiply));
    TEST_ASSERT_TRUE_MESSAGE(multiply > average * 1.2f,
        "AVERAGE and MULTIPLY landed together -- an authored default material "
        "is still falling through to Bullet's rule");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_rule_is_what_unity_says);
    RUN_TEST(test_the_mode_reaches_the_solver);
    RUN_TEST(test_no_material_still_means_bullets_own_combine);
    RUN_TEST(test_a_default_material_is_average_not_multiply);
    return UNITY_END();
}
