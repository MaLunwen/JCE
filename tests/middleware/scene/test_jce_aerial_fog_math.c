/*
 * test_jce_aerial_fog_math.c
 *
 * C mirror of the per-fragment aerial-fog factor math implemented in
 * engine/shaders/pbr/fog_apply.sh. Shader pixels cannot be unit-tested, but
 * the SCALAR factor math (LINEAR/EXP/EXP2 + Wronski height integral) is pure
 * and must (a) stay in [0,1], (b) be monotonic in distance, (c) stay finite
 * as the view ray approaches horizontal (rayDirY -> 0, the division-by-zero
 * singularity the shader's branchless limit guards). Keep this file in lock-
 * step with fog_apply.sh: any change to the GLSL factor math changes here too.
 */
#include <math.h>
#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* --- reference math (mirror of fog_apply.sh) --- */
static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static float fog_factor_linear(float d, float start, float end) {
    return clamp01((d - start) / fmaxf(end - start, 1e-4f));
}
static float fog_factor_exp(float d, float density) {
    return clamp01(1.0f - expf(-density * d));
}
static float fog_factor_exp2(float d, float density) {
    float x = density * d;
    return clamp01(1.0f - expf(-(x * x)));
}
/* Wronski/UE closed-form height attenuation for a ray of length d whose
 * direction has vertical component rayDirY, with falloff hf about origin h0.
 * scale = exp(-hf*(camY-h0)) * (1 - exp(-hf*rayDirY*d)) / (hf*rayDirY)
 * As rayDirY -> 0 the (1-exp(-a))/a factor -> d (its limit), so we lerp to the
 * branchless limit `d` to avoid 0/0. */
static float fog_height_scale(float camY, float h0, float hf,
                              float rayDirY, float d) {
    float t = hf * rayDirY * d;
    float ad = hf * rayDirY;
    /* (1 - exp(-t)) / ad, with the rayDirY->0 limit == d */
    float num = 1.0f - expf(-t);
    float denom = ad;
    float ratio = (fabsf(denom) > 1e-5f) ? (num / denom) : d;
    return expf(-hf * (camY - h0)) * ratio;
}

static void test_linear_bounds_and_monotonic(void) {
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, fog_factor_linear(0.0f, 10.0f, 100.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, fog_factor_linear(5.0f, 10.0f, 100.0f));   /* before start */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, fog_factor_linear(200.0f, 10.0f, 100.0f)); /* past end */
    float a = fog_factor_linear(30.0f, 10.0f, 100.0f);
    float b = fog_factor_linear(60.0f, 10.0f, 100.0f);
    TEST_ASSERT_TRUE(b > a);
    TEST_ASSERT_TRUE(a >= 0.0f && a <= 1.0f && b >= 0.0f && b <= 1.0f);
}
static void test_linear_degenerate_range_is_safe(void) {
    /* start == end must not divide by zero; clamp keeps result in [0,1]. */
    float f = fog_factor_linear(50.0f, 50.0f, 50.0f);
    TEST_ASSERT_TRUE(f >= 0.0f && f <= 1.0f);
    TEST_ASSERT_FALSE(isnan(f));
}
static void test_exp_bounds(void) {
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, fog_factor_exp(0.0f, 0.02f));
    float f = fog_factor_exp(100.0f, 0.02f);
    TEST_ASSERT_TRUE(f > 0.0f && f < 1.0f);
    TEST_ASSERT_TRUE(fog_factor_exp(1000.0f, 0.05f) > fog_factor_exp(100.0f, 0.05f));
}
static void test_exp2_bounds(void) {
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, fog_factor_exp2(0.0f, 0.02f));
    float f = fog_factor_exp2(80.0f, 0.02f);
    TEST_ASSERT_TRUE(f >= 0.0f && f <= 1.0f);
}
static void test_height_scale_finite_at_horizontal_ray(void) {
    /* rayDirY -> 0 must stay finite (the division-by-zero guard). */
    float s0  = fog_height_scale(10.0f, 0.0f, 0.1f, 0.0f,   50.0f);
    float sEps= fog_height_scale(10.0f, 0.0f, 0.1f, 1e-7f,  50.0f);
    TEST_ASSERT_FALSE(isnan(s0));
    TEST_ASSERT_FALSE(isinf(s0));
    /* limit continuity: near-zero ray matches the exact-zero branch closely */
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, s0, sEps);
}
static void test_height_scale_decreases_with_altitude(void) {
    /* Higher camera above origin => less fog scale (exp(-hf*(camY-h0))). */
    float low  = fog_height_scale(2.0f,  0.0f, 0.1f, -0.3f, 50.0f);
    float high = fog_height_scale(40.0f, 0.0f, 0.1f, -0.3f, 50.0f);
    TEST_ASSERT_TRUE(low > high);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_linear_bounds_and_monotonic);
    RUN_TEST(test_linear_degenerate_range_is_safe);
    RUN_TEST(test_exp_bounds);
    RUN_TEST(test_exp2_bounds);
    RUN_TEST(test_height_scale_finite_at_horizontal_ray);
    RUN_TEST(test_height_scale_decreases_with_altitude);
    return UNITY_END();
}
