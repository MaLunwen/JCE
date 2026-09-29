/*
 * test_jce_capsule2d_direction.c — Collider2D.capsule_direction reaches the
 * simulated shape, not just the file.
 *
 * capsule_direction (0 = length along Y, 1 = length along X) was authored,
 * serialised and shown in the Inspector, and the runtime ignored it: every 2D
 * capsule was built vertical.  The editor gizmo said so in a COMMENT --
 * "the capsule is always vertical (the runtime ignores capsule_direction)" --
 * which made the drawing honest and the feature absent.
 *
 * A shape's axis is not observable from the component, so this asks Box2D
 * directly, with a ray that can only hit one orientation:
 *
 *      capsule: radius 0.25, half-length 1.0, centred at the origin
 *      probe:   a ray along +Y at x = +0.7
 *
 *   vertical  (extent x in [-0.25, +0.25]) -> the ray at x = 0.7 MISSES
 *   horizontal(extent x in [-1.25, +1.25]) -> it HITS
 *
 * and the transposed probe (a ray along +X at y = +0.7) must give the
 * mirror-image answer, so a shape that is somehow fat in both directions
 * cannot pass.  Both orientations are asserted in both probes: an assertion
 * that only ever checks the new behaviour would still pass if the wrapper
 * started building everything horizontal.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/physics/jce_physics2d.h>

#include <math.h>
#include <string.h>
#include <stdbool.h>

static jce_vec2 ray_o(float x, float y) { jce_vec2 v; v.x = x; v.y = y; return v; }
static jce_vec2 ray_d(float x, float y) { jce_vec2 v; v.x = x; v.y = y; return v; }

static JcePhysics2D *make_world(void)
{
    JcePhysics2DDesc wd;
    memset(&wd, 0, sizeof(wd));
    wd.gravity.x  = 0.0f;
    wd.gravity.y  = 0.0f;
    wd.max_bodies = 16u;
    return jce_physics2d_create(&wd);
}

static JceBodyHandle add_capsule(JcePhysics2D *w, bool horizontal)
{
    JceBody2DDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.type           = JCE_BODY_STATIC;
    bd.shape          = horizontal ? JCE_SHAPE2D_CAPSULE_X : JCE_SHAPE2D_CAPSULE;
    bd.position.x     = 0.0f;
    bd.position.y     = 0.0f;
    bd.angle          = 0.0f;
    bd.half_extents.x = 0.25f;   /* radius      */
    bd.half_extents.y = 1.0f;    /* half length */
    return jce_physics2d_body_create(w, &bd);
}

/* Ray along +Y at x = OFFSET, starting below the shape. */
static bool probe_y(bool horizontal)
{
    JcePhysics2D *w = make_world();
    TEST_ASSERT_NOT_NULL(w);
    (void)add_capsule(w, horizontal);
    JceRaycast2DResult r = jce_physics2d_raycast(
        w, ray_o(0.7f, -3.0f), ray_d(0.0f, 1.0f), 6.0f);
    bool hit = r.hit;
    jce_physics2d_destroy(w);
    return hit;
}

/* Ray along +X at y = OFFSET, starting left of the shape. */
static bool probe_x(bool horizontal)
{
    JcePhysics2D *w = make_world();
    TEST_ASSERT_NOT_NULL(w);
    (void)add_capsule(w, horizontal);
    JceRaycast2DResult r = jce_physics2d_raycast(
        w, ray_o(-3.0f, 0.7f), ray_d(1.0f, 0.0f), 6.0f);
    bool hit = r.hit;
    jce_physics2d_destroy(w);
    return hit;
}

static void test_vertical_is_thin_in_x(void)
{
    /* x = 0.7 is outside a vertical capsule's 0.25 radius. */
    TEST_ASSERT_FALSE_MESSAGE(probe_y(false),
        "a VERTICAL capsule must not extend to x = 0.7");
    /* ...and y = 0.7 is inside its 1.0 half-length. */
    TEST_ASSERT_TRUE_MESSAGE(probe_x(false),
        "a VERTICAL capsule must extend to y = 0.7");
}

static void test_horizontal_is_thin_in_y(void)
{
    TEST_ASSERT_TRUE_MESSAGE(probe_y(true),
        "a HORIZONTAL capsule must extend to x = 0.7 -- if this fails the "
        "wrapper is still building every capsule vertical");
    TEST_ASSERT_FALSE_MESSAGE(probe_x(true),
        "a HORIZONTAL capsule must not extend to y = 0.7");
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_vertical_is_thin_in_x);
    RUN_TEST(test_horizontal_is_thin_in_y);
    return UNITY_END();
}
