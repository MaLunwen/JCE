#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_camera.h>
#include <jce/renderer/jce_camera.h>

#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static JceTransform camera_transform(jce_vec3 position, jce_quat rotation)
{
    JceTransform transform;

    memset(&transform, 0, sizeof(transform));
    transform.position = position;
    transform.rotation = rotation;
    transform.scale = jce_v3(1.0f, 1.0f, 1.0f);
    return transform;
}

static JceCameraComponent camera_component(bool primary)
{
    JceCameraComponent component;

    memset(&component, 0, sizeof(component));
    component.fov_deg = 20.0f;
    component.near_plane = 0.01f;
    component.far_plane = 1200.0f;
    component.is_primary = primary;
    return component;
}

static void test_resolve_primary_uses_world_pose_and_projection(void)
{
    JceScene *scene = jce_scene_create();
    JceEntity parent = jce_scene_create_entity(scene, "rig");
    JceEntity entity = jce_scene_create_entity(scene, "camera");
    JceTransform parent_transform = camera_transform(
        jce_v3(10.0f, 0.0f, 0.0f),
        jce_q_from_axis_angle(jce_v3(0.0f, 1.0f, 0.0f),
                              90.0f * JCE_DEG2RAD));
    JceTransform local_transform = camera_transform(
        jce_v3(0.0f, 0.0f, 2.0f), jce_q_identity());
    JceCameraComponent component = camera_component(true);
    JceSceneCameraPose pose;

    TEST_ASSERT_NOT_NULL(scene);
    jce_scene_set_transform(scene, parent, &parent_transform);
    jce_scene_set_transform(scene, entity, &local_transform);
    jce_scene_set_parent(scene, entity, parent);
    jce_scene_set_camera(scene, entity, &component);

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_CAMERA_RESOLVE_OK,
        jce_scene_camera_resolve_primary(scene, &pose));
    TEST_ASSERT_EQUAL_UINT64(entity, pose.entity);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 12.0f, pose.position.x);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, pose.position.y);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, pose.position.z);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, -1.0f, pose.forward.x);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, pose.forward.y);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, pose.forward.z);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, pose.up.x);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, pose.up.y);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, pose.up.z);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 20.0f, pose.fov_deg);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.01f, pose.near_plane);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1200.0f, pose.far_plane);
    TEST_ASSERT_FALSE(pose.orthographic);

    jce_scene_destroy(scene);
}

static void test_apply_primary_updates_runtime_camera(void)
{
    JceScene *scene = jce_scene_create();
    JceEntity entity = jce_scene_create_entity(scene, "camera");
    JceTransform transform = camera_transform(
        jce_v3(86.60254f, 50.0f, 0.0f),
        jce_euler_to_q(-30.0f, 90.0f, 0.0f));
    JceCameraComponent component = camera_component(true);
    JceCameraDesc desc;
    JceCamera *camera;

    memset(&desc, 0, sizeof(desc));
    desc.mode = JCE_CAMERA_PERSPECTIVE;
    desc.position = jce_v3(0.0f, 2.0f, 5.0f);
    desc.target = jce_v3(0.0f, 0.0f, 0.0f);
    desc.up = jce_v3(0.0f, 1.0f, 0.0f);
    desc.fov_deg = 60.0f;
    desc.near_plane = 0.1f;
    desc.far_plane = 100.0f;
    camera = jce_camera_create(&desc);
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_NOT_NULL(camera);

    jce_scene_set_transform(scene, entity, &transform);
    jce_scene_set_camera(scene, entity, &component);
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_CAMERA_RESOLVE_OK,
        jce_scene_camera_apply_primary(scene, camera, NULL));

    {
        jce_vec3 position = jce_camera_get_position(camera);
        jce_vec3 forward = jce_camera_get_forward(camera);
        TEST_ASSERT_FLOAT_WITHIN(0.0001f, 86.60254f, position.x);
        TEST_ASSERT_FLOAT_WITHIN(0.0001f, 50.0f, position.y);
        TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, position.z);
        TEST_ASSERT_FLOAT_WITHIN(0.0001f, -0.8660254f, forward.x);
        TEST_ASSERT_FLOAT_WITHIN(0.0001f, -0.5f, forward.y);
        TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, forward.z);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 20.0f,
                             jce_camera_get_fov(camera));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.01f,
                             jce_camera_get_near(camera));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1200.0f,
                             jce_camera_get_far(camera));

    jce_camera_destroy(camera);
    jce_scene_destroy(scene);
}

