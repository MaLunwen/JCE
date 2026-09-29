/*
 * test_jce_collider2d_polygon.c — Collider2D POLYGON is a polygon.
 *
 * JceCollider2DComponent.point_count and points[] were authored, serialised
 * and read by nothing: the 2D wrapper had no polygon shape, so POLYGON fell
 * through to BOX and EDGE degraded to a single segment.  A ramp collided as
 * its bounding box, and nothing said so -- a collider that is too BIG is the
 * failure mode that reads as level design.
 *
 * Box2D has had b2ComputeHull + b2MakePolygon all along.
 *
 * The probe is the corner a box has and a triangle does not: a right triangle
 * on (0,0) (2,0) (0,2) fills the lower-left half of its 2x2 bounding box, so
 * a point near (1.6, 1.6) is inside the BOX and outside the TRIANGLE.
 *
 * Both shapes are asserted at both points, so a build that made every polygon
 * a box fails as loudly as one that made every box a polygon.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/physics/jce_physics2d.h>

#include <stdbool.h>
#include <string.h>

static jce_vec2 v2(float x, float y) { jce_vec2 v; v.x = x; v.y = y; return v; }

/* A static shape covering the 2x2 box (0,0)-(2,2), either as a box or as the
 * lower-left right triangle.  Returns whether a ray fired at `probe` from
 * far below hits it. */
static bool probe_hits(bool as_polygon, float px)
{
    JcePhysics2DDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.gravity.x = 0.0f; wd.gravity.y = 0.0f;
    wd.max_bodies = 8u;
    JcePhysics2D *w = jce_physics2d_create(&wd);
    TEST_ASSERT_NOT_NULL(w);

    static const float tri[3][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f },
                                     { -1.0f,  1.0f } };

    JceBody2DDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.type = JCE_BODY_STATIC;
    bd.position = v2(0.0f, 0.0f);
    bd.half_extents = v2(1.0f, 1.0f);
    if (as_polygon) {
        bd.shape = JCE_SHAPE2D_POLYGON;
        bd.points = &tri[0][0];
        bd.point_count = 3;
    } else {
        bd.shape = JCE_SHAPE2D_BOX;
    }
    TEST_ASSERT_TRUE(jce_body_valid(jce_physics2d_body_create(w, &bd)));

    /* Straight up through x = px, from below the shape. */
    JceRaycast2DResult r =
        jce_physics2d_raycast(w, v2(px, -4.0f), v2(0.0f, 1.0f), 8.0f);
    bool hit = r.hit;
    jce_physics2d_destroy(w);
    return hit;
}

static void test_box_fills_its_bounding_box(void)
{
    /* x = 0.7 is inside the 2x2 box at every height. */
    TEST_ASSERT_TRUE_MESSAGE(probe_hits(false, 0.7f),
        "a BOX must cover its own extents");
    TEST_ASSERT_TRUE_MESSAGE(probe_hits(false, -0.7f),
        "a BOX must cover its own extents on both sides");
}

static void test_polygon_does_not_fill_the_corner(void)
{
    /* The triangle keeps the -x half; a ray up through x = +0.7 clips only
     * its lower edge, and one through x = -0.7 passes through its body. */
    TEST_ASSERT_TRUE_MESSAGE(probe_hits(true, -0.7f),
        "the triangle must be hit where it IS -- if this fails the hull was "
        "rejected and the body has no shape at all");
    TEST_ASSERT_TRUE_MESSAGE(probe_hits(true, 0.7f),
        "the triangle still has a lower edge at x = 0.7");
}

/* The discriminating case: a shape that is a box and a shape that is a
 * triangle cannot both be right about the upper-right corner. */
static void test_box_and_polygon_disagree(void)
{
    JcePhysics2DDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.max_bodies = 8u;

    static const float tri[3][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f },
                                     { -1.0f,  1.0f } };
    bool hit[2];
    for (int poly = 0; poly < 2; ++poly) {
        JcePhysics2D *w = jce_physics2d_create(&wd);
        TEST_ASSERT_NOT_NULL(w);
        JceBody2DDesc bd;
        memset(&bd, 0, sizeof bd);
        bd.type = JCE_BODY_STATIC;
        bd.position = v2(0.0f, 0.0f);
        bd.half_extents = v2(1.0f, 1.0f);
        if (poly) {
            bd.shape = JCE_SHAPE2D_POLYGON;
            bd.points = &tri[0][0];
            bd.point_count = 3;
        } else {
            bd.shape = JCE_SHAPE2D_BOX;
        }
        TEST_ASSERT_TRUE(jce_body_valid(jce_physics2d_body_create(w, &bd)));
        /* At y = 0.6 the BOX spans x in [-1, 1]; the triangle (hypotenuse
         * from (1,-1) to (-1,1), keeping x + y <= 0) spans only [-1, -0.6].
         * A ray from x = 4 leftward that STOPS at x = 0.8 therefore reaches
         * the box and never reaches the triangle.  A full-length ray would
         * eventually enter the triangle too and discriminate nothing -- which
         * is exactly what the first version of this test did. */
        JceRaycast2DResult r =
            jce_physics2d_raycast(w, v2(4.0f, 0.6f), v2(-1.0f, 0.0f), 3.2f);
        hit[poly] = r.hit;
        jce_physics2d_destroy(w);
    }
    TEST_ASSERT_TRUE_MESSAGE(hit[0],
        "the BOX must be hit across its upper-right corner");
    TEST_ASSERT_TRUE_MESSAGE(hit[0] != hit[1],
        "a POLYGON must not collide as its bounding box -- if these agree, "
        "the authored points did not reach the shape");
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_box_fills_its_bounding_box);
    RUN_TEST(test_polygon_does_not_fill_the_corner);
    RUN_TEST(test_box_and_polygon_disagree);
    return UNITY_END();
}
