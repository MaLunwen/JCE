// test_jce_editor_quat.cpp — round-trip tests for header-only quat<->Euler helpers.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_editor_quat.h"

#include <cmath>

namespace {

constexpr float kTol = 1e-3f;

bool nearly_equal(float a, float b, float tol = kTol)
{
    return std::fabs(a - b) <= tol;
}

}  // namespace

TEST_CASE("editor_q_from_euler_deg(0,0,0) yields identity quaternion")
{
    const float zero[3] = { 0.f, 0.f, 0.f };
    jce_quat q = editor_q_from_euler_deg(zero);
    CHECK(nearly_equal(q.x, 0.f));
    CHECK(nearly_equal(q.y, 0.f));
    CHECK(nearly_equal(q.z, 0.f));
    CHECK(nearly_equal(q.w, 1.f));
}

TEST_CASE("editor_q_to_euler_deg round-trips small angles")
{
    const float in[3] = { 12.0f, -34.5f, 7.25f };
    jce_quat q = editor_q_from_euler_deg(in);
    float out[3] = { 0, 0, 0 };
    editor_q_to_euler_deg(q, out);
    CHECK(nearly_equal(out[0], in[0]));
    CHECK(nearly_equal(out[1], in[1]));
    CHECK(nearly_equal(out[2], in[2]));
}

TEST_CASE("editor_q_to_euler_deg returns degrees in expected magnitude")
{
    /* 90 deg pitch -> Euler.y should be near 90 (not pi/2). */
    const float in[3] = { 0.f, 90.f, 0.f };
    jce_quat q = editor_q_from_euler_deg(in);
    float out[3] = { 0, 0, 0 };
    editor_q_to_euler_deg(q, out);
    CHECK(nearly_equal(std::fabs(out[1]), 90.f, 0.05f));
}
