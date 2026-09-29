// test_jce_editor_camera_tools.cpp - pure Align/Pilot camera pose math.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "scene/jce_editor_camera_tools.h"

#include <cmath>

namespace {

bool near_equal(float a, float b, float tolerance = 1.0e-5f)
{
    return std::fabs(a - b) <= tolerance;
}

void check_vec3(jce_vec3 actual, jce_vec3 expected, float tolerance = 1.0e-5f)
{
    CHECK(near_equal(actual.x, expected.x, tolerance));
    CHECK(near_equal(actual.y, expected.y, tolerance));
    CHECK(near_equal(actual.z, expected.z, tolerance));
}

} /* namespace */

TEST_CASE("camera pose converts to an orbit without changing the view ray")
{
    const JceEditorCameraPose pose = {
        jce_v3(1.0f, 2.0f, 3.0f),
        jce_v3(0.0f, 0.0f, -1.0f),
        jce_v3(0.0f, 1.0f, 0.0f),
    };
    JceEditorCameraOrbit orbit{};

    REQUIRE(jce_editor_camera_tools_make_orbit(pose, 5.0f, &orbit));
    check_vec3(orbit.target, jce_v3(1.0f, 2.0f, -2.0f));
    CHECK(near_equal(std::fabs(orbit.yaw), JCE_PI));
    CHECK(near_equal(orbit.pitch, 0.0f));
    CHECK(near_equal(orbit.distance, 5.0f));

    const jce_vec3 reconstructed = jce_v3(
        orbit.target.x + orbit.distance * sinf(orbit.yaw) * cosf(orbit.pitch),
        orbit.target.y + orbit.distance * sinf(orbit.pitch),
        orbit.target.z - orbit.distance * cosf(orbit.yaw) * cosf(orbit.pitch));
    check_vec3(reconstructed, pose.position);
}

TEST_CASE("camera basis creates a quaternion with JCE minus-Z forward")
{
    const JceEditorCameraPose pose = {
        jce_v3(0.0f, 0.0f, 0.0f),
        jce_v3(1.0f, -0.25f, -0.5f),
        jce_v3(0.0f, 1.0f, 0.0f),
    };
    jce_quat rotation{};

    REQUIRE(jce_editor_camera_tools_make_rotation(pose, &rotation));
    const jce_vec3 expected_forward = jce_v3_normalize(pose.forward);
    const jce_vec3 actual_forward =
        jce_q_rotate(rotation, jce_v3(0.0f, 0.0f, -1.0f));
    check_vec3(actual_forward, expected_forward, 1.0e-4f);

    const jce_vec3 actual_up =
        jce_q_rotate(rotation, jce_v3(0.0f, 1.0f, 0.0f));
    CHECK(near_equal(jce_v3_dot(actual_up, actual_forward), 0.0f, 1.0e-4f));
    CHECK(near_equal(jce_v3_len(actual_up), 1.0f, 1.0e-4f));
}

TEST_CASE("camera pose validation rejects unusable input")
{
    JceEditorCameraPose pose{};
    pose.position = jce_v3(0.0f, 0.0f, 0.0f);
    pose.forward = jce_v3(0.0f, 0.0f, 0.0f);
    pose.up = jce_v3(0.0f, 1.0f, 0.0f);
    JceEditorCameraOrbit orbit{};
    jce_quat rotation{};

    CHECK_FALSE(jce_editor_camera_tools_make_orbit(pose, 10.0f, &orbit));
    CHECK_FALSE(jce_editor_camera_tools_make_orbit(pose, 0.0f, &orbit));
    CHECK_FALSE(jce_editor_camera_tools_make_rotation(pose, &rotation));
}

TEST_CASE("parallel up vector uses a stable orthogonal fallback")
{
    const JceEditorCameraPose pose = {
        jce_v3(0.0f, 0.0f, 0.0f),
        jce_v3(0.0f, 1.0f, 0.0f),
        jce_v3(0.0f, 1.0f, 0.0f),
    };
    jce_quat rotation{};

    REQUIRE(jce_editor_camera_tools_make_rotation(pose, &rotation));
    const jce_vec3 forward =
        jce_q_rotate(rotation, jce_v3(0.0f, 0.0f, -1.0f));
    check_vec3(forward, pose.forward, 1.0e-4f);
}
