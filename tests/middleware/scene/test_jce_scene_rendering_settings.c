/*
 * test_jce_scene_rendering_settings.c
 *
 * Scene-level rendering environment state: ambient, fog, post-fx, and
 * shadow quality must belong to the scene data model, not editor panel
 * globals. These tests exercise the public scene API and JSON round-trip.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>

#include <stdio.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_default_settings_are_conservative(void)
{
    JceSceneRenderingSettings r = jce_scene_rendering_settings_default();

    TEST_ASSERT_FALSE(r.fog_enabled);
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_FOG_NONE, r.fog_mode);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.1f,  r.ambient_color[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.12f, r.ambient_color[2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f,  r.ambient_intensity);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 100.0f, r.shadow_distance);
    TEST_ASSERT_EQUAL_INT(4, r.cascade_count);
    TEST_ASSERT_EQUAL_INT(2048, r.shadow_resolution);
    for (int i = 0; i < JCE_SCENE_RENDERING_POSTFX_COUNT; ++i)
        TEST_ASSERT_FALSE(r.postfx_enabled[i]);
}

static void test_look_profile_defaults_are_neutral(void)
{
    JceSceneRenderingSettings r = jce_scene_rendering_settings_default();
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, r.wrap_factor);
    TEST_ASSERT_FALSE(r.ambient_hemisphere);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, r.rim_intensity);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, r.lut_strength);
    TEST_ASSERT_EQUAL_INT(JCE_TONEMAP_ACES, r.tonemap_op);
    TEST_ASSERT_FALSE(r.toon_character);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, r.bloom_knee);
    TEST_ASSERT_EQUAL_STRING("", r.lut_path);
}

static void test_scene_clear_drops_rendering_settings(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceSceneRenderingSettings r = jce_scene_rendering_settings_default();
    r.fog_enabled = true;
    r.fog_mode = JCE_SCENE_FOG_EXP2;
    r.fog_density = 0.075f;
    jce_scene_set_rendering_settings(s, &r);

    TEST_ASSERT_TRUE(jce_scene_has_rendering_settings(s));
    TEST_ASSERT_EQUAL_INT(0, jce_scene_clear(s));
    TEST_ASSERT_FALSE(jce_scene_has_rendering_settings(s));

    jce_scene_destroy(s);
}

static void test_rendering_settings_round_trip_in_scene_json(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    JceSceneRenderingSettings r = jce_scene_rendering_settings_default();
    r.ambient_color[0] = 0.25f;
    r.ambient_color[1] = 0.5f;
    r.ambient_color[2] = 0.75f;
    r.ambient_intensity = 1.6f;
    r.fog_enabled = true;
    r.fog_mode = JCE_SCENE_FOG_LINEAR;
    r.fog_color[0] = 0.6f;
    r.fog_density = 0.033f;
    r.fog_start = 12.0f;
    r.fog_end = 180.0f;
    r.fog_height_falloff = 0.12f;
    r.shadow_distance = 64.0f;
    r.cascade_count = 2;
    r.split_lambda = 0.8f;
    r.shadow_resolution = 1024;
    r.postfx_enabled[1] = true;
    r.bloom_intensity = 0.9f;
    jce_scene_set_rendering_settings(src, &r);

    JceJson *root = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(root);

    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(0, loaded);
    TEST_ASSERT_TRUE(jce_scene_has_rendering_settings(dst));

    const JceSceneRenderingSettings *out = jce_scene_get_rendering_settings(dst);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_FOG_LINEAR, out->fog_mode);
    TEST_ASSERT_TRUE(out->fog_enabled);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.25f, out->ambient_color[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.6f, out->ambient_intensity);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.033f, out->fog_density);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 12.0f, out->fog_start);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 180.0f, out->fog_end);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 64.0f, out->shadow_distance);
    TEST_ASSERT_EQUAL_INT(2, out->cascade_count);
    TEST_ASSERT_EQUAL_INT(1024, out->shadow_resolution);
    TEST_ASSERT_TRUE(out->postfx_enabled[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.9f, out->bloom_intensity);

    jce_json_free(root);
    jce_scene_destroy(dst);
    jce_scene_destroy(src);
}

static void test_look_profile_round_trip_in_scene_json(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    TEST_ASSERT_NOT_NULL(src); TEST_ASSERT_NOT_NULL(dst);

    JceSceneRenderingSettings r = jce_scene_rendering_settings_default();
    r.wrap_factor = 0.35f;
    r.ambient_hemisphere = true;
    r.ambient_ground_color[0] = 0.20f;
    r.ambient_ground_color[1] = 0.12f;
    r.ambient_ground_color[2] = 0.05f;
    r.rim_color[0] = 0.9f; r.rim_color[1] = 0.8f; r.rim_color[2] = 0.6f;
    r.rim_power = 3.0f;
    r.rim_intensity = 0.7f;
    r.tonemap_op = JCE_TONEMAP_AGX;
    snprintf(r.lut_path, sizeof(r.lut_path), "luts/golden_hour.png");
    r.lut_strength = 0.8f;
    r.toon_character = true;
    r.bloom_knee = 0.5f;
    jce_scene_set_rendering_settings(src, &r);

    JceJson *root = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_INT(0, jce_scene_load_json(dst, root));

    const JceSceneRenderingSettings *out = jce_scene_get_rendering_settings(dst);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.35f, out->wrap_factor);
    TEST_ASSERT_TRUE(out->ambient_hemisphere);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.20f, out->ambient_ground_color[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.9f, out->rim_color[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 3.0f, out->rim_power);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.7f, out->rim_intensity);
    TEST_ASSERT_EQUAL_INT(JCE_TONEMAP_AGX, out->tonemap_op);
    TEST_ASSERT_EQUAL_STRING("luts/golden_hour.png", out->lut_path);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.8f, out->lut_strength);
    TEST_ASSERT_TRUE(out->toon_character);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, out->bloom_knee);

    jce_json_free(root);
    jce_scene_destroy(dst); jce_scene_destroy(src);
}

static void test_scene_json_without_look_keeps_defaults(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    TEST_ASSERT_NOT_NULL(src); TEST_ASSERT_NOT_NULL(dst);

    /* author a scene, then strip the "look" object from its JSON. */
    JceSceneRenderingSettings r = jce_scene_rendering_settings_default();
    r.ambient_intensity = 1.3f;
    jce_scene_set_rendering_settings(src, &r);
    JceJson *root = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(root);
    /* find scene.rendering and delete "look" (simulate a pre-feature file). */
    JceJson *scene = jce_json_get(root, "scene");
    JceJson *rendering = scene ? jce_json_get(scene, "rendering") : NULL;
    if (rendering) jce_json_remove(rendering, "look");

    TEST_ASSERT_EQUAL_INT(0, jce_scene_load_json(dst, root));
    const JceSceneRenderingSettings *out = jce_scene_get_rendering_settings(dst);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, out->wrap_factor);
    TEST_ASSERT_FALSE(out->ambient_hemisphere);
    TEST_ASSERT_EQUAL_INT(JCE_TONEMAP_ACES, out->tonemap_op);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.3f, out->ambient_intensity);

    jce_json_free(root);
    jce_scene_destroy(dst); jce_scene_destroy(src);
}

