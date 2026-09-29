/*
 * test_jce_sky_dome_serialize.c
 *
 * Stylized sky dome: JSON serialization round-trip + absent-key default.
 * Tests:
 *   1. Full round-trip via the public _to_json / _from_json wrappers:
 *      mutated dome fields serialize and parse back correctly.
 *   2. Absent dome keys land on the golden-hour defaults (old-scene compat).
 *   3. Full scene-level round-trip (jce_scene_save_json / load_json) with
 *      sky_mode == JCE_SCENE_SKY_STYLIZED and dome fields authored.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* ── Helper ─────────────────────────────────────────────────────────── */

static void test_near(float a, float b, const char *file, int line)
{
    UNITY_TEST_ASSERT_FLOAT_WITHIN(1e-4f, b, a, line, file);
}
#define NEAR(a,b) test_near((a),(b), __FILE__, __LINE__)

/* ── 1. Round-trip via _to_json / _from_json ─────────────────────────── */

static void test_dome_round_trip_via_wrappers(void)
{
    JceSceneRenderingSettings a = jce_scene_rendering_settings_default();
    a.sky_mode               = JCE_SCENE_SKY_STYLIZED;
    a.sky_dome_zenith[0]     = 0.11f;
    a.sky_dome_zenith[2]     = 0.71f;
    a.sky_dome_mid_pos       = 0.37f;
    a.sky_dome_glow_falloff  = 9.5f;
    a.sky_dome_sun_size      = 0.9991f;
    a.sky_dome_halo_power    = 64.0f;
    a.sky_dome_halo_strength = 0.5f;

    char *json = jce_scene_rendering_settings_to_json(&a);
    TEST_ASSERT_NOT_NULL_MESSAGE(json, "to_json returned NULL");

    JceSceneRenderingSettings b;
    memset(&b, 0, sizeof(b));
    TEST_ASSERT_TRUE_MESSAGE(
        jce_scene_rendering_settings_from_json(json, &b),
        "from_json returned false");
    free(json);

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_SKY_STYLIZED, b.sky_mode);
    NEAR(b.sky_dome_zenith[0],     0.11f);
    NEAR(b.sky_dome_zenith[2],     0.71f);
    NEAR(b.sky_dome_mid_pos,       0.37f);
    NEAR(b.sky_dome_glow_falloff,  9.5f);
    NEAR(b.sky_dome_sun_size,      0.9991f);
    NEAR(b.sky_dome_halo_power,    64.0f);
    NEAR(b.sky_dome_halo_strength, 0.5f);
}

/* ── 2. Absent dome keys → golden-hour defaults (old-scene compat) ───── */

static void test_absent_dome_keys_use_defaults(void)
{
    /* A minimal JSON with ONLY {"sky":{"mode":2}} and no dome sub-object. */
    JceSceneRenderingSettings c;
    memset(&c, 0, sizeof(c));
    TEST_ASSERT_TRUE(
        jce_scene_rendering_settings_from_json("{\"sky\":{\"mode\":2}}", &c));

    JceSceneRenderingSettings d = jce_scene_rendering_settings_default();
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_SKY_PREETHAM, c.sky_mode);
    /* Dome fields must land on golden-hour defaults (not zero). */
    NEAR(c.sky_dome_mid_pos,      d.sky_dome_mid_pos);
    NEAR(c.sky_dome_halo_power,   d.sky_dome_halo_power);
    NEAR(c.sky_dome_glow_falloff, d.sky_dome_glow_falloff);
    NEAR(c.sky_dome_sun_size,     d.sky_dome_sun_size);
}

/* ── 3. Full scene round-trip with dome fields ───────────────────────── */

