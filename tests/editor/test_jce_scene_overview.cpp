// Complete actual Overview/focus/camera bodies with real Scene and CPU Camera.
// Unloaded mesh bounds only: the typed asset-cache port always returns NULL.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "scene/jce_scene_render_internal.h"
#include <jce/middleware/scene/jce_component_registry.h>
#include <cmath>
#include <cstring>
#include <limits>

SceneRenderState s_sr{};
static JceScene *provided_scene;
static unsigned mesh_requests, persist_requests;

// Editor borrowers only; entity identity and enable data stay in the real ECS.
JceScene *jce_state_get_scene(void) { return provided_scene; }
bool jce_state_entity_exists(uint32_t id)
{
    return jce_scene_entity_from_index(provided_scene, id) != 0;
}
bool jce_state_entity_enabled(uint32_t id)
{
    const JceEntity e = jce_scene_entity_from_index(provided_scene, id);
    JceEditorMeta *meta = e ? jce_scene_get_editor_meta(provided_scene, e) : nullptr;
    return meta && meta->enabled;
}
JceMesh *get_cached_mesh(const char *path, const float *world_position)
{
    CHECK(path != nullptr);
    CHECK(world_position != nullptr);
    ++mesh_requests;
    return nullptr;
}
static void orbit_persist_pose(void) { ++persist_requests; }

#include "overview_constants.inc.h"
#include "overview_orbit.inc.h"
#include "overview_cancel.inc.h"
#include "overview_reset.inc.h"
#include JCE_TEST_SCENE_OVERVIEW_BOUNDS_OWNER
#include "overview_owner.inc.h"
#include "overview_update.inc.h"

namespace {
struct Fixture {
    JceScene *scene = jce_scene_create();
    JceCamera *camera = nullptr;
    Fixture()
    {
        REQUIRE(scene != nullptr);
        JceCameraDesc desc{};
        desc.mode = JCE_CAMERA_PERSPECTIVE;
        desc.position = jce_v3(8, 6, 8);
        desc.target = jce_v3(0, 0, 0);
        desc.up = jce_v3(0, 1, 0);
        desc.fov_deg = 60;
        desc.near_plane = .01f;
        desc.far_plane = 1000;
        camera = jce_camera_create(&desc);
        REQUIRE(camera != nullptr);
        provided_scene = scene;
        s_sr = {};
        s_sr.camera = camera;
        s_sr.initialized = true;
        jce_editor_scene_camera_reset();
        mesh_requests = persist_requests = 0;
    }
    ~Fixture()
    {
        s_sr = {};
        provided_scene = nullptr;
        jce_camera_destroy(camera);
        jce_scene_destroy(scene);
    }
    JceEntity entity(const char *name, jce_vec3 position, jce_vec3 scale)
    {
        const JceEntity e = jce_scene_create_entity(scene, name);
        REQUIRE(e != 0);
        JceTransform t{};
        t.position = position;
        t.rotation = jce_q_identity();
        t.scale = scale;
        jce_scene_set_transform(scene, e, &t);
        JceEditorMeta meta;
        jce_editor_meta_init(&meta);
        meta.enabled = true;
        jce_scene_set_editor_meta(scene, e, &meta);
        return e;
    }
    JceEntity mesh(const char *name, jce_vec3 position, jce_vec3 scale)
    {
        const JceEntity e = entity(name, position, scale);
        JceMeshRenderer mr;
        jce_mesh_renderer_init(&mr);
        mr.visible = true;
        mr.mesh_path = jce_scene_intern(scene, "unloaded/control.glb");
        jce_scene_set_mesh_renderer(scene, e, &mr);
        return e;
    }
};

void check_vec(jce_vec3 value, jce_vec3 expected)
{
    CHECK(value.x == doctest::Approx(expected.x).epsilon(.00001));
    CHECK(value.y == doctest::Approx(expected.y).epsilon(.00001));
    CHECK(value.z == doctest::Approx(expected.z).epsilon(.00001));
}
void check_framing(const float lo[3], const float hi[3])
{
    // Fit distance comes from the real existing focus math, not a test resolver.
    const auto expected = jce_editor_scene_focus_make_target(
        lo, hi, jce_camera_get_fov(s_sr.camera), 0);
    REQUIRE(s_sr.focus_anim.active);
    check_vec(s_sr.focus_anim.to_center, expected.center);
    CHECK(s_sr.focus_anim.to_distance == doctest::Approx(expected.distance));
    CHECK(s_sr.orbit_pitch == doctest::Approx(65 * JCE_DEG2RAD));
    CHECK_FALSE(s_sr.focus_last_bounds_valid);
    CHECK(s_sr.focus_zoom_step == 0);

    // Advance the actual production update tail through completion and apply
    // its pose to an allocated real Camera, rather than only inspect a request.
    jce_editor_scene_camera_update(1.0f);
    CHECK_FALSE(s_sr.focus_anim.active);
    check_vec(s_sr.orbit_target, expected.center);
    CHECK(s_sr.orbit_distance == doctest::Approx(expected.distance));
    const jce_vec3 eye = jce_camera_get_position(s_sr.camera);
    const jce_vec3 forward = jce_camera_get_forward(s_sr.camera);
    const float dx = expected.center.x - eye.x;
    const float dy = expected.center.y - eye.y;
    const float dz = expected.center.z - eye.z;
    const float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
    CHECK(distance == doctest::Approx(expected.distance));
    CHECK((forward.x*dx + forward.y*dy + forward.z*dz) / distance
          == doctest::Approx(1.0f));
    CHECK(jce_camera_get_near(s_sr.camera) > 0);
    CHECK(jce_camera_get_far(s_sr.camera) > distance);
    CHECK(persist_requests > 0);
}
void check_reset_pose()
{
    CHECK_FALSE(s_sr.focus_anim.active);
    check_vec(s_sr.orbit_target, jce_v3(0, 0, 0));
    CHECK(s_sr.orbit_distance == doctest::Approx(std::sqrt(164.0f)));
    check_vec(jce_camera_get_position(s_sr.camera), jce_v3(8, 6, 8));
    CHECK_FALSE(s_sr.focus_last_bounds_valid);
    CHECK(s_sr.focus_zoom_step == 0);
}
} // namespace

