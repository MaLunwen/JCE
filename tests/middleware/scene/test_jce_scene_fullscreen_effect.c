#include "unity.h"

#include <jce/api_scene.h>
#include <jce/middleware/scene/jce_component_registry.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>
#include <jce/renderer/jce_scene_renderer.h>

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static JceSceneFullscreenEffect authored(void)
{
    JceSceneFullscreenEffect value = jce_scene_fullscreen_effect_default();
    value.enabled = true;
    value.required = true;
    value.use_scene_color = true;
    value.use_scene_depth = true;
    value.use_history = true;
    value.texture_count = 4u;
    value.order = -7;
    value.insertion = JCE_FULLSCREEN_EFFECT_HDR_BEFORE_POSTFX;
    value.blend = JCE_FULLSCREEN_EFFECT_REPLACE;
    value.output_format = JCE_RENDER_FORMAT_RGBA16F;
    value.resolution_scale = 1.0f;
    strcpy(value.shader, "scientific_probe");
    for (uint32_t i = 0; i < JCE_FULLSCREEN_EFFECT_MAX_TEXTURES; ++i) {
        snprintf(value.textures[i], sizeof(value.textures[i]),
                 "textures/probe_%u.jceasset", i);
        value.samplers[i].address_u = i % 3u;
        value.samplers[i].address_v = (i + 1u) % 3u;
        value.samplers[i].filter_min = i % 2u;
        value.samplers[i].filter_mag = i % 2u;
        value.samplers[i].filter_mip = (i + 1u) % 2u;
    }
    for (uint32_t row = 0; row < JCE_FULLSCREEN_EFFECT_MAX_PARAMS; ++row)
        for (uint32_t col = 0; col < 4u; ++col)
            value.params[row][col] = (float)(row * 4u + col) * 0.25f;
    return value;
}

typedef struct FindCtx {
    const char *name;
    JceEntity entity;
} FindCtx;

static void find_cb(JceScene *scene, JceEntity entity, void *userdata)
{
    FindCtx *ctx = userdata;
    const char *name = jce_scene_entity_registered_name(scene, entity);
    if (name && strcmp(name, ctx->name) == 0)
        ctx->entity = entity;
}

static JceEntity find_by_name(JceScene *scene, const char *name)
{
    FindCtx ctx = { name, JCE_ENTITY_INVALID };
    jce_scene_each_entity(scene, find_cb, &ctx);
    return ctx.entity;
}

static void test_defaults_are_neutral(void)
{
    JceSceneFullscreenEffect value = jce_scene_fullscreen_effect_default();
    TEST_ASSERT_EQUAL_UINT32(sizeof(value), value.struct_size);
    TEST_ASSERT_EQUAL_UINT32(JCE_FULLSCREEN_EFFECT_VERSION, value.version);
    TEST_ASSERT_FALSE(value.enabled);
    TEST_ASSERT_EQUAL_UINT32(JCE_RENDER_FORMAT_RGBA16F, value.output_format);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, value.resolution_scale);
}

static void test_typed_accessors_preserve_every_field(void)
{
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    JceEntity entity = jce_scene_create_entity(scene, "Effect");
    JceSceneFullscreenEffect input = authored();
    jce_scene_set_fullscreen_effect(scene, entity, &input);
    TEST_ASSERT_TRUE(jce_scene_has_fullscreen_effect(scene, entity));
    JceSceneFullscreenEffect *output =
        jce_scene_get_fullscreen_effect(scene, entity);
    TEST_ASSERT_NOT_NULL(output);
    TEST_ASSERT_EQUAL_MEMORY(&input, output, sizeof(input));
    jce_scene_remove_fullscreen_effect(scene, entity);
    TEST_ASSERT_FALSE(jce_scene_has_fullscreen_effect(scene, entity));
    jce_scene_destroy(scene);
}

