/* test_jce_water.c
 *
 * Unit tests for the pure Gerstner / sum-of-sines water model (jce_water) and
 * the JceWaterComponent scene round-trip:
 *   - sum-of-sines height is deterministic and matches a hand-computed value
 *   - a single wave is temporally periodic (height at t == height at t+period)
 *   - the analytic normal is unit length with Y > 0
 *   - full Gerstner displacement sums multiple waves (and moves X/Z, not just Y)
 *   - JceWaterComponent survives prefab save -> instantiate (all fields, waves[])
 */

#include <jce/middleware/scene/jce_water.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_prefab.h>

#include "unity.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define WATER_PREFAB "jce_water_roundtrip.prefab.json"
#define WPI 3.14159265358979323846

void setUp(void)    {}
void tearDown(void) { remove(WATER_PREFAB); }

/* ── Hand-computed sum-of-sines height ────────────────────────────────
 * One wave: A=1, L=2*PI (=> k = 2*PI/L = 1), S=1 (=> omega = k*S = 1),
 * dir=(1,0).  At (x,z,t) = (PI/2, 0, 0): phase = k*(1*x + 0*z) + omega*t
 * = PI/2, so height = base_y + A*sin(PI/2) = base_y + 1. */
static void test_height_matches_hand_value(void)
{
    JceWaterWave w;
    memset(&w, 0, sizeof w);
    w.amplitude  = 1.0f;
    w.wavelength = (float)(2.0 * WPI);
    w.speed      = 1.0f;
    w.dir_x      = 1.0f;
    w.dir_z      = 0.0f;
    w.steepness  = 0.7f;        /* must not affect the vertical height */

    const float base_y = 10.0f;
    const float x = (float)(WPI / 2.0), z = 0.0f, t = 0.0f;

    float h  = jce_water_sample_height(&w, 1, base_y, x, z, t);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 11.0f, h);   /* 10 + sin(PI/2) */

    /* Determinism: identical inputs -> identical output, bit for bit. */
    float h2 = jce_water_sample_height(&w, 1, base_y, x, z, t);
    TEST_ASSERT_EQUAL_FLOAT(h, h2);

    /* Zero waves / NULL fall back to the still-water plane. */
    TEST_ASSERT_EQUAL_FLOAT(base_y, jce_water_sample_height(&w, 0, base_y, x, z, t));
    TEST_ASSERT_EQUAL_FLOAT(base_y, jce_water_sample_height(NULL, 1, base_y, x, z, t));
}

/* ── Temporal periodicity ─────────────────────────────────────────────
 * For a single wave period = 2*PI/omega = L/S.  height(t) == height(t+period). */
static void test_height_periodic(void)
{
    JceWaterWave w;
    memset(&w, 0, sizeof w);
    w.amplitude  = 1.3f;
    w.wavelength = 8.0f;
    w.speed      = 2.0f;
    w.dir_x      = 0.6f;
    w.dir_z      = 0.8f;
    w.steepness  = 0.5f;

    const float period = w.wavelength / w.speed;   /* L / S */
    const float x = 3.0f, z = -2.0f, base = 1.0f;

    for (float t = 0.0f; t < 3.0f; t += 0.37f) {
        float a = jce_water_sample_height(&w, 1, base, x, z, t);
        float b = jce_water_sample_height(&w, 1, base, x, z, t + period);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, a, b);
    }
}

/* ── Analytic normal is unit length with Y > 0 ────────────────────────── */
static void test_normal_unit_and_up(void)
{
    JceWaterWave waves[2];
    memset(waves, 0, sizeof waves);
    waves[0].amplitude = 0.5f; waves[0].wavelength = 12.0f; waves[0].speed = 1.0f;
    waves[0].dir_x = 1.0f; waves[0].dir_z = 0.0f; waves[0].steepness = 0.4f;
    waves[1].amplitude = 0.2f; waves[1].wavelength = 5.0f;  waves[1].speed = 1.7f;
    waves[1].dir_x = 0.3f; waves[1].dir_z = 0.95f; waves[1].steepness = 0.3f;

    for (float t = 0.0f; t < 4.0f; t += 0.5f) {
        for (float x = -10.0f; x <= 10.0f; x += 3.3f) {
            for (float z = -10.0f; z <= 10.0f; z += 3.3f) {
                float n[3];
                jce_water_sample_normal(waves, 2, x, z, t, n);
                float len = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
                TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, len);  /* unit length */
                TEST_ASSERT_TRUE(n[1] > 0.0f);               /* generally +Y */
            }
        }
    }

    /* Flat water (zero waves) -> straight up. */
    float n0[3];
    jce_water_sample_normal(waves, 0, 1.0f, 2.0f, 0.5f, n0);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, n0[0]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, n0[1]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, n0[2]);
}