static void test_apply_primary_preserves_authored_roll(void)
{
    JceScene *scene = jce_scene_create();
    JceEntity entity = jce_scene_create_entity(scene, "rolled_camera");
    JceTransform transform = camera_transform(
        jce_v3(1.0f, 2.0f, 3.0f),
        jce_euler_to_q(-12.0f, 34.0f, 27.0f));
    JceCameraComponent component = camera_component(true);
    JceCameraDesc desc;
    JceSceneCameraPose pose;
    JceCamera *camera;

    memset(&desc, 0, sizeof(desc));
    desc.mode = JCE_CAMERA_PERSPECTIVE;
    desc.position = jce_v3(0.0f, 0.0f, 0.0f);
    desc.target = jce_v3(0.0f, 0.0f, -1.0f);
    desc.up = jce_v3(0.0f, 1.0f, 0.0f);
    desc.fov_deg = 60.0f;
    desc.near_plane = 0.1f;
    desc.far_plane = 100.0f;
    camera = jce_camera_create(&desc);

    jce_scene_set_transform(scene, entity, &transform);
    jce_scene_set_camera(scene, entity, &component);
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_CAMERA_RESOLVE_OK,
        jce_scene_camera_apply_primary(scene, camera, &pose));
    {
        jce_vec3 forward = jce_camera_get_forward(camera);
        jce_vec3 up = jce_camera_get_up(camera);
        TEST_ASSERT_FLOAT_WITHIN(0.0001f, pose.forward.x, forward.x);
        TEST_ASSERT_FLOAT_WITHIN(0.0001f, pose.forward.y, forward.y);
        TEST_ASSERT_FLOAT_WITHIN(0.0001f, pose.forward.z, forward.z);
        TEST_ASSERT_FLOAT_WITHIN(0.0001f, pose.up.x, up.x);
        TEST_ASSERT_FLOAT_WITHIN(0.0001f, pose.up.y, up.y);
        TEST_ASSERT_FLOAT_WITHIN(0.0001f, pose.up.z, up.z);
    }

    jce_camera_destroy(camera);
    jce_scene_destroy(scene);
}

static void test_resolve_primary_rejects_missing_and_ambiguous_scenes(void)
{
    JceScene *scene = jce_scene_create();
    JceEntity first = jce_scene_create_entity(scene, "first");
    JceEntity second = jce_scene_create_entity(scene, "second");
    JceCameraComponent component = camera_component(true);
    JceSceneCameraPose pose;

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_CAMERA_RESOLVE_NOT_FOUND,
        jce_scene_camera_resolve_primary(scene, &pose));
    jce_scene_set_camera(scene, first, &component);
    jce_scene_set_camera(scene, second, &component);
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_CAMERA_RESOLVE_AMBIGUOUS,
        jce_scene_camera_resolve_primary(scene, &pose));

    jce_scene_set_component_enabled(scene, second, JCE_COMP_FLAG_CAMERA,
                                    false);
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_CAMERA_RESOLVE_OK,
        jce_scene_camera_resolve_primary(scene, &pose));
    TEST_ASSERT_EQUAL_UINT64(first, pose.entity);

    jce_scene_destroy(scene);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_resolve_primary_uses_world_pose_and_projection);
    RUN_TEST(test_apply_primary_updates_runtime_camera);
    RUN_TEST(test_apply_primary_preserves_authored_roll);
    RUN_TEST(test_resolve_primary_rejects_missing_and_ambiguous_scenes);
    return UNITY_END();
}