static void test_json_round_trip_and_dense_presence(void)
{
    JceScene *scene = jce_scene_create();
    JceEntity entity = jce_scene_create_entity(scene, "Effect");
    JceSceneFullscreenEffect input = authored();
    jce_scene_set_fullscreen_effect(scene, entity, &input);
    int component_id = jce_component_find("FullscreenEffect");
    TEST_ASSERT_NOT_EQUAL(JCE_COMP_ID_INVALID, component_id);
    TEST_ASSERT_EQUAL_UINT64(0, jce_component_legacy_flag(component_id));
    TEST_ASSERT_TRUE(jce_scene_has_comp(scene, entity, component_id));
    JceTransform transform;
    memset(&transform, 0, sizeof(transform));
    transform.position = jce_v3(3.0f, -2.0f, 7.0f);
    transform.rotation = (jce_quat){ 0.0f, 0.38268343f, 0.0f, 0.92387953f };
    transform.scale = jce_v3(1.0f, 1.0f, 1.0f);
    jce_scene_set_transform(scene, entity, &transform);
    jce_scene_set_comp_enabled(scene, entity, component_id, false);
    TEST_ASSERT_FALSE(jce_scene_comp_enabled(scene, entity, component_id));
    JceJson *json = jce_scene_save_json(scene);
    TEST_ASSERT_NOT_NULL(json);
    char *printed = jce_json_print(json, false);
    TEST_ASSERT_NOT_NULL(printed);
    TEST_ASSERT_NOT_NULL(strstr(printed, "FullscreenEffect"));

    JceScene *loaded = jce_scene_create();
    TEST_ASSERT_EQUAL_INT(1, jce_scene_load_json(loaded, json));
    JceEntity loaded_entity = find_by_name(loaded, "Effect");
    JceSceneFullscreenEffect *output =
        jce_scene_get_fullscreen_effect(loaded, loaded_entity);
    TEST_ASSERT_NOT_NULL(output);
    TEST_ASSERT_EQUAL_MEMORY(&input, output, sizeof(input));
    TEST_ASSERT_TRUE(jce_scene_has_comp(loaded, loaded_entity, component_id));
    TEST_ASSERT_FALSE(jce_scene_comp_enabled(loaded, loaded_entity, component_id));
    const JceTransform *loaded_transform =
        jce_scene_get_transform(loaded, loaded_entity);
    TEST_ASSERT_NOT_NULL(loaded_transform);
    const float *expected_transform = (const float *)&transform;
    const float *actual_transform = (const float *)loaded_transform;
    for (size_t i = 0; i < sizeof(transform) / sizeof(float); ++i)
        TEST_ASSERT_FLOAT_WITHIN(1.0e-6f,
            expected_transform[i], actual_transform[i]);
    jce_json_free_string(printed);
    jce_json_free(json);
    jce_scene_destroy(loaded);
    jce_scene_destroy(scene);
}

static void test_active_query_honours_all_enable_layers(void)
{
    JceScene *scene = jce_scene_create();
    JceEntity entity = jce_scene_create_entity(scene, "Effect");
    JceSceneFullscreenEffect value = authored();
    int component_id = jce_component_find("FullscreenEffect");

    jce_scene_set_fullscreen_effect(scene, entity, &value);
    TEST_ASSERT_TRUE(jce_scene_renderer_has_fullscreen_effect(scene));

    value.enabled = false;
    jce_scene_set_fullscreen_effect(scene, entity, &value);
    TEST_ASSERT_FALSE(jce_scene_renderer_has_fullscreen_effect(scene));

    value.enabled = true;
    jce_scene_set_fullscreen_effect(scene, entity, &value);
    jce_scene_set_comp_enabled(scene, entity, component_id, false);
    TEST_ASSERT_FALSE(jce_scene_renderer_has_fullscreen_effect(scene));

    jce_scene_set_comp_enabled(scene, entity, component_id, true);
    JceEditorMeta meta = {0};
    meta.enabled = false;
    jce_scene_set_editor_meta(scene, entity, &meta);
    TEST_ASSERT_FALSE(jce_scene_renderer_has_fullscreen_effect(scene));

    jce_scene_destroy(scene);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_are_neutral);
    RUN_TEST(test_typed_accessors_preserve_every_field);
    RUN_TEST(test_json_round_trip_and_dense_presence);
    RUN_TEST(test_active_query_honours_all_enable_layers);
    return UNITY_END();
}
