/*
 * test_jce_contact_impulse.c — JceContactEvent.applied_impulse.
 *
 * The contact event carried a normal, a point and a penetration depth, and no
 * measure of how HARD the hit was.  So JceFractureComponent.break_impulse --
 * authored, serialised, in the Inspector -- had nothing to compare against,
 * and fracture stayed API-only: a wall could be hit at any speed and never
 * break unless a script noticed and asked.
 *
 * Bullet had the number all along (btManifoldPoint::m_appliedImpulse); it was
 * simply never carried out.
 *
 * Asserted:
 *   1. a real collision reports a NON-ZERO impulse -- zero would mean every
 *      break threshold is unreachable and the feature is still dead;
 *   2. it TRACKS the hit: a heavier faller reports a larger impulse.  A
 *      constant would let a threshold fire on every collision or none.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/physics/jce_physics.h>

#include <math.h>
#include <string.h>

static float g_max_impulse;

static void on_contact(const JceContactEvent *ev, void *ud)
{
    (void)ud;
    if (!ev) return;
    if (ev->applied_impulse > g_max_impulse)
        g_max_impulse = ev->applied_impulse;
}

/* Drop a box of `mass` onto a static floor and return the largest contact
 * impulse seen. */
static float drop(float mass)
{
    g_max_impulse = 0.0f;

    JcePhysicsWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.gravity.x = 0.0f; wd.gravity.y = -9.81f; wd.gravity.z = 0.0f;
    wd.max_bodies = 16u;
    JcePhysicsWorld *w = jce_physics_create(&wd);
    TEST_ASSERT_NOT_NULL(w);
    jce_physics_set_contact_begin(w, on_contact, NULL);

    JceBodyDesc gd;
    memset(&gd, 0, sizeof gd);
    gd.type = JCE_BODY_STATIC;
    gd.shape = JCE_SHAPE_BOX;
    gd.rotation.w = 1.0f;
    gd.position.y = -0.5f;
    gd.half_extents.x = 20.0f; gd.half_extents.y = 0.5f; gd.half_extents.z = 20.0f;
    gd.collision_group = 0xFFFFFFFFu; gd.collision_mask = 0xFFFFFFFFu;
    (void)jce_physics_body_create(w, &gd);

    JceBodyDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.type = JCE_BODY_DYNAMIC;
    bd.shape = JCE_SHAPE_BOX;
    bd.rotation.w = 1.0f;
    bd.position.y = 5.0f;
    bd.half_extents.x = 0.5f; bd.half_extents.y = 0.5f; bd.half_extents.z = 0.5f;
    bd.mass = mass;
    bd.collision_group = 0xFFFFFFFFu; bd.collision_mask = 0xFFFFFFFFu;
    (void)jce_physics_body_create(w, &bd);

    for (int i = 0; i < 180; ++i)
        jce_physics_step(w, 1.0f / 60.0f);

    jce_physics_destroy(w);
    return g_max_impulse;
}

static void test_a_real_collision_reports_a_real_impulse(void)
{
    float f = drop(1.0f);
    TEST_ASSERT_TRUE_MESSAGE(isfinite(f), "impulse is not finite");
    TEST_ASSERT_TRUE_MESSAGE(f > 0.0f,
        "a box landing on a floor must report a non-zero contact impulse -- "
        "zero means every break_impulse threshold is unreachable");
}

static void test_impulse_tracks_the_hit(void)
{
    float light = drop(1.0f);
    float heavy = drop(20.0f);
    TEST_ASSERT_TRUE(isfinite(light) && isfinite(heavy));
    TEST_ASSERT_TRUE_MESSAGE(heavy > light * 2.0f,
        "a heavier faller must report a larger impulse -- a constant would "
        "make a threshold fire on every collision or on none");
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_real_collision_reports_a_real_impulse);
    RUN_TEST(test_impulse_tracks_the_hit);
    return UNITY_END();
}
