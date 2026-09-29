/*
 * test_jce_rb_shape_type.c — JceRigidBodyComponent.shape_type.
 *
 * shape_type is documented as "JceShapeType enum value" and was read by
 * nothing.  Both spawn paths hard-coded a BOX for a body with no collider
 * component:
 *
 *     } else {
 *         // No collider authored -- placeholder box from transform scale
 *         bd.shape = JCE_SHAPE_BOX;
 *
 * so a ball with no SphereCollider collided as a cube while the one field
 * named for the shape sat next to it saying "sphere".
 *
 * The observable is a RAYCAST, not a resting height.  Resting height cannot
 * tell these apart on purpose: all three placeholders are derived to span the
 * same transform-scaled box, so their vertical extents are identical by
 * construction.  What differs is the silhouette, and a ray reads that directly.
 *
 * With scale (1, 3, 1) -- half extents (0.5, 1.5, 0.5) -- the three shapes are
 * a 1x3x1 box, a sphere of radius 1.5, and a Y-capsule of radius 0.5 spanning
 * y in [-1.5, 1.5].  Two probes fired along -Z separate all three:
 *
 *            probe            box     sphere   capsule
 *     A  (x=0.45, y=1.45)     HIT      miss     miss     |x|,|y| inside the box;
 *                                                        r=1.518 > 1.5; 0.636 > 0.5
 *     B  (x=0.90, y=0.00)     miss     HIT      miss     0.9 > 0.5; r=0.9 < 1.5
 *     C  (x=0.45, y=0.00)     HIT      HIT      HIT      proves the body exists
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/application/jce_runtime.h>
#include <jce/middleware/physics/jce_physics.h>
#include <jce/middleware/physics/jce_physics2d.h>
#include <jce/middleware/scene/jce_scene.h>

#include <stdbool.h>
#include <string.h>

typedef struct { bool a, b, c; } Probes;

/* Spawn a kinematic body with NO collider component and probe its silhouette.
 * Kinematic so it stays exactly where it was authored -- a dynamic body would
 * fall out from under the rays before the first step finished. */
static Probes probe_placeholder(uint8_t shape_type)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity e = jce_scene_create_entity(s, "Body");
    JceTransform tf;
    memset(&tf, 0, sizeof tf);
    tf.rotation.w = 1.0f;
    tf.scale = jce_v3(1.0f, 3.0f, 1.0f);
    jce_scene_set_transform(s, e, &tf);

    JceRigidBodyComponent rb;
    memset(&rb, 0, sizeof rb);
    rb.shape_type   = shape_type;
    rb.is_kinematic = true;
    rb.mass         = 1.0f;
    rb.friction     = 0.5f;
    jce_scene_set_rigidbody(s, e, &rb);
    /* No BoxCollider / SphereCollider / CapsuleCollider: this is the whole
     * point.  A collider, when present, still wins outright. */

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = true;
    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);
    jce_runtime_step(rt, 1.0f / 60.0f);

    JcePhysicsWorld *w = jce_runtime_physics(rt);
    TEST_ASSERT_NOT_NULL_MESSAGE(w,
        "no physics world means every probe below reads 'miss' for the same "
        "reason and the test proves nothing");

    Probes p;
    const jce_vec3 dir = { 0.0f, 0.0f, -1.0f };
    JceRaycastResult r;
    r = jce_physics_raycast(w, jce_v3(0.45f, 1.45f, 10.0f), dir, 30.0f);
    p.a = r.hit;
    r = jce_physics_raycast(w, jce_v3(0.90f, 0.00f, 10.0f), dir, 30.0f);
    p.b = r.hit;
    r = jce_physics_raycast(w, jce_v3(0.45f, 0.00f, 10.0f), dir, 30.0f);
    p.c = r.hit;

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
    return p;
}

static void test_default_shape_type_is_still_a_box(void)
{
    /* 0 == JCE_SHAPE_BOX is what a zeroed component and every scene authored
     * before this change carries.  If this stops being a box, wiring the field
     * silently reshaped every existing collider-less body. */
    Probes p = probe_placeholder(0);
    TEST_ASSERT_TRUE_MESSAGE(p.c, "the body must exist at all");
    TEST_ASSERT_TRUE_MESSAGE(p.a,
        "a box fills its corners: probe A must hit");
    TEST_ASSERT_FALSE_MESSAGE(p.b,
        "a 1-wide box must not reach x = 0.9");
}

