#include "unity.h"

#include <jce/renderer/jce_fullscreen_effect.h>

#include <math.h>
#include <string.h>

void setUp(void) { }
void tearDown(void) { }

static void test_default_is_inert_and_hdr(void)
{
    JceFullscreenEffectPassDesc d =
        jce_fullscreen_effect_pass_desc_default();

    TEST_ASSERT_EQUAL_UINT32(sizeof(d), d.struct_size);
    TEST_ASSERT_EQUAL_UINT32(JCE_FULLSCREEN_EFFECT_ABI_VERSION, d.version);
    TEST_ASSERT_FALSE(d.enabled);
    TEST_ASSERT_EQUAL_UINT32(JCE_FULLSCREEN_EFFECT_HDR_BEFORE_POSTFX,
                             d.insertion);
    TEST_ASSERT_EQUAL_UINT32(JCE_FULLSCREEN_EFFECT_REPLACE, d.blend);
    TEST_ASSERT_EQUAL_UINT32(JCE_RENDER_FORMAT_RGBA16F, d.output_format);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, d.resolution_scale);
}

static void test_sanitize_clamps_bounded_fields(void)
{
    JceFullscreenEffectPassDesc d =
        jce_fullscreen_effect_pass_desc_default();
    d.enabled = true;
    d.texture_count = 99;
    d.insertion = 99;
    d.blend = 99;
    d.output_format = 99;
    d.resolution_scale = 0.001f;
    d.shader[0] = 'x';

    TEST_ASSERT_TRUE(jce_fullscreen_effect_sanitize(&d));
    TEST_ASSERT_EQUAL_UINT8(JCE_FULLSCREEN_EFFECT_MAX_TEXTURES,
                            d.texture_count);
    TEST_ASSERT_EQUAL_UINT32(JCE_FULLSCREEN_EFFECT_HDR_BEFORE_POSTFX,
                             d.insertion);
    TEST_ASSERT_EQUAL_UINT32(JCE_FULLSCREEN_EFFECT_REPLACE, d.blend);
    TEST_ASSERT_EQUAL_UINT32(JCE_RENDER_FORMAT_RGBA16F, d.output_format);
    TEST_ASSERT_EQUAL_FLOAT(0.0625f, d.resolution_scale);

    d.resolution_scale = 5.0f;
    TEST_ASSERT_TRUE(jce_fullscreen_effect_sanitize(&d));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, d.resolution_scale);
}

static void test_sanitize_rejects_bad_abi_and_enabled_empty_shader(void)
{
    JceFullscreenEffectPassDesc d =
        jce_fullscreen_effect_pass_desc_default();
    d.enabled = true;
    TEST_ASSERT_FALSE(jce_fullscreen_effect_sanitize(&d));

    d.shader[0] = 'x';
    d.version = 99;
    TEST_ASSERT_FALSE(jce_fullscreen_effect_sanitize(&d));
}

static void test_target_extent_rounds_and_bounds(void)
{
    uint32_t w = 0, h = 0;
    jce_fullscreen_effect_target_extent(0.5f, 1919, 1079, &w, &h);
    TEST_ASSERT_EQUAL_UINT32(960, w);
    TEST_ASSERT_EQUAL_UINT32(540, h);

    jce_fullscreen_effect_target_extent(0.01f, 1, 0, &w, &h);
    TEST_ASSERT_EQUAL_UINT32(1, w);
    TEST_ASSERT_EQUAL_UINT32(1, h);

    jce_fullscreen_effect_target_extent(4.0f, 10000, 10000, &w, &h);
    TEST_ASSERT_EQUAL_UINT32(8192, w);
    TEST_ASSERT_EQUAL_UINT32(8192, h);
}

static void test_sort_key_is_total_and_deterministic(void)
{
    JceFullscreenEffectSortKey a = { 0, -2, 30 };
    JceFullscreenEffectSortKey b = { 0, -1, 10 };
    TEST_ASSERT_LESS_THAN_INT(0, jce_fullscreen_effect_sort_key_compare(&a, &b));
    b.order = -2;
    TEST_ASSERT_GREATER_THAN_INT(0,
        jce_fullscreen_effect_sort_key_compare(&a, &b));
    b.entity_id = 30;
    TEST_ASSERT_EQUAL_INT(0, jce_fullscreen_effect_sort_key_compare(&a, &b));
}

