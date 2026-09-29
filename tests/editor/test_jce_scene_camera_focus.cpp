// test_jce_scene_camera_focus.cpp - Unity-style scene camera focus math.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "scene/jce_scene_camera_focus.h"

#include <cmath>

namespace {

constexpr float kTol = 1e-4f;

bool nearly_equal(float a, float b, float tol = kTol)
{
    return std::fabs(a - b) <= tol;
}

}  // namespace

TEST_CASE("focus target fits an AABB with the current vertical FOV")
{
    const float bmin[3] = { -1.0f, -1.0f, -1.0f };
    const float bmax[3] = {  1.0f,  1.0f,  1.0f };

    JceEditorSceneFocusTarget target =
        jce_editor_scene_focus_make_target(bmin, bmax, 60.0f, 0);
    JceEditorSceneFocusTarget far_target =
        jce_editor_scene_focus_make_target(bmin, bmax, 60.0f, 1);

    CHECK(nearly_equal(target.center.x, 0.0f));
    CHECK(nearly_equal(target.center.y, 0.0f));
    CHECK(nearly_equal(target.center.z, 0.0f));
    CHECK(nearly_equal(target.base_distance, 3.9837167f, 1e-3f));
    CHECK(nearly_equal(target.distance, target.base_distance * 0.65f, 1e-3f));
    CHECK(nearly_equal(far_target.distance, far_target.base_distance * 2.0f, 1e-3f));
}

TEST_CASE("focus target clamps tiny selections to a usable framing distance")
{
    const float bmin[3] = { 4.0f, 5.0f, 6.0f };
    const float bmax[3] = { 4.0f, 5.0f, 6.0f };

    JceEditorSceneFocusTarget target =
        jce_editor_scene_focus_make_target(bmin, bmax, 60.0f, 0);

    CHECK(nearly_equal(target.center.x, 4.0f));
    CHECK(nearly_equal(target.center.y, 5.0f));
    CHECK(nearly_equal(target.center.z, 6.0f));
    CHECK(nearly_equal(target.base_distance, 1.15f, 1e-3f));
}

TEST_CASE("repeat focus toggles between Unity-style close and far modes")
{
    CHECK(jce_editor_scene_focus_next_zoom_step(false, 0) == 0);
    CHECK(jce_editor_scene_focus_next_zoom_step(true, 0) == 1);
    CHECK(jce_editor_scene_focus_next_zoom_step(true, 1) == 0);
    CHECK(jce_editor_scene_focus_next_zoom_step(true, 4) == 1);

    const float bmin[3] = { -1.0f, -1.0f, -1.0f };
    const float bmax[3] = {  1.0f,  1.0f,  1.0f };

    JceEditorSceneFocusTarget close =
        jce_editor_scene_focus_make_target(bmin, bmax, 60.0f, 0);
    JceEditorSceneFocusTarget far_mode =
        jce_editor_scene_focus_make_target(bmin, bmax, 60.0f, 1);

    CHECK(close.distance < far_mode.distance);
    CHECK(nearly_equal(close.distance, close.base_distance * 0.65f, 1e-3f));
    CHECK(nearly_equal(far_mode.distance, far_mode.base_distance * 2.0f, 1e-3f));
}

TEST_CASE("bounds comparison tolerates small float noise but rejects movement")
{
    const float a_min[3] = { -1.0f, -1.0f, -1.0f };
    const float a_max[3] = {  1.0f,  1.0f,  1.0f };
    const float noisy_min[3] = { -1.0001f, -1.0f, -1.0f };
    const float noisy_max[3] = {  1.0f,  1.0001f,  1.0f };
    const float moved_min[3] = { -1.05f, -1.0f, -1.0f };
    const float moved_max[3] = {  1.0f,   1.0f,  1.0f };

    CHECK(jce_editor_scene_focus_same_bounds(a_min, a_max,
                                             noisy_min, noisy_max,
                                             0.001f));
    CHECK_FALSE(jce_editor_scene_focus_same_bounds(a_min, a_max,
                                                   moved_min, moved_max,
                                                   0.001f));
}

TEST_CASE("local mesh bounds transform to world bounds before framing")
{
    const float local_min[3] = { -2.0f, -1.0f, -0.5f };
    const float local_max[3] = {  2.0f,  1.0f,  0.5f };
    float world_min[3] = { 0.0f, 0.0f, 0.0f };
    float world_max[3] = { 0.0f, 0.0f, 0.0f };

    jce_editor_scene_focus_transform_aabb(local_min,
                                          local_max,
                                          jce_v3(10.0f, 0.0f, -5.0f),
                                          jce_q_identity(),
                                          jce_v3(3.0f, 2.0f, 1.0f),
                                          world_min,
                                          world_max);

    CHECK(nearly_equal(world_min[0],  4.0f));
    CHECK(nearly_equal(world_max[0], 16.0f));
    CHECK(nearly_equal(world_min[1], -2.0f));
    CHECK(nearly_equal(world_max[1],  2.0f));
    CHECK(nearly_equal(world_min[2], -5.5f));
    CHECK(nearly_equal(world_max[2], -4.5f));
}

TEST_CASE("focus animation eases toward the target and ends exactly")
{
    JceEditorSceneFocusAnim anim{};
    jce_editor_scene_focus_anim_start(&anim,
                                      jce_v3(0.0f, 0.0f, 0.0f), 10.0f,
                                      jce_v3(10.0f, 0.0f, 0.0f), 2.0f,
                                      0.30f);

    JceEditorSceneFocusSample half =
        jce_editor_scene_focus_anim_step(&anim, 0.15f);
    CHECK(anim.active);
    CHECK(half.center.x > 0.0f);
    CHECK(half.center.x < 10.0f);
    CHECK(half.distance < 10.0f);
    CHECK(half.distance > 2.0f);

    JceEditorSceneFocusSample end =
        jce_editor_scene_focus_anim_step(&anim, 1.0f);
    CHECK_FALSE(anim.active);
    CHECK(nearly_equal(end.center.x, 10.0f));
    CHECK(nearly_equal(end.center.y, 0.0f));
    CHECK(nearly_equal(end.center.z, 0.0f));
    CHECK(nearly_equal(end.distance, 2.0f));
}