static void test_dome_round_trip_in_scene_json(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    JceSceneRenderingSettings r = jce_scene_rendering_settings_default();
    r.sky_mode               = JCE_SCENE_SKY_STYLIZED;
    r.sky_dome_zenith[0]     = 0.16f;
    r.sky_dome_zenith[1]     = 0.33f;
    r.sky_dome_zenith[2]     = 0.62f;
    r.sky_dome_mid_pos       = 0.45f;
    r.sky_dome_glow_falloff  = 7.0f;
    r.sky_dome_sun_size      = 0.9985f;
    r.sky_dome_sun_softness  = 0.0010f;
    r.sky_dome_halo_power    = 48.0f;
    r.sky_dome_halo_strength = 0.35f;
    r.sky_dome_sun_color[0]  = 1.0f;
    r.sky_dome_sun_color[1]  = 0.92f;
    r.sky_dome_sun_color[2]  = 0.70f;
    jce_scene_set_rendering_settings(src, &r);

    JceJson *root = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(root);

    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, loaded,
        "jce_scene_load_json should return 0 for a scene with no entities");

    const JceSceneRenderingSettings *out = jce_scene_get_rendering_settings(dst);
    TEST_ASSERT_NOT_NULL(out);

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_SKY_STYLIZED, out->sky_mode);
    NEAR(out->sky_dome_zenith[0],     0.16f);
    NEAR(out->sky_dome_zenith[1],     0.33f);
    NEAR(out->sky_dome_zenith[2],     0.62f);
    NEAR(out->sky_dome_mid_pos,       0.45f);
    NEAR(out->sky_dome_glow_falloff,  7.0f);
    NEAR(out->sky_dome_sun_size,      0.9985f);
    NEAR(out->sky_dome_sun_softness,  0.0010f);
    NEAR(out->sky_dome_halo_power,    48.0f);
    NEAR(out->sky_dome_halo_strength, 0.35f);
    NEAR(out->sky_dome_sun_color[0],  1.0f);
    NEAR(out->sky_dome_sun_color[1],  0.92f);
    NEAR(out->sky_dome_sun_color[2],  0.70f);

    jce_json_free(root);
    jce_scene_destroy(dst);
    jce_scene_destroy(src);
}

/* ── 4. Scene JSON without dome block keeps golden-hour defaults ──────── */

static void test_scene_without_dome_keeps_defaults(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    TEST_ASSERT_NOT_NULL(src); TEST_ASSERT_NOT_NULL(dst);

    JceSceneRenderingSettings r = jce_scene_rendering_settings_default();
    r.sky_mode = JCE_SCENE_SKY_PREETHAM;
    jce_scene_set_rendering_settings(src, &r);

    JceJson *root = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(root);

    /* Strip the dome sub-object from the serialized sky, simulating a
     * pre-stylized-sky scene file. */
    JceJson *scene_obj = jce_json_get(root, "scene");
    JceJson *rendering = scene_obj ? jce_json_get(scene_obj, "rendering") : NULL;
    JceJson *env       = rendering ? jce_json_get(rendering, "environment") : NULL;
    JceJson *sky       = env       ? jce_json_get(env, "sky") : NULL;
    if (sky) jce_json_remove(sky, "dome");

    TEST_ASSERT_EQUAL_INT(0, jce_scene_load_json(dst, root));

    const JceSceneRenderingSettings *out = jce_scene_get_rendering_settings(dst);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_SKY_PREETHAM, out->sky_mode);

    JceSceneRenderingSettings d = jce_scene_rendering_settings_default();
    NEAR(out->sky_dome_mid_pos,      d.sky_dome_mid_pos);
    NEAR(out->sky_dome_halo_power,   d.sky_dome_halo_power);
    NEAR(out->sky_dome_glow_falloff, d.sky_dome_glow_falloff);
    NEAR(out->sky_dome_sun_size,     d.sky_dome_sun_size);

    jce_json_free(root);
    jce_scene_destroy(dst); jce_scene_destroy(src);
}

/* ── Every sky mode survives the round trip ────────────────────────────
 *
 * The rendering-settings sanitiser clamped `sky_mode > 3` to GRADIENT with a
 * LITERAL upper bound.  Adding a fifth mode therefore compiled, serialized,
 * appeared in the editor, and was silently reset to GRADIENT on every load --
 * implemented everywhere and rendering nowhere.
 *
 * Looping over the whole enum is what makes the next added mode fail here
 * rather than in someone's scene: a test that names one mode only re-checks
 * the mode it names. */