TEST_CASE("Overview frames a translated scaled unloaded cube through the real camera")
{
    Fixture f;
    const auto e = f.mesh("cube", jce_v3(3, 2, -4), jce_v3(2, 4, 6));
    float actual_lo[3], actual_hi[3];
    REQUIRE(jce_editor_scene_camera_get_entity_focus_bounds(
        uint32_t(e), actual_lo, actual_hi));
    const float lo[3] = {2, 0, -7}, hi[3] = {4, 4, -1};
    for (int k = 0; k < 3; ++k) {
        CHECK(actual_lo[k] == doctest::Approx(lo[k]));
        CHECK(actual_hi[k] == doctest::Approx(hi[k]));
    }
    mesh_requests = 0;
    jce_editor_scene_frame_overview();
    check_framing(lo, hi);
    CHECK(mesh_requests == 1);
    CHECK(s_sr.orbit_distance < 100);
}

TEST_CASE("Overview unions renderables but excludes distant logic light and sky entities")
{
    Fixture f;
    f.mesh("cube-a", jce_v3(3, 2, -4), jce_v3(2, 4, 6));
    f.mesh("cube-b", jce_v3(-5, 1, 2), jce_v3(4, 2, 2));
    f.entity("logic-probe", jce_v3(0, -1000, 0), jce_v3(1, 1, 1));
    const auto light = f.entity("light", jce_v3(9000, 8000, 7000), jce_v3(100, 100, 100));
    JcePointLight light_data{};
    light_data.radius = 500;
    jce_scene_set_point_light(f.scene, light, &light_data);
    const auto sky = f.entity("sky", jce_v3(-9000, -8000, -7000), jce_v3(1000, 1000, 1000));
    JceSkyboxComponent sky_data{};
    jce_scene_set_skybox(f.scene, sky, &sky_data);
    const float lo[3] = {-7, 0, -7}, hi[3] = {4, 4, 3};
    jce_editor_scene_frame_overview();
    check_framing(lo, hi);
    CHECK(mesh_requests == 2);
    CHECK(s_sr.orbit_distance < 100);
}

