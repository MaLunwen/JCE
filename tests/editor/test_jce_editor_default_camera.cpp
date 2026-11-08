/* Actual helper consumed by both default-scene builders; real ECS/JSON/resolve. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <initializer_list>
#ifndef JCE_EDITOR_DEFAULT_CAMERA_HEADER
#define JCE_EDITOR_DEFAULT_CAMERA_HEADER "core/jce_editor_default_camera.h"
#endif
#include JCE_EDITOR_DEFAULT_CAMERA_HEADER
#include <jce/middleware/scene/jce_scene_camera.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>

namespace {
struct Scene {
    JceScene *scene = jce_scene_create();
    Scene() { REQUIRE(scene != nullptr); }
    ~Scene() { jce_scene_destroy(scene); }
};
JceEntity add_camera(JceScene *scene, const char *name)
{
    const JceEntity entity = jce_scene_create_entity(scene, name);
    REQUIRE(entity != 0);
    JceTransform transform{};
    transform.position = jce_v3(0, 5, 10);
    transform.rotation = jce_q_identity();
    transform.scale = jce_v3(1, 1, 1);
    jce_scene_set_transform(scene, entity, &transform);
    JceCameraComponent camera{};
    camera.fov_deg = 60; camera.near_plane = .1f; camera.far_plane = 1000;
    jce_scene_set_camera(scene, entity, &camera);
    return entity;
}
}
TEST_CASE("3D default camera is the sole resolved primary without changing secondary cameras")
{
    Scene owner;
    const auto primary = add_camera(owner.scene, "Main Camera");
    const auto secondary = add_camera(owner.scene, "Additional Camera");
    REQUIRE(jce_editor_seed_default_camera(owner.scene, primary, false));
    JceSceneCameraPose pose{};
    REQUIRE(jce_scene_camera_resolve_primary(owner.scene, &pose) == JCE_SCENE_CAMERA_RESOLVE_OK);
    CHECK(pose.entity == primary);
    CHECK_FALSE(pose.orthographic);
    CHECK_FALSE(jce_scene_get_camera(owner.scene, secondary)->is_primary);
    CHECK(pose.position.y == doctest::Approx(5));
}
TEST_CASE("2D default camera resolves an orthographic front-facing pose")
{
    Scene owner;
    const auto entity = add_camera(owner.scene, "Main Camera");
    REQUIRE(jce_editor_seed_default_camera(owner.scene, entity, true));
    JceSceneCameraPose pose{};
    REQUIRE(jce_scene_camera_resolve_primary(owner.scene, &pose) == JCE_SCENE_CAMERA_RESOLVE_OK);
    CHECK(pose.orthographic);
    CHECK(pose.ortho_size == doctest::Approx(5));
    CHECK(pose.position.y == doctest::Approx(0));
    CHECK(pose.position.z == doctest::Approx(10));
}
TEST_CASE("primary and projection survive actual whole-scene JSON serialize-reload")
{
    for (bool is_2d : {false, true}) {
        Scene source, restored;
        const auto entity = add_camera(source.scene, "Main Camera");
        REQUIRE(jce_editor_seed_default_camera(source.scene, entity, is_2d));
        JceJson *json = jce_scene_save_json(source.scene);
        REQUIRE(json != nullptr);
        char *text = jce_json_print(json, false);
        REQUIRE(text != nullptr);
        JceJson *parsed = jce_json_parse_strict(text, 0);
        REQUIRE(parsed != nullptr);
        CHECK(jce_scene_load_json(restored.scene, parsed) == 1);
        jce_json_free(parsed); jce_json_free_string(text); jce_json_free(json);
        JceSceneCameraPose pose{};
        REQUIRE(jce_scene_camera_resolve_primary(restored.scene, &pose) == JCE_SCENE_CAMERA_RESOLVE_OK);
        CHECK(pose.orthographic == is_2d);
        if (is_2d) CHECK(pose.ortho_size == doctest::Approx(5));
    }
}
TEST_CASE("default-camera helper refuses incomplete owners")
{
    Scene owner;
    const auto entity = jce_scene_create_entity(owner.scene, "No camera");
    CHECK_FALSE(jce_editor_seed_default_camera(nullptr, entity, false));
    CHECK_FALSE(jce_editor_seed_default_camera(owner.scene, 0, false));
    CHECK_FALSE(jce_editor_seed_default_camera(owner.scene, entity, false));
    JceSceneCameraPose pose{};
    CHECK(jce_scene_camera_resolve_primary(owner.scene, &pose) == JCE_SCENE_CAMERA_RESOLVE_NOT_FOUND);
}
