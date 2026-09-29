/*
 * test_jce_rb_shape_persist.c — JceRigidBodyComponent.shape_type survives a
 * save and a load.
 *
 * 9392b8ba made shape_type decide the shape a body with no collider component
 * gets, and gave it an inspector control.  It missed half the wiring: the 3D
 * parse/serialize pair never touched the field, so a designer's choice reached
 * the running scene and then vanished the moment the scene was written out.
 * The 2D sibling had round-tripped `shapeType` all along, which is what made
 * the gap invisible -- the same key was right there, one function down.
 *
 * check_component_serializer_roundtrip.py cannot see this class: it pairs JSON
 * KEYS between parse and serialize, and here there was no key at all.
 *
 * The observable is the SHAPE after a round trip, not the field value.  Reading
 * the field back only proves the JSON carried a number; the raycast proves the
 * number still decides what the physics world builds.  Probes are the ones from
 * test_jce_rb_shape_type.c, scale (1,3,1):
 *
 *          probe A (x=0.45, y=1.45)   probe B (x=0.90, y=0)
 *   box            HIT                       miss
 *   sphere         miss                      HIT
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/application/jce_runtime.h>
#include <jce/middleware/physics/jce_physics.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>

#include <stdbool.h>
#include <string.h>

typedef struct { bool a, b; uint8_t field; } Probe;

static void find_body_cb(JceScene *s, JceEntity e, void *ud)
{
    JceEntity *out = (JceEntity *)ud;
    if (*out == JCE_ENTITY_INVALID && jce_scene_get_rigidbody(s, e)) *out = e;
}

/* Author a collider-less kinematic body with `shape_type`, save the whole
 * scene to JSON, load it into a FRESH scene, and probe what that scene's
 * physics world actually built. */
static Probe roundtrip_probe(uint8_t shape_type)
{
    JceScene *src = jce_scene_create();
    TEST_ASSERT_NOT_NULL(src);

    JceEntity e = jce_scene_create_entity(src, "Body");
    JceTransform tf;
    memset(&tf, 0, sizeof tf);
    tf.rotation.w = 1.0f;
    tf.scale = jce_v3(1.0f, 3.0f, 1.0f);
    jce_scene_set_transform(src, e, &tf);

    JceRigidBodyComponent rb;
    memset(&rb, 0, sizeof rb);
    rb.shape_type   = shape_type;
    rb.is_kinematic = true;
    rb.mass         = 1.0f;
    jce_scene_set_rigidbody(src, e, &rb);

    JceJson *doc = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL_MESSAGE(doc, "the scene must serialise at all");
    jce_scene_destroy(src);

    JceScene *dst = jce_scene_create();
    TEST_ASSERT_NOT_NULL(dst);
    int loaded = jce_scene_load_json(dst, doc);
    jce_json_free(doc);
    TEST_ASSERT_TRUE_MESSAGE(loaded > 0, "the saved scene must load back");

    /* Find the reloaded body: entity ids are not promised to survive. */
    JceEntity re = JCE_ENTITY_INVALID;
    jce_scene_each_entity(dst, find_body_cb, &re);
    TEST_ASSERT_TRUE_MESSAGE(re != JCE_ENTITY_INVALID,
        "the reloaded scene must still carry a Rigidbody");

    JceRigidBodyComponent *out = jce_scene_get_rigidbody(dst, re);

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene          = dst;
    desc.enable_physics = true;
    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);
    jce_runtime_step(rt, 1.0f / 60.0f);

    JcePhysicsWorld *w = jce_runtime_physics(rt);
    TEST_ASSERT_NOT_NULL(w);

    Probe p;
    p.field = out ? out->shape_type : 255;
    const jce_vec3 dir = { 0.0f, 0.0f, -1.0f };
    p.a = jce_physics_raycast(w, jce_v3(0.45f, 1.45f, 10.0f), dir, 30.0f).hit;
    p.b = jce_physics_raycast(w, jce_v3(0.90f, 0.00f, 10.0f), dir, 30.0f).hit;

    jce_runtime_destroy(rt);
    jce_scene_destroy(dst);
    return p;
}

static void test_box_survives_the_round_trip(void)
{
    /* 0 == JCE_SHAPE_BOX is what every scene written before this carries, and
     * what an omitted key must still mean. */
    Probe p = roundtrip_probe(0);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, p.field, "the default must come back 0");
    TEST_ASSERT_TRUE_MESSAGE(p.a, "a box fills its corners");
    TEST_ASSERT_FALSE_MESSAGE(p.b, "a 1-wide box cannot reach x = 0.9");
}

static void test_sphere_survives_the_round_trip(void)
{
    Probe p = roundtrip_probe((uint8_t)JCE_SHAPE_SPHERE);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)JCE_SHAPE_SPHERE, p.field,
        "the authored shape must come back out of the file -- before this the "
        "3D parse/serialize pair never touched the field and it came back 0");
    TEST_ASSERT_FALSE_MESSAGE(p.a,
        "and it must still DECIDE: a sphere has no corners");
    TEST_ASSERT_TRUE_MESSAGE(p.b,
        "reading the field back only proves the JSON carried a number; this "
        "proves the number still builds the shape");
}

static void test_capsule_survives_the_round_trip(void)
{
    Probe p = roundtrip_probe((uint8_t)JCE_SHAPE_CAPSULE);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)JCE_SHAPE_CAPSULE, p.field,
        "capsule must come back out of the file");
    TEST_ASSERT_FALSE_MESSAGE(p.a, "a Y-capsule misses the box's top corner");
    TEST_ASSERT_FALSE_MESSAGE(p.b, "and radius 0.5 cannot reach x = 0.9");
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_box_survives_the_round_trip);
    RUN_TEST(test_sphere_survives_the_round_trip);
    RUN_TEST(test_capsule_survives_the_round_trip);
    return UNITY_END();
}
