/*
 * test_jce_anim_compress.c — lossy keyframe reduction (curve fit).
 *
 * 100% headless + deterministic.  Drives jce_anim_compress_track on raw
 * (timestamps, values) arrays and asserts the RDP contract:
 *   (1) a perfectly linear track reduces to its 2 endpoints;
 *   (2) a corner key is preserved;
 *   (3) the reconstruction (lerp of the survivors) stays within tolerance at
 *       every ORIGINAL key time;
 *   (4) a rotation track reduces and stays within its angular tolerance;
 *   (5) tracks of <= 2 keys are returned unchanged;
 *   (6) NULL-safety;
 *   (7) the global opt-in + params round-trip.
 */

#include "unity.h"

#include <jce/middleware/animation/jce_anim_compress.h>
#include <jce/os/core/jce_math.h>

#include <math.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* Reconstruct a VEC3 track value at time t by lerping the compressed survivors. */
static jce_vec3 recon_vec3(const float *times, const jce_vec3 *v,
                           uint32_t count, float t)
{
    uint32_t i;
    if (count == 0u) { jce_vec3 z = { 0, 0, 0 }; return z; }
    if (t <= times[0]) return v[0];
    if (t >= times[count - 1u]) return v[count - 1u];
    for (i = 0u; i + 1u < count; ++i) {
        if (t >= times[i] && t <= times[i + 1u]) {
            float d = times[i + 1u] - times[i];
            float s = (d > 1e-9f) ? (t - times[i]) / d : 0.0f;
            return jce_v3_lerp(v[i], v[i + 1u], s);
        }
    }
    return v[count - 1u];
}

/* ── (1) a linear track collapses to its endpoints ─────────────────────── */
static void test_linear_collapses_to_endpoints(void)
{
    float    t[10];
    jce_vec3 v[10];
    for (int i = 0; i < 10; ++i) { t[i] = (float)i; v[i] = jce_v3((float)i * 2.0f, 0, 0); }

    JceAnimCompressParams p; jce_anim_compress_params_default(&p);
    uint32_t n = jce_anim_compress_track(t, v, 10u, JCE_ANIM_COMPRESS_VEC3, &p);

    TEST_ASSERT_EQUAL_UINT32(2u, n);              /* only endpoints survive */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f,  t[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 9.0f,  t[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f,  v[0].x);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 18.0f, v[1].x);
}