static void test_sphere_shape_type_gives_a_sphere(void)
{
    Probes p = probe_placeholder((uint8_t)JCE_SHAPE_SPHERE);
    TEST_ASSERT_TRUE_MESSAGE(p.c, "the body must exist at all");
    TEST_ASSERT_FALSE_MESSAGE(p.a,
        "a sphere has no corners: probe A (r = 1.518 > 1.5) must miss");
    TEST_ASSERT_TRUE_MESSAGE(p.b,
        "the placeholder sphere's radius is the LARGEST half extent, so it "
        "contains the box the old code built rather than rattling inside it -- "
        "probe B at x = 0.9 must hit");
}

static void test_capsule_shape_type_gives_a_capsule(void)
{
    Probes p = probe_placeholder((uint8_t)JCE_SHAPE_CAPSULE);
    TEST_ASSERT_TRUE_MESSAGE(p.c, "the body must exist at all");
    TEST_ASSERT_FALSE_MESSAGE(p.a,
        "a Y-capsule of radius 0.5 does not reach the box's top corner");
    TEST_ASSERT_FALSE_MESSAGE(p.b,
        "and it is not the sphere either: radius 0.5 cannot reach x = 0.9");
}

static void test_an_unrepresentable_shape_falls_back_to_box(void)
{
    /* TRIANGLE_MESH needs authored geometry.  Reaching for it here must give
     * the box, not a body with no shape at all. */
    Probes p = probe_placeholder((uint8_t)JCE_SHAPE_TRIANGLE_MESH);
    TEST_ASSERT_TRUE_MESSAGE(p.a && !p.b,
        "an unrepresentable shape_type must land on the box placeholder");
}