TEST_CASE("Overview ignores disabled entities and disabled renderable components")
{
    Fixture f;
    f.mesh("visible", jce_v3(3, 2, -4), jce_v3(2, 4, 6));
    const auto disabled_entity = f.mesh("disabled-entity", jce_v3(1000, 1000, 1000), jce_v3(100, 100, 100));
    jce_scene_get_editor_meta(f.scene, disabled_entity)->enabled = false;
    const auto disabled_component = f.mesh("disabled-component", jce_v3(-1000, -1000, -1000), jce_v3(100, 100, 100));
    const int mesh_id = jce_component_find("MeshRenderer");
    REQUIRE(mesh_id != JCE_COMP_ID_INVALID);
    jce_scene_set_comp_enabled(f.scene, disabled_component, mesh_id, false);
    CHECK_FALSE(jce_scene_comp_enabled(f.scene, disabled_component, mesh_id));
    const float lo[3] = {2, 0, -7}, hi[3] = {4, 4, -1};
    jce_editor_scene_frame_overview();
    check_framing(lo, hi);
    CHECK(mesh_requests == 1);
}

TEST_CASE("Overview admits an enabled non-mesh LineRenderer through the real registry")
{
    Fixture f;
    const auto e = f.entity("line", jce_v3(4, 3, 2), jce_v3(2, 4, 6));
    JceLineRendererComponent line{};
    line.position_count = 2;
    line.positions[1][0] = 1;
    line.width_start = line.width_end = .1f;
    jce_scene_set_line_renderer(f.scene, e, &line);
    const int line_id = jce_component_find("LineRenderer");
    REQUIRE(line_id != JCE_COMP_ID_INVALID);
    REQUIRE(jce_scene_has_comp(f.scene, e, line_id));
    const float lo[3] = {3, 1, -1}, hi[3] = {5, 5, 5};
    jce_editor_scene_frame_overview();
    check_framing(lo, hi);
    CHECK(mesh_requests == 0);
}

TEST_CASE("Overview rejects nonfinite entity bounds before union")
{
    Fixture f;
    f.mesh("finite", jce_v3(3, 2, -4), jce_v3(2, 4, 6));
    f.mesh("invalid", jce_v3(std::numeric_limits<float>::quiet_NaN(), 0, 0), jce_v3(1, 1, 1));
    const float lo[3] = {2, 0, -7}, hi[3] = {4, 4, -1};
    jce_editor_scene_frame_overview();
    check_framing(lo, hi);
    CHECK(std::isfinite(s_sr.orbit_distance));
}

TEST_CASE("Overview retains authored streaming chunk union over loaded entity bounds")
{
    Fixture f;
    f.mesh("loaded-outlier", jce_v3(10000, 10000, 10000), jce_v3(1, 1, 1));
    auto settings = jce_scene_streaming_settings_default();
    settings.enabled = true;
    settings.chunk_count = 2;
    settings.chunks[0].id = 1;
    settings.chunks[0].center[0] = -100;
    settings.chunks[0].center[2] = 40;
    settings.chunks[0].radius = 20;
    settings.chunks[1].id = 2;
    settings.chunks[1].center[0] = 150;
    settings.chunks[1].center[1] = 10;
    settings.chunks[1].center[2] = -20;
    settings.chunks[1].radius = 30;
    jce_scene_set_streaming_settings(f.scene, &settings);
    REQUIRE(jce_scene_get_streaming_settings(f.scene) != nullptr);
    REQUIRE(jce_scene_get_streaming_settings(f.scene)->chunk_count == 2);
    // Retain the existing streamed-world vertical widening for a span <100.
    const float lo[3] = {-120, -70, -50}, hi[3] = {180, 440, 60};
    jce_editor_scene_frame_overview();
    check_framing(lo, hi);
    CHECK(mesh_requests == 0);
}

