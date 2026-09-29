/*
 * test_jce_capsule3d_axis.c — CapsuleCollider.axis reaches the simulated shape.
 *
 * Bullet has btCapsuleShapeX / btCapsuleShape / btCapsuleShapeZ; only the Y
 * class was reachable, so a capsule authored along X simulated as a vertical
 * one.  The runtime said so in a comment ("`axis` is intentionally ignored")
 * and the editor overlay drew Y-aligned to match, which made the pair
 * consistent and the feature absent.
 *
 * Shapes are not readable from the API, so this asks the world: a ray that
 * can only hit one orientation.  Capsule radius 0.25, half-length 1.0, at the
 * origin.
 *
 *   Y-aligned -> extends to y = +-1.25, x only to +-0.25
 *   X-aligned -> the transpose
 *
 * Both axes are asserted under both probes, so a build that made every
 * capsule X-aligned fails as loudly as one that made none.  Z is checked too:
 * the enum has a fourth value and an off-by-one in the mapping would send Z
 * to X without either of the first two probes noticing.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/physics/jce_physics.h>

#include <math.h>
#include <stdbool.h>
#include <string.h>

static JcePhysicsWorld *make_world(void)
{
    JcePhysicsWorldDesc wd;
    memset(&wd, 0, sizeof(wd));
    wd.gravity.x = 0.0f; wd.gravity.y = 0.0f; wd.gravity.z = 0.0f;
    wd.max_bodies = 8u;
    return jce_physics_create(&wd);
}

static jce_vec3 v3(float x, float y, float z)
{
    jce_vec3 v; v.x = x; v.y = y; v.z = z; return v;
}

/* Static capsule on `axis` at the origin; ray from `from` toward `dir`.
 * Returns whether anything was hit. */
static bool probe(uint8_t axis, jce_vec3 from, jce_vec3 dir)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    JceBodyDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.type = JCE_BODY_STATIC;
    bd.shape = JCE_SHAPE_CAPSULE;
    bd.rotation.w = 1.0f;
    bd.half_extents.x = 0.25f;   /* radius            */
    bd.half_extents.y = 1.0f;    /* cylinder half-len */
    bd.collision_group = 0xFFFFFFFFu;
    bd.collision_mask  = 0xFFFFFFFFu;
    bd.capsule_axis = axis;
    (void)jce_physics_body_create(w, &bd);

    JceRaycastResult r;
    memset(&r, 0, sizeof(r));
    r = jce_physics_raycast(w, from, dir, 10.0f);
    bool hit = r.hit;
    jce_physics_destroy(w);
    return hit;
}

/* A point 0.7 along an axis is inside the capsule only when that axis is the
 * LONG one (half-length 1.0 + radius), never when it is a short one (0.25). */
static bool reaches_x(uint8_t axis) { return probe(axis, v3(0.7f, -3.0f, 0.0f), v3(0.0f, 1.0f, 0.0f)); }
static bool reaches_y(uint8_t axis) { return probe(axis, v3(-3.0f, 0.7f, 0.0f), v3(1.0f, 0.0f, 0.0f)); }
static bool reaches_z(uint8_t axis) { return probe(axis, v3(-3.0f, 0.0f, 0.7f), v3(1.0f, 0.0f, 0.0f)); }

static void test_default_is_still_y(void)
{
    /* Zero is what every desc built before this field existed carried. */
    TEST_ASSERT_TRUE_MESSAGE(reaches_y(JCE_CAPSULE_AXIS_DEFAULT),
        "a DEFAULT (zero) capsule must still be Y-aligned -- 0 meaning X "
        "would silently re-orient every capsule in every existing scene");
    TEST_ASSERT_FALSE(reaches_x(JCE_CAPSULE_AXIS_DEFAULT));
    TEST_ASSERT_FALSE(reaches_z(JCE_CAPSULE_AXIS_DEFAULT));
}

static void test_x_axis(void)
{
    TEST_ASSERT_TRUE_MESSAGE(reaches_x(JCE_CAPSULE_AXIS_X),
        "an X-axis capsule must extend to x = 0.7 -- if this fails the "
        "wrapper is still building every capsule Y-aligned");
    TEST_ASSERT_FALSE(reaches_y(JCE_CAPSULE_AXIS_X));
    TEST_ASSERT_FALSE(reaches_z(JCE_CAPSULE_AXIS_X));
}

static void test_y_axis(void)
{
    TEST_ASSERT_TRUE(reaches_y(JCE_CAPSULE_AXIS_Y));
    TEST_ASSERT_FALSE(reaches_x(JCE_CAPSULE_AXIS_Y));
    TEST_ASSERT_FALSE(reaches_z(JCE_CAPSULE_AXIS_Y));
}

static void test_z_axis(void)
{
    TEST_ASSERT_TRUE_MESSAGE(reaches_z(JCE_CAPSULE_AXIS_Z),
        "a Z-axis capsule must extend to z = 0.7");
    TEST_ASSERT_FALSE(reaches_x(JCE_CAPSULE_AXIS_Z));
    TEST_ASSERT_FALSE(reaches_y(JCE_CAPSULE_AXIS_Z));
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_is_still_y);
    RUN_TEST(test_x_axis);
    RUN_TEST(test_y_axis);
    RUN_TEST(test_z_axis);
    return UNITY_END();
}