/* ── Full Gerstner displacement sums multiple waves + moves X/Z ──────── */
static void test_displacement_sums_waves(void)
{
    JceWaterWave waves[2];
    memset(waves, 0, sizeof waves);
    waves[0].amplitude = 0.8f; waves[0].wavelength = 10.0f; waves[0].speed = 1.0f;
    waves[0].dir_x = 1.0f; waves[0].dir_z = 0.0f; waves[0].steepness = 0.6f;
    waves[1].amplitude = 0.4f; waves[1].wavelength = 4.0f;  waves[1].speed = 2.0f;
    waves[1].dir_x = 0.0f; waves[1].dir_z = 1.0f; waves[1].steepness = 0.5f;

    const float base = 3.0f, x = 1.7f, z = -0.9f, t = 0.65f;

    float d2[3], d1a[3], d1b[3];
    jce_water_sample_displacement(waves,   2, base, x, z, t, d2);
    jce_water_sample_displacement(&waves[0], 1, base, x, z, t, d1a);
    jce_water_sample_displacement(&waves[1], 1, base, x, z, t, d1b);

    /* The Y of the two-wave displacement equals base + the two single-wave
     * vertical contributions (each single = base + its own sine). */
    float expect_y = base + (d1a[1] - base) + (d1b[1] - base);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, expect_y, d2[1]);

    /* The Y also equals the sum-of-sines height function. */
    float h = jce_water_sample_height(waves, 2, base, x, z, t);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, h, d2[1]);

    /* Gerstner also rolls X and Z (non-zero steepness/amplitude). */
    TEST_ASSERT_TRUE(fabsf(d2[0] - x) > 1e-4f);
    TEST_ASSERT_TRUE(fabsf(d2[2] - z) > 1e-4f);

    /* Determinism. */
    float again[3];
    jce_water_sample_displacement(waves, 2, base, x, z, t, again);
    TEST_ASSERT_EQUAL_FLOAT(d2[0], again[0]);
    TEST_ASSERT_EQUAL_FLOAT(d2[1], again[1]);
    TEST_ASSERT_EQUAL_FLOAT(d2[2], again[2]);
}

/* ── JceWaterComponent registry-driven prefab round-trip ──────────────── */
static void test_component_roundtrip(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "water");
    JceTransform t; memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f; t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);

    JceWaterComponent w; memset(&w, 0, sizeof w);
    w.size_x = 256.0f; w.size_z = 192.0f;
    w.wave_count = 3;
    w.base_height = -2.5f;
    for (int i = 0; i < JCE_WATER_COMP_MAX_WAVES; ++i) {
        w.waves[i].amplitude  = 0.1f * (float)(i + 1);
        w.waves[i].wavelength = 2.0f * (float)(i + 1);
        w.waves[i].speed      = 0.5f * (float)(i + 1);
        w.waves[i].dir_x      = 0.25f * (float)(i + 1);
        w.waves[i].dir_z      = -0.5f * (float)(i + 1);
        w.waves[i].steepness  = 0.2f * (float)(i + 1);
    }
    w.color_shallow[0] = 0.15f; w.color_shallow[1] = 0.45f; w.color_shallow[2] = 0.55f;
    w.color_deep[0]    = 0.02f; w.color_deep[1]    = 0.08f; w.color_deep[2]    = 0.18f;
    w.transparency = 0.35f;
    w.sun_specular = 2.5f;
    w.visible = true;
    /* Depth write: authored, default off.  A field that fails to serialize is
     * indistinguishable from one left at its default, so a round-trip test has
     * to set it to the NON-default value or it proves nothing. */
    w.depth_write = true;
    /* Non-default so a dropped field is distinguishable from a default one. */
    w.ocean       = true;
    w.fft_fetch   = 120000.0f;
    w.fft_swell   = 0.35f;
    jce_scene_set_water(s, e, &w);

    TEST_ASSERT_TRUE(jce_prefab_save_subtree(s, e, WATER_PREFAB));
    jce_scene_destroy(s);

    JceScene *s2 = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s2);
    JceEntity root = jce_prefab_instantiate_file(s2, WATER_PREFAB, NULL);
    TEST_ASSERT_TRUE(root != 0);

    JceWaterComponent *g = jce_scene_get_water(s2, root);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_FLOAT(256.0f, g->size_x);
    TEST_ASSERT_EQUAL_FLOAT(192.0f, g->size_z);
    TEST_ASSERT_EQUAL_INT(3, g->wave_count);
    TEST_ASSERT_EQUAL_FLOAT(-2.5f, g->base_height);
    TEST_ASSERT_TRUE(g->depth_write);
    TEST_ASSERT_TRUE(g->ocean);
    TEST_ASSERT_EQUAL_FLOAT(120000.0f, g->fft_fetch);
    TEST_ASSERT_EQUAL_FLOAT(0.35f, g->fft_swell);
    for (int i = 0; i < JCE_WATER_COMP_MAX_WAVES; ++i) {
        TEST_ASSERT_EQUAL_FLOAT(0.1f * (float)(i + 1), g->waves[i].amplitude);
        TEST_ASSERT_EQUAL_FLOAT(2.0f * (float)(i + 1), g->waves[i].wavelength);
        TEST_ASSERT_EQUAL_FLOAT(0.5f * (float)(i + 1), g->waves[i].speed);
        TEST_ASSERT_EQUAL_FLOAT(0.25f * (float)(i + 1), g->waves[i].dir_x);
        TEST_ASSERT_EQUAL_FLOAT(-0.5f * (float)(i + 1), g->waves[i].dir_z);
        TEST_ASSERT_EQUAL_FLOAT(0.2f * (float)(i + 1), g->waves[i].steepness);
    }
    TEST_ASSERT_EQUAL_FLOAT(0.45f, g->color_shallow[1]);
    TEST_ASSERT_EQUAL_FLOAT(0.18f, g->color_deep[2]);
    TEST_ASSERT_EQUAL_FLOAT(0.35f, g->transparency);
    TEST_ASSERT_EQUAL_FLOAT(2.5f,  g->sun_specular);
    TEST_ASSERT_TRUE(g->visible);

    jce_scene_destroy(s2);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_height_matches_hand_value);
    RUN_TEST(test_height_periodic);
    RUN_TEST(test_normal_unit_and_up);
    RUN_TEST(test_displacement_sums_waves);
    RUN_TEST(test_component_roundtrip);
    return UNITY_END();
}
