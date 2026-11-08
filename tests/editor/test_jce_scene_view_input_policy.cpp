#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "gizmo/jce_gizmo_math.h"
#include "panels/jce_scene_view_input_policy.h"

TEST_CASE("scene selection starts only from a viewport-owned left click")
{
    CHECK(jce_scene_view_should_start_selection(true, false, false, false));

    CHECK_FALSE(jce_scene_view_should_start_selection(false, false, false, false));
    CHECK_FALSE(jce_scene_view_should_start_selection(true, true, false, false));
    CHECK_FALSE(jce_scene_view_should_start_selection(true, false, true, false));
    CHECK_FALSE(jce_scene_view_should_start_selection(true, false, false, true));
}

TEST_CASE("continuous left input belongs to the scene view only while its item owns it")
{
    CHECK(jce_scene_view_left_input_belongs_to_viewport(true, false, true));
    CHECK(jce_scene_view_left_input_belongs_to_viewport(false, true, true));

    CHECK_FALSE(jce_scene_view_left_input_belongs_to_viewport(false, false, true));
    CHECK_FALSE(jce_scene_view_left_input_belongs_to_viewport(false, true, false));
}

TEST_CASE("external left clicks cancel deferred scene picking")
{
    CHECK(jce_scene_view_should_cancel_deferred_pick(true, false, false));

    CHECK_FALSE(jce_scene_view_should_cancel_deferred_pick(false, false, false));
    CHECK_FALSE(jce_scene_view_should_cancel_deferred_pick(true, true, false));
    CHECK_FALSE(jce_scene_view_should_cancel_deferred_pick(true, false, true));
}

TEST_CASE("scene context menu opens only from a viewport-owned right click")
{
    CHECK(jce_scene_view_should_open_context_menu(true, false));

    CHECK_FALSE(jce_scene_view_should_open_context_menu(false, false));
    CHECK_FALSE(jce_scene_view_should_open_context_menu(true, true));
}

TEST_CASE("navigation chrome only blocks new gizmo interaction")
{
    CHECK(jce_scene_view_should_update_gizmo(false, false, false));
    CHECK_FALSE(jce_scene_view_should_update_gizmo(true, false, false));
    CHECK_FALSE(jce_scene_view_should_update_gizmo(false, true, false));
    CHECK(jce_scene_view_should_update_gizmo(true, true, true));
}

TEST_CASE("navigation layout is clamped and hit regions cannot escape the viewport")
{
    const auto small = jce_scene_view_cube_layout(10, 20, 100, 50);
    const auto large = jce_scene_view_cube_layout(10, 20, 4000, 3000);
    CHECK(small.radius == 28.0f); CHECK(large.radius == 50.0f);
    auto bounds = small.bounds;
    REQUIRE(jce_scene_view_clip_overlay_rect(&bounds, 10, 20, 100, 50));
    CHECK(bounds.x0 >= 10); CHECK(bounds.y0 >= 20);
    CHECK(bounds.x1 <= 110); CHECK(bounds.y1 <= 70);
    CHECK_FALSE(jce_scene_view_clip_overlay_rect(&bounds, 10, 20, 0, 50));
    CHECK_FALSE(jce_scene_view_clip_overlay_rect(nullptr, 10, 20, 100, 50));
    CHECK(jce_scene_view_axis_layout(0, 0, 100, 50).radius == 20.0f);
    CHECK(jce_scene_view_axis_layout(0, 0, 4000, 3000).radius == 45.0f);
}

TEST_CASE("CPU click rays use the viewport and the backend clip-depth convention")
{
    JceGizmoCamera camera = {};
    const jce_mat4 identity = jce_m4_identity();
    memcpy(camera.view, identity.raw, sizeof(camera.view));
    memcpy(camera.proj, identity.raw, sizeof(camera.proj));
    camera.viewport_origin[0] = 10.0f;
    camera.viewport_origin[1] = 20.0f;
    camera.viewport_size[0] = 100.0f;
    camera.viewport_size[1] = 200.0f;

    float origin[3], direction[3], length;
    REQUIRE(gm_screen_to_clip_ray(&camera, 85.0f, 70.0f, true,
                                  origin, direction, &length));
    CHECK(origin[0] == doctest::Approx(0.5f));
    CHECK(origin[1] == doctest::Approx(0.5f));
    CHECK(origin[2] == doctest::Approx(-1.0f));
    CHECK(direction[2] == doctest::Approx(1.0f));
    CHECK(length == doctest::Approx(2.0f));

    REQUIRE(gm_screen_to_clip_ray(&camera, 85.0f, 70.0f, false,
                                  origin, direction, &length));
    CHECK(origin[2] == doctest::Approx(0.0f));
    CHECK(length == doctest::Approx(1.0f));

    memset(camera.proj, 0, sizeof(camera.proj));
    CHECK_FALSE(gm_screen_to_clip_ray(&camera, 85.0f, 70.0f, true,
                                      origin, direction, &length));
}