TEST_CASE("Overview with authored zero chunks still frames actual geometry")
{
    Fixture f;
    f.mesh("cube", jce_v3(3, 2, -4), jce_v3(2, 4, 6));
    auto settings = jce_scene_streaming_settings_default();
    settings.enabled = true;
    jce_scene_set_streaming_settings(f.scene, &settings);
    REQUIRE(jce_scene_get_streaming_settings(f.scene) != nullptr);
    REQUIRE(jce_scene_get_streaming_settings(f.scene)->chunk_count == 0);
    const float lo[3] = {2, 0, -7}, hi[3] = {4, 4, -1};
    jce_editor_scene_frame_overview();
    check_framing(lo, hi);
}

TEST_CASE("Overview without finite renderable geometry uses the actual reset pose")
{
    Fixture f;
    SUBCASE("empty scene") {}
    SUBCASE("no scene") { provided_scene = nullptr; }
    SUBCASE("only invalid geometry") {
        f.mesh("invalid", jce_v3(std::numeric_limits<float>::infinity(), 0, 0), jce_v3(1, 1, 1));
    }
    s_sr.orbit_target = jce_v3(300, 400, 500);
    s_sr.orbit_distance = 6000;
    s_sr.focus_anim.active = true;
    s_sr.focus_last_bounds_valid = true;
    s_sr.focus_zoom_step = 1;
    jce_editor_scene_frame_overview();
    check_reset_pose();
    CHECK(persist_requests == 1);
}

TEST_CASE("Overview is a no-op without initialization or a camera")
{
    Fixture f;
    SUBCASE("not initialized") { s_sr.initialized = false; }
    SUBCASE("no camera") { s_sr.camera = nullptr; }
    s_sr.orbit_target = jce_v3(3, 4, 5);
    s_sr.orbit_distance = 77;
    s_sr.orbit_pitch = .2f;
    s_sr.orbit_yaw = .3f;
    s_sr.focus_anim.active = true;
    s_sr.focus_anim.to_distance = 123;
    s_sr.focus_zoom_step = 1;
    const auto before_eye = jce_camera_get_position(f.camera);
    jce_editor_scene_frame_overview();
    check_vec(s_sr.orbit_target, jce_v3(3, 4, 5));
    CHECK(s_sr.orbit_distance == 77);
    CHECK(s_sr.orbit_pitch == .2f);
    CHECK(s_sr.orbit_yaw == .3f);
    CHECK(s_sr.focus_anim.active);
    CHECK(s_sr.focus_anim.to_distance == 123);
    CHECK(s_sr.focus_zoom_step == 1);
    check_vec(jce_camera_get_position(f.camera), before_eye);
    CHECK(mesh_requests == 0);
    CHECK(persist_requests == 0);
}