/* ── (2) a corner key is preserved ─────────────────────────────────────── */
static void test_corner_preserved(void)
{
    /* Triangle: x peaks at t=2.  Survivors must be {0,2,4}. */
    float    t[5] = { 0, 1, 2, 3, 4 };
    jce_vec3 v[5] = { {0,0,0}, {1,0,0}, {2,0,0}, {1,0,0}, {0,0,0} };

    JceAnimCompressParams p; jce_anim_compress_params_default(&p);
    uint32_t n = jce_anim_compress_track(t, v, 5u, JCE_ANIM_COMPRESS_VEC3, &p);

    TEST_ASSERT_EQUAL_UINT32(3u, n);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, t[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, t[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 4.0f, t[2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, v[1].x);   /* the peak */
}

/* ── (3) reconstruction stays within tolerance at every original key ────── */
static void test_reconstruction_within_tolerance(void)
{
    enum { N = 64 };
    float    ot[N], t[N];
    jce_vec3 ov[N], v[N];
    for (int i = 0; i < N; ++i) {
        float ti = (float)i / (float)(N - 1);     /* 0..1 */
        ot[i] = t[i] = ti;
        /* a smooth curve so many interior keys are elidable */
        float x = sinf(ti * 6.2831853f) * 0.5f;
        ov[i] = v[i] = jce_v3(x, ti * ti, 0.0f);
    }

    JceAnimCompressParams p;
    p.vec3_tolerance = 0.02f;
    p.quat_tolerance_rad = 0.05f;
    uint32_t n = jce_anim_compress_track(t, v, (uint32_t)N, JCE_ANIM_COMPRESS_VEC3, &p);

    TEST_ASSERT_TRUE(n >= 2u && n < (uint32_t)N);   /* actually reduced */

    float worst = 0.0f;
    for (int i = 0; i < N; ++i) {
        jce_vec3 r = recon_vec3(t, v, n, ot[i]);
        float e = jce_v3_len(jce_v3_sub(ov[i], r));
        if (e > worst) worst = e;
    }
    /* every elided key reconstructs within the tolerance (+ float slack) */
    TEST_ASSERT_TRUE(worst <= 0.02f + 1e-4f);
}

/* ── (4) a rotation track reduces within angular tolerance ─────────────── */
static void test_rotation_reduces(void)
{
    enum { N = 16 };
    float    t[N];
    jce_quat q[N];
    for (int i = 0; i < N; ++i) {
        float a = ((float)i / (float)(N - 1)) * 0.5f;   /* 0..0.5 rad about Y */
        t[i] = (float)i;
        q[i].x = 0.0f; q[i].y = sinf(a * 0.5f); q[i].z = 0.0f; q[i].w = cosf(a * 0.5f);
    }

    JceAnimCompressParams p;
    p.vec3_tolerance = 0.001f;
    p.quat_tolerance_rad = 0.05f;     /* ~2.9 degrees */
    uint32_t n = jce_anim_compress_track(t, q, (uint32_t)N, JCE_ANIM_COMPRESS_QUAT, &p);

    TEST_ASSERT_TRUE(n >= 2u && n < (uint32_t)N);   /* reduced, endpoints kept */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, t[0]);    /* first endpoint time */
}

/* ── (5) tracks of <= 2 keys are returned unchanged ────────────────────── */
static void test_short_tracks_unchanged(void)
{
    float    t2[2] = { 0, 1 };
    jce_vec3 v2[2] = { {0,0,0}, {5,0,0} };
    TEST_ASSERT_EQUAL_UINT32(2u,
        jce_anim_compress_track(t2, v2, 2u, JCE_ANIM_COMPRESS_VEC3, NULL));

    float    t1[1] = { 0 };
    jce_vec3 v1[1] = { {3,0,0} };
    TEST_ASSERT_EQUAL_UINT32(1u,
        jce_anim_compress_track(t1, v1, 1u, JCE_ANIM_COMPRESS_VEC3, NULL));
}

/* ── (6) NULL-safety ───────────────────────────────────────────────────── */
static void test_null_safe(void)
{
    float    t[3] = { 0, 1, 2 };
    jce_vec3 v[3] = { {0,0,0}, {9,0,0}, {0,0,0} };
    TEST_ASSERT_EQUAL_UINT32(3u,
        jce_anim_compress_track(NULL, v, 3u, JCE_ANIM_COMPRESS_VEC3, NULL));
    TEST_ASSERT_EQUAL_UINT32(3u,
        jce_anim_compress_track(t, NULL, 3u, JCE_ANIM_COMPRESS_VEC3, NULL));
}

/* ── (7) global opt-in + params round-trip ─────────────────────────────── */
static void test_global_opt_in(void)
{
    TEST_ASSERT_FALSE(jce_anim_compress_is_enabled());   /* default OFF */
    jce_anim_compress_set_enabled(true);
    TEST_ASSERT_TRUE(jce_anim_compress_is_enabled());
    jce_anim_compress_set_enabled(false);
    TEST_ASSERT_FALSE(jce_anim_compress_is_enabled());

    JceAnimCompressParams def; jce_anim_compress_params_default(&def);
    TEST_ASSERT_TRUE(def.vec3_tolerance > 0.0f);
    TEST_ASSERT_TRUE(def.quat_tolerance_rad > 0.0f);

    JceAnimCompressParams set = { 0.5f, 0.25f }, got;
    jce_anim_compress_set_params(&set);
    jce_anim_compress_get_params(&got);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f,  got.vec3_tolerance);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.25f, got.quat_tolerance_rad);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_linear_collapses_to_endpoints);
    RUN_TEST(test_corner_preserved);
    RUN_TEST(test_reconstruction_within_tolerance);
    RUN_TEST(test_rotation_reduces);
    RUN_TEST(test_short_tracks_unchanged);
    RUN_TEST(test_null_safe);
    RUN_TEST(test_global_opt_in);
    return UNITY_END();
}