static void test_required_replace_conflict_is_exact(void)
{
    JceFullscreenEffectPassDesc a =
        jce_fullscreen_effect_pass_desc_default();
    JceFullscreenEffectPassDesc b = a;
    a.enabled = b.enabled = true;
    a.required = b.required = true;
    a.order = b.order = 7;
    TEST_ASSERT_TRUE(jce_fullscreen_effect_required_replace_conflict(&a, &b));
    b.order = 8;
    TEST_ASSERT_FALSE(jce_fullscreen_effect_required_replace_conflict(&a, &b));
    b.order = 7;
    b.required = false;
    TEST_ASSERT_FALSE(jce_fullscreen_effect_required_replace_conflict(&a, &b));
}

static void test_uniform_pack_uses_documented_screen_convention(void)
{
    JceFullscreenEffectPassDesc p =
        jce_fullscreen_effect_pass_desc_default();
    JceFullscreenEffectFrameDesc f =
        jce_fullscreen_effect_frame_desc_default();
    JceFullscreenEffectUniforms u;
    p.params[3][2] = 42.0f;
    f.width = 800;
    f.height = 400;
    f.elapsed_sec = 12.0f;
    f.delta_sec = 0.25f;
    f.frame_index = 17;
    f.camera_position = jce_v3(1.0f, 2.0f, 3.0f);
    f.camera_right = jce_v3(1.0f, 0.0f, 0.0f);
    f.camera_up = jce_v3(0.0f, 1.0f, 0.0f);
    f.camera_forward = jce_v3(0.0f, 0.0f, -1.0f);
    f.tan_half_vertical_fov = 0.5f;
    f.aspect_ratio = 2.0f;

    TEST_ASSERT_TRUE(jce_fullscreen_effect_pack_uniforms(
        &p, &f, 400, 200, true, &u));
    TEST_ASSERT_EQUAL_FLOAT(800.0f, u.viewport[0]);
    TEST_ASSERT_EQUAL_FLOAT(400.0f, u.viewport[1]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f / 800.0f, u.viewport[2]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f / 400.0f, u.viewport[3]);
    TEST_ASSERT_EQUAL_FLOAT(400.0f, u.output_viewport[0]);
    TEST_ASSERT_EQUAL_FLOAT(200.0f, u.output_viewport[1]);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, u.output_viewport[2]);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, u.output_viewport[3]);
    TEST_ASSERT_EQUAL_FLOAT(12.0f, u.time_frame[0]);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, u.time_frame[1]);
    TEST_ASSERT_EQUAL_FLOAT(17.0f, u.time_frame[2]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, u.time_frame[3]);
    TEST_ASSERT_EQUAL_FLOAT(42.0f, u.user_params[3][2]);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, u.camera_basis[2][2]);
}

static void test_history_key_changes_on_contract_inputs(void)
{
    JceFullscreenEffectPassDesc p =
        jce_fullscreen_effect_pass_desc_default();
    JceFullscreenEffectFrameDesc f =
        jce_fullscreen_effect_frame_desc_default();
    strcpy(p.shader, "science");
    p.enabled = true;
    f.width = 640;
    f.height = 360;
    uint64_t a = jce_fullscreen_effect_history_key(&p, &f);
    p.params[0][0] = 1.0f;
    uint64_t b = jce_fullscreen_effect_history_key(&p, &f);
    TEST_ASSERT_NOT_EQUAL(a, b);
    p.params[0][0] = 0.0f;
    f.width = 641;
    TEST_ASSERT_NOT_EQUAL(a, jce_fullscreen_effect_history_key(&p, &f));

    f.width = 640;
    f.view_proj.raw[3][0] = 2.0f;
    TEST_ASSERT_NOT_EQUAL_MESSAGE(a,
        jce_fullscreen_effect_history_key(&p, &f),
        "camera motion must invalidate non-reprojected history");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_is_inert_and_hdr);
    RUN_TEST(test_sanitize_clamps_bounded_fields);
    RUN_TEST(test_sanitize_rejects_bad_abi_and_enabled_empty_shader);
    RUN_TEST(test_target_extent_rounds_and_bounds);
    RUN_TEST(test_sort_key_is_total_and_deterministic);
    RUN_TEST(test_required_replace_conflict_is_exact);
    RUN_TEST(test_uniform_pack_uses_documented_screen_convention);
    RUN_TEST(test_history_key_changes_on_contract_inputs);
    return UNITY_END();
}