static void test_an_authored_collider_still_wins(void)
{
    /* shape_type is the FALLBACK, not a second source of truth: a body that
     * has a BoxCollider must be a box no matter what shape_type says. */
    JceScene *s = jce_scene_create();
    JceEntity e = jce_scene_create_entity(s, "Body");
    JceTransform tf;
    memset(&tf, 0, sizeof tf);
    tf.rotation.w = 1.0f;
    tf.scale = jce_v3(1.0f, 3.0f, 1.0f);
    jce_scene_set_transform(s, e, &tf);

    JceRigidBodyComponent rb;
    memset(&rb, 0, sizeof rb);
    rb.shape_type   = (uint8_t)JCE_SHAPE_SPHERE;   /* deliberately disagrees */
    rb.is_kinematic = true;
    rb.mass         = 1.0f;
    jce_scene_set_rigidbody(s, e, &rb);

    JceBoxColliderComponent bc;
    memset(&bc, 0, sizeof bc);
    bc.size[0] = 1.0f; bc.size[1] = 1.0f; bc.size[2] = 1.0f;
    jce_scene_set_box_collider(s, e, &bc);

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = true;
    JceRuntime *rt = jce_runtime_create(&desc);
    jce_runtime_step(rt, 1.0f / 60.0f);

    JcePhysicsWorld *w = jce_runtime_physics(rt);
    const jce_vec3 dir = { 0.0f, 0.0f, -1.0f };
    JceRaycastResult a = jce_physics_raycast(w, jce_v3(0.45f, 1.45f, 10.0f), dir, 30.0f);
    JceRaycastResult b = jce_physics_raycast(w, jce_v3(0.90f, 0.00f, 10.0f), dir, 30.0f);

    TEST_ASSERT_TRUE_MESSAGE(a.hit,
        "the BoxCollider must decide: corner probe hits");
    TEST_ASSERT_FALSE_MESSAGE(b.hit,
        "if shape_type could override an authored collider it would be a "
        "second source of truth, and the collider would stop meaning anything");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

/* ── the 2D path, which is a SEPARATE placeholder in a separate function ── */

/* A 2D ray runs in the XY plane, so the 3D probe layout does not carry over:
 * a downward ray only reads the shape's x-extent.  What discriminates here is
 * WHERE it lands.  With scale (1, 3, 1) the three placeholders are a 1x3 box,
 * a circle of radius 1.5, and a Y-capsule of radius 0.5 spanning y in
 * [-1.5, 1.5], so a ray straight down the line x = 0.45 stops at
 *
 *     box      y = 1.5     (flat top edge)
 *     circle   y = sqrt(1.5^2 - 0.45^2)          = 1.431
 *     capsule  y = 1.0 + sqrt(0.5^2 - 0.45^2)    = 1.218   (top cap)
 *
 * and a ray down x = 0.9 misses the box and the capsule but hits the circle. */
typedef struct { bool at_09; float y_at_045; } Probes2D;

static Probes2D probe_placeholder_2d(uint8_t shape_type)
{
    JceScene *s = jce_scene_create();
    JceEntity e = jce_scene_create_entity(s, "Body2D");
    JceTransform tf;
    memset(&tf, 0, sizeof tf);
    tf.rotation.w = 1.0f;
    tf.scale = jce_v3(1.0f, 3.0f, 1.0f);
    jce_scene_set_transform(s, e, &tf);

    JceRigidBody2DComponent rb;
    memset(&rb, 0, sizeof rb);
    rb.shape_type = shape_type;
    rb.body_type  = 1;            /* kinematic: stays where it was authored */
    rb.mass       = 1.0f;
    jce_scene_set_rigidbody2d(s, e, &rb);

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = true;
    JceRuntime *rt = jce_runtime_create(&desc);
    jce_runtime_step(rt, 1.0f / 60.0f);

    JcePhysics2D *w = jce_runtime_physics2d(rt);
    TEST_ASSERT_NOT_NULL_MESSAGE(w,
        "the runtime had no 2D world, so every probe below would read the same "
        "way for the same reason");

    Probes2D p;
    const jce_vec2 down = { 0.0f, -1.0f };
    jce_vec2 o09  = { 0.90f, 10.0f };
    jce_vec2 o045 = { 0.45f, 10.0f };
    JceRaycast2DResult r = jce_physics2d_raycast(w, o09, down, 30.0f);
    p.at_09 = r.hit;
    r = jce_physics2d_raycast(w, o045, down, 30.0f);
    p.y_at_045 = r.hit ? r.point.y : -99.0f;

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
    return p;
}

static void test_2d_default_is_still_a_box(void)
{
    Probes2D p = probe_placeholder_2d(0);
    TEST_ASSERT_FALSE_MESSAGE(p.at_09, "a 1-wide box does not reach x = 0.9");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f, 1.5f, p.y_at_045,
        "a box has a flat top edge at y = 1.5; if this moved, wiring the field "
        "reshaped every existing 2D body");
}

static void test_2d_sphere_shape_type_gives_a_circle(void)
{
    Probes2D p = probe_placeholder_2d((uint8_t)JCE_SHAPE_SPHERE);
    TEST_ASSERT_TRUE_MESSAGE(p.at_09, "a circle of radius 1.5 reaches x = 0.9");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f, 1.431f, p.y_at_045,
        "and it is round: the ray stops below the box's flat top");
}

static void test_2d_capsule_shape_type_gives_a_capsule(void)
{
    Probes2D p = probe_placeholder_2d((uint8_t)JCE_SHAPE_CAPSULE);
    TEST_ASSERT_FALSE_MESSAGE(p.at_09, "radius 0.5 cannot reach x = 0.9");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f, 1.218f, p.y_at_045,
        "the top CAP, not the box edge and not the circle -- all three land at "
        "a different height and only one of them is right");
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_shape_type_is_still_a_box);
    RUN_TEST(test_sphere_shape_type_gives_a_sphere);
    RUN_TEST(test_capsule_shape_type_gives_a_capsule);
    RUN_TEST(test_an_unrepresentable_shape_falls_back_to_box);
    RUN_TEST(test_an_authored_collider_still_wins);
    RUN_TEST(test_2d_default_is_still_a_box);
    RUN_TEST(test_2d_sphere_shape_type_gives_a_circle);
    RUN_TEST(test_2d_capsule_shape_type_gives_a_capsule);
    return UNITY_END();
}
