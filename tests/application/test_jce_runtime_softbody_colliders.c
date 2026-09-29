/*
 * test_jce_runtime_softbody_colliders.c
 *
 * The RUNTIME half of soft-body collision: does a sphere or capsule collider
 * authored in a scene actually reach the soft world?
 *
 * WHY THIS EXISTS SEPARATELY FROM tests/middleware/physics/test_jce_softbody.c:
 * that file calls jce_softbody_add_static_sphere directly, so it proves the
 * PROXY works and says nothing about whether anything ever creates one from a
 * scene.  A mutation removing the sphere/capsule branches from
 * rt_mirror_static_collider survived that file completely -- the API was
 * covered and the wiring was not, which is the shape this repo keeps finding:
 * a capability that is authored, stored, and read by nobody.
 *
 * HOW IT OBSERVES THE PROXIES WITHOUT A NEW ACCESSOR.  jce_softbody_add_static_*
 * returns the proxy's INDEX, assigned from a count that only grows until
 * jce_softbody_clear_statics().  So adding one proxy after the runtime has
 * spawned reports how many the runtime made: 0 means it mirrored nothing.
 * That is an existing public function answering the question, rather than a
 * count accessor added to the API so a test could watch.
 *
 * IN THE TREE: tests/ is tracked on this branch and gitignored on `main`.
 * Settle it with `git check-ignore -v <path>`, never from memory.
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/physics/jce_softbody.h>

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static JceEntity add_collider_entity(JceScene *s, const char *name, float y)
{
    JceEntity e = jce_scene_create_entity(s, name);
    JceTransform *tf = jce_scene_get_transform(s, e);
    TEST_ASSERT_NOT_NULL(tf);
    tf->position   = jce_v3(0.0f, y, 0.0f);
    tf->rotation.x = tf->rotation.y = tf->rotation.z = 0.0f;
    tf->rotation.w = 1.0f;
    tf->scale      = jce_v3(1.0f, 1.0f, 1.0f);
    return e;
}

/* A scene with ONE soft body (the mirror runs on the first one) plus the three
 * primitive colliders, each on its own static entity. */
static JceScene *make_scene(bool with_sphere, bool with_capsule)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity box_e = add_collider_entity(s, "BoxGround", -1.0f);
    JceBoxColliderComponent box;
    memset(&box, 0, sizeof box);
    box.size[0] = 10.0f; box.size[1] = 1.0f; box.size[2] = 10.0f;
    jce_scene_set_box_collider(s, box_e, &box);

    if (with_sphere) {
        JceEntity e = add_collider_entity(s, "SphereRock", -3.0f);
        JceSphereColliderComponent sp;
        memset(&sp, 0, sizeof sp);
        sp.radius = 2.0f;
        jce_scene_set_sphere_collider(s, e, &sp);
    }
    if (with_capsule) {
        JceEntity e = add_collider_entity(s, "CapsuleLog", -6.0f);
        JceCapsuleColliderComponent cp;
        memset(&cp, 0, sizeof cp);
        cp.radius = 1.0f;
        cp.height = 6.0f;
        cp.axis   = 0;               /* X */
        jce_scene_set_capsule_collider(s, e, &cp);
    }

    JceEntity soft_e = add_collider_entity(s, "Blob", 4.0f);
    JceSoftBodyComponent sb;
    memset(&sb, 0, sizeof sb);
    sb.enabled          = true;      /* default is OFF -> inert */
    sb.radius[0] = sb.radius[1] = sb.radius[2] = 0.5f;
    sb.mass             = 2.0f;
    sb.pressure         = 100.0f;
    sb.stiffness_linear = 0.4f;
    sb.stiffness_volume = 0.4f;
    sb.damping          = 0.02f;
    sb.friction         = 0.5f;
    sb.resolution       = 64;
    jce_scene_set_soft_body(s, soft_e, &sb);
    return s;
}

/* Spawn the runtime over `s` and report how many static proxies it mirrored. */
static uint32_t mirrored_count(JceScene *s)
{
    JceRuntimeDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.scene          = s;
    rd.pak            = NULL;
    rd.enable_physics = true;

    JceRuntime *rt = jce_runtime_create(&rd);
    TEST_ASSERT_NOT_NULL(rt);
    jce_runtime_step(rt, 1.0f / 60.0f);

    /* The next index IS the count of what the runtime already made. */
    uint32_t next = jce_softbody_add_static_box(jce_v3(0.0f, -100.0f, 0.0f),
                                                jce_v3(1.0f, 1.0f, 1.0f));
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(
        UINT32_MAX, next,
        "could not add a probe proxy, so the count below means nothing");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
    jce_softbody_clear_statics();
    return next;
}

/* POSITIVE CONTROL FIRST, and it is the one that makes the others readable: a
 * scene with ONLY a box collider mirrors exactly one.  If this is not 1 the
 * runtime is not mirroring at all, or is mirroring things this test does not
 * know about, and every count below is uninterpretable. */
static void test_a_box_only_scene_mirrors_exactly_one(void)
{
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, mirrored_count(make_scene(false, false)),
        "a scene with one box collider and one soft body did not mirror "
        "exactly one static proxy -- the baseline this file counts against "
        "is wrong, so the sphere and capsule cases prove nothing");
}

static void test_a_sphere_collider_reaches_the_soft_world(void)
{
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        2u, mirrored_count(make_scene(true, false)),
        "a SPHERE collider in the scene was not mirrored into the soft world "
        "-- cloth falls through it and nothing reports that, which is the "
        "defect this row was opened for");
}

static void test_a_capsule_collider_reaches_the_soft_world(void)
{
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        2u, mirrored_count(make_scene(false, true)),
        "a CAPSULE collider in the scene was not mirrored into the soft world");
}

static void test_all_three_primitives_are_mirrored_together(void)
{
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        3u, mirrored_count(make_scene(true, true)),
        "box + sphere + capsule did not produce three proxies -- one shape is "
        "being dropped, or one is being mirrored twice");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_box_only_scene_mirrors_exactly_one);
    RUN_TEST(test_a_sphere_collider_reaches_the_soft_world);
    RUN_TEST(test_a_capsule_collider_reaches_the_soft_world);
    RUN_TEST(test_all_three_primitives_are_mirrored_together);
    return UNITY_END();
}