static void test_all_sky_modes_survive_round_trip(void)
{
    const int modes[] = {
        JCE_SCENE_SKY_GRADIENT, JCE_SCENE_SKY_EQUIRECT,
        JCE_SCENE_SKY_PREETHAM, JCE_SCENE_SKY_STYLIZED,
        JCE_SCENE_SKY_PHYSICAL,
    };
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        JceSceneRenderingSettings a = jce_scene_rendering_settings_default();
        a.sky_mode = modes[i];

        char *json = jce_scene_rendering_settings_to_json(&a);
        TEST_ASSERT_NOT_NULL(json);
        JceSceneRenderingSettings b;
        memset(&b, 0, sizeof b);
        TEST_ASSERT_TRUE(jce_scene_rendering_settings_from_json(json, &b));
        free(json);

        TEST_ASSERT_EQUAL_INT(modes[i], b.sky_mode);
    }

    /* The CLAMP lives in the scene's sanitiser, which runs on
     * jce_scene_set_rendering_settings -- not in from_json.  Testing it here
     * means going through that path; asserting it on from_json would have been
     * testing a function that never sanitises and calling the result a pass. */
    JceScene *sc = jce_scene_create();
    TEST_ASSERT_NOT_NULL(sc);

    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        JceSceneRenderingSettings ok = jce_scene_rendering_settings_default();
        ok.sky_mode = modes[i];
        jce_scene_set_rendering_settings(sc, &ok);
        /* Every real mode must SURVIVE the sanitiser: a literal upper bound
         * here is what silently reset a newly-added mode to GRADIENT. */
        TEST_ASSERT_EQUAL_INT(modes[i],
                              jce_scene_get_rendering_settings(sc)->sky_mode);
    }

    /* Widening the clamp must not mean removing it: a corrupt scene must not
     * select a mode the shader has no branch for. */
    JceSceneRenderingSettings bad = jce_scene_rendering_settings_default();
    bad.sky_mode = JCE_SCENE_SKY_PHYSICAL + 7;
    jce_scene_set_rendering_settings(sc, &bad);
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_SKY_GRADIENT,
                          jce_scene_get_rendering_settings(sc)->sky_mode);

    /* Cloud settings round-trip at NON-default values, and the sanitiser
     * rejects a degenerate layer rather than letting the march divide by a
     * zero-thickness slab. */
    JceSceneRenderingSettings cl = jce_scene_rendering_settings_default();
    cl.cloud_coverage  = 0.62f;
    cl.cloud_density   = 3.5f;
    cl.cloud_bottom_km = 1.2f;
    cl.cloud_top_km    = 5.5f;
    char *cj = jce_scene_rendering_settings_to_json(&cl);
    TEST_ASSERT_NOT_NULL(cj);
    JceSceneRenderingSettings cb;
    memset(&cb, 0, sizeof cb);
    TEST_ASSERT_TRUE(jce_scene_rendering_settings_from_json(cj, &cb));
    free(cj);
    NEAR(cb.cloud_coverage,  0.62f);
    NEAR(cb.cloud_density,   3.5f);
    NEAR(cb.cloud_bottom_km, 1.2f);
    NEAR(cb.cloud_top_km,    5.5f);

    JceSceneRenderingSettings degen = jce_scene_rendering_settings_default();
    degen.cloud_coverage  = 0.5f;
    degen.cloud_bottom_km = 4.0f;
    degen.cloud_top_km    = 2.0f;      /* top below bottom */
    jce_scene_set_rendering_settings(sc, &degen);
    const JceSceneRenderingSettings *fixed =
        jce_scene_get_rendering_settings(sc);
    TEST_ASSERT_TRUE(fixed->cloud_top_km <= 0.0f ||
                     fixed->cloud_top_km > fixed->cloud_bottom_km);

    jce_scene_destroy(sc);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dome_round_trip_via_wrappers);
    RUN_TEST(test_absent_dome_keys_use_defaults);
    RUN_TEST(test_dome_round_trip_in_scene_json);
    RUN_TEST(test_scene_without_dome_keeps_defaults);
    RUN_TEST(test_all_sky_modes_survive_round_trip);
    return UNITY_END();
}
