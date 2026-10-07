/* Real scene save/load must preserve authored rotations, not just close angles. */
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/resource/jce_scene_serial.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_math.h>
#include "unity.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}
static void capture_entity(JceScene *scene, JceEntity entity, void *user)
{
    (void)scene;
    *(JceEntity *)user = entity;
}
static void exact_vec3(jce_vec3 expected, jce_vec3 actual)
{
    TEST_ASSERT_EQUAL_MEMORY(&expected.x, &actual.x, sizeof(float));
    TEST_ASSERT_EQUAL_MEMORY(&expected.y, &actual.y, sizeof(float));
    TEST_ASSERT_EQUAL_MEMORY(&expected.z, &actual.z, sizeof(float));
}
static void repeat_rotation(jce_quat rotation)
{
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    JceEntity entity = jce_scene_create_entity(scene, "rotation");
    JceTransform authored = {0};
    authored.position = jce_v3(1.25f, -7.125f, 8192.5f);
    authored.rotation = rotation;
    authored.scale = jce_v3(.125f, 2.5f, 8.f);
    JcePivotComponent pivot = {0};
    pivot.local_position = jce_v3(.25f, -.5f, 2.f);
    pivot.local_rotation = rotation;
    jce_scene_set_transform(scene, entity, &authored);
    jce_scene_set_pivot(scene, entity, &pivot);
    for (int cycle = 0; cycle < 100; ++cycle) {
        size_t length = 0;
        char *saved = jce_scene_serial_save(scene, &length);
        TEST_ASSERT_NOT_NULL(saved);
        /* Same public clear/load transaction as editor history restoration. */
        TEST_ASSERT_TRUE(jce_scene_clear(scene) >= 0);
        TEST_ASSERT_TRUE(jce_scene_serial_load(scene, saved, length));
        jce_scene_serial_free(saved);
        entity = JCE_ENTITY_INVALID;
        jce_scene_each_entity(scene, capture_entity, &entity);
        TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, entity);
        const JceTransform *actual = jce_scene_get_transform(scene, entity);
        const JcePivotComponent *actual_pivot = jce_scene_get_pivot(scene, entity);
        TEST_ASSERT_NOT_NULL(actual);
        TEST_ASSERT_NOT_NULL(actual_pivot);
        TEST_ASSERT_EQUAL_MEMORY(&authored.rotation, &actual->rotation, sizeof(jce_quat));
        TEST_ASSERT_EQUAL_MEMORY(&pivot.local_rotation, &actual_pivot->local_rotation, sizeof(jce_quat));
        exact_vec3(authored.position, actual->position);
        exact_vec3(authored.scale, actual->scale);
        exact_vec3(pivot.local_position, actual_pivot->local_position);
    }
    jce_scene_destroy(scene);
}
static void hundred_roundtrips_preserve_actual_unit_quaternions(void)
{
    const float degrees[][3] = {{0,0,0},{0,25,0},{0,-180,6},
        {12.5f,89.9f,-46.25f},{179.99f,-179.999f,179.95f}};
    for (unsigned i = 0; i < sizeof(degrees)/sizeof(degrees[0]); ++i)
        repeat_rotation(jce_q_from_euler(degrees[i][0]*JCE_DEG2RAD,
            degrees[i][1]*JCE_DEG2RAD,degrees[i][2]*JCE_DEG2RAD));
    repeat_rotation(jce_q_normalize(jce_v4(.1f,.2f,.3f,.9f)));
    repeat_rotation(jce_v4(0,0,0,-1));
}
static void legacy_euler_input_still_uses_the_existing_orientation(void)
{
    const char *text = "{\"components\":[{\"type\":\"Transform\",\"rotX\":0,\"rotY\":25,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1}]}";
    JceJson *value = jce_json_parse(text, strlen(text));
    TEST_ASSERT_NOT_NULL(value);
    JceScene *scene = jce_scene_create();
    JceEntity entity = jce_scene_create_entity(scene,"legacy");
    jce_scene_parse_entity_json(scene,entity,value);
    jce_quat expected = jce_q_from_euler(0,25*JCE_DEG2RAD,0);
    TEST_ASSERT_NOT_NULL(jce_scene_get_transform(scene,entity));
    TEST_ASSERT_EQUAL_MEMORY(&expected,&jce_scene_get_transform(scene,entity)->rotation,sizeof(expected));
    jce_json_free(value);
    jce_scene_destroy(scene);
}
static void nonunit_and_zero_input_remain_normalized_for_transform_and_pivot(void)
{
    const char *text = "{\"components\":[{\"type\":\"Transform\",\"rotX\":2,\"rotY\":0,\"rotZ\":0,\"rotW\":0},{\"type\":\"Pivot\",\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"rotW\":0}]}";
    JceJson *value = jce_json_parse(text,strlen(text));
    JceScene *scene = jce_scene_create();
    JceEntity entity = jce_scene_create_entity(scene,"normalize");
    jce_scene_parse_entity_json(scene,entity,value);
    jce_quat expected = jce_v4(1,0,0,0), identity = jce_q_identity();
    TEST_ASSERT_NOT_NULL(jce_scene_get_transform(scene,entity));
    TEST_ASSERT_EQUAL_MEMORY(&expected,&jce_scene_get_transform(scene,entity)->rotation,sizeof(expected));
    TEST_ASSERT_NOT_NULL(jce_scene_get_pivot(scene,entity));
    TEST_ASSERT_EQUAL_MEMORY(&identity,&jce_scene_get_pivot(scene,entity)->local_rotation,sizeof(identity));
    jce_json_free(value);
    jce_scene_destroy(scene);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(hundred_roundtrips_preserve_actual_unit_quaternions);
    RUN_TEST(legacy_euler_input_still_uses_the_existing_orientation);
    RUN_TEST(nonunit_and_zero_input_remain_normalized_for_transform_and_pivot);
    return UNITY_END();
}