TEST_CASE("Overview and direct focus resolve the live occupant after actual index recycling")
{
    Fixture f;
    const auto dead = f.mesh("retired", jce_v3(-20, -30, -40), jce_v3(1, 1, 1));
    REQUIRE((dead >> 32) == 0);
    jce_scene_destroy_entity(f.scene, dead);
    REQUIRE(jce_scene_entity_from_index(f.scene, uint32_t(dead)) == 0);

    // Same bounded real reuse mechanism as test_jce_scene_entity_generation.
    // Keep other created slots occupied; absence of reuse is a failed setup,
    // never a skipped or passing generation control.
    JceEntity reused = 0;
    for (unsigned attempt = 0; attempt < 4096; ++attempt) {
        const auto candidate = jce_scene_create_entity(f.scene, "recycle-control");
        REQUIRE(candidate != 0);
        if (uint32_t(candidate) == uint32_t(dead)) {
            reused = candidate;
            break;
        }
    }
    REQUIRE(reused != 0);
    REQUIRE(uint32_t(reused) == uint32_t(dead));
    REQUIRE(reused != dead);
    REQUIRE((reused >> 32) != 0);
    REQUIRE_FALSE(jce_scene_entity_alive(f.scene, dead));
    REQUIRE(jce_scene_entity_alive(f.scene, reused));
    REQUIRE(jce_scene_entity_from_index(f.scene, uint32_t(dead)) == reused);
    REQUIRE(jce_scene_get_transform(f.scene, dead) == nullptr);

    JceTransform transform{};
    transform.position = jce_v3(13, 5, -9);
    transform.rotation = jce_q_identity();
    transform.scale = jce_v3(4, 6, 2);
    jce_scene_set_transform(f.scene, reused, &transform);
    JceEditorMeta meta;
    jce_editor_meta_init(&meta);
    meta.enabled = true;
    jce_scene_set_editor_meta(f.scene, reused, &meta);
    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    mr.visible = true;
    mr.mesh_path = jce_scene_intern(f.scene, "unloaded/recycled.glb");
    jce_scene_set_mesh_renderer(f.scene, reused, &mr);

    const float lo[3] = {11, 2, -10}, hi[3] = {15, 8, -8};
    float actual_lo[3], actual_hi[3];
    REQUIRE(jce_editor_scene_camera_get_entity_focus_bounds(
        uint32_t(reused), actual_lo, actual_hi));
    for (int k = 0; k < 3; ++k) {
        CHECK(actual_lo[k] == doctest::Approx(lo[k]));
        CHECK(actual_hi[k] == doctest::Approx(hi[k]));
    }
    jce_editor_scene_frame_overview();
    check_framing(lo, hi);
    CHECK(mesh_requests == 2);
}

TEST_CASE("Direct focus bounds return true only for finite ordered fallback outputs")
{
    Fixture f;
    const auto e = f.mesh("direct-contract", jce_v3(3, 2, -4), jce_v3(2, 4, 6));
    const JceTransform valid = *jce_scene_get_transform(f.scene, e);
    float lo[3], hi[3];
    REQUIRE(jce_editor_scene_camera_get_entity_focus_bounds(uint32_t(e), lo, hi));
    for (int k = 0; k < 3; ++k) {
        CHECK(std::isfinite(lo[k]));
        CHECK(std::isfinite(hi[k]));
        CHECK(lo[k] <= hi[k]);
    }
    CHECK_FALSE(jce_editor_scene_camera_get_entity_focus_bounds(uint32_t(e), nullptr, hi));
    CHECK_FALSE(jce_editor_scene_camera_get_entity_focus_bounds(uint32_t(e), lo, nullptr));
    CHECK_FALSE(jce_editor_scene_camera_get_entity_focus_bounds(uint32_t(e), nullptr, nullptr));

    const float invalid_values[] = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };
    for (const float invalid : invalid_values) {
        auto transform = valid;
        transform.position.x = invalid;
        jce_scene_set_transform(f.scene, e, &transform);
        CHECK_FALSE(jce_editor_scene_camera_get_entity_focus_bounds(uint32_t(e), lo, hi));
        transform = valid;
        transform.scale.y = invalid;
        jce_scene_set_transform(f.scene, e, &transform);
        CHECK_FALSE(jce_editor_scene_camera_get_entity_focus_bounds(uint32_t(e), lo, hi));
        transform = valid;
        transform.rotation.z = invalid;
        jce_scene_set_transform(f.scene, e, &transform);
        CHECK_FALSE(jce_editor_scene_camera_get_entity_focus_bounds(uint32_t(e), lo, hi));
    }
    jce_scene_set_transform(f.scene, e, &valid);
    REQUIRE(jce_editor_scene_camera_get_entity_focus_bounds(uint32_t(e), lo, hi));
    for (int k = 0; k < 3; ++k) {
        CHECK(std::isfinite(lo[k]));
        CHECK(std::isfinite(hi[k]));
        CHECK(lo[k] <= hi[k]);
    }
}