static void test_scene_json_without_rendering_clears_prior_settings(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceSceneRenderingSettings r = jce_scene_rendering_settings_default();
    r.fog_enabled = true;
    r.fog_mode = JCE_SCENE_FOG_EXP;
    jce_scene_set_rendering_settings(s, &r);
    TEST_ASSERT_TRUE(jce_scene_has_rendering_settings(s));

    JceJson *root = jce_json_object();
    JceJson *scene = jce_json_object();
    JceJson *entities = jce_json_array();
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_NOT_NULL(entities);
    jce_json_set_child(root, "scene", scene);
    jce_json_set_child(scene, "entities", entities);

    int loaded = jce_scene_load_json(s, root);
    TEST_ASSERT_EQUAL_INT(0, loaded);
    TEST_ASSERT_FALSE(jce_scene_has_rendering_settings(s));

    jce_json_free(root);
    jce_scene_destroy(s);
}

static void test_additive_json_without_rendering_keeps_prior_settings(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_EQUAL(0, jce_scene_create_entity(s, "Existing"));

    JceSceneRenderingSettings r = jce_scene_rendering_settings_default();
    r.ambient_intensity = 1.7f;
    jce_scene_set_rendering_settings(s, &r);
    TEST_ASSERT_TRUE(jce_scene_has_rendering_settings(s));

    JceJson *root = jce_json_object();
    JceJson *scene = jce_json_object();
    JceJson *entities = jce_json_array();
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_NOT_NULL(entities);
    jce_json_set_child(root, "scene", scene);
    jce_json_set_child(scene, "entities", entities);

    int loaded = jce_scene_load_json(s, root);
    TEST_ASSERT_EQUAL_INT(0, loaded);
    TEST_ASSERT_TRUE(jce_scene_has_rendering_settings(s));
    const JceSceneRenderingSettings *out =
        jce_scene_get_rendering_settings(s);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.7f, out->ambient_intensity);

    jce_json_free(root);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_settings_are_conservative);
    RUN_TEST(test_look_profile_defaults_are_neutral);
    RUN_TEST(test_scene_clear_drops_rendering_settings);
    RUN_TEST(test_rendering_settings_round_trip_in_scene_json);
    RUN_TEST(test_look_profile_round_trip_in_scene_json);
    RUN_TEST(test_scene_json_without_look_keeps_defaults);
    RUN_TEST(test_scene_json_without_rendering_clears_prior_settings);
    RUN_TEST(test_additive_json_without_rendering_keeps_prior_settings);
    return UNITY_END();
}
