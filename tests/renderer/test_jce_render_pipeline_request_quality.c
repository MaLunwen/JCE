/* Real pipeline request/active lifecycle with controlled caps providers.
 * The production jce_render_pipeline.c is compiled unchanged into this target.
 * No graphics device, shader, mirrored resolver, or private state access. */
#include "unity.h"
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <string.h>

static JceRendererBackend fixture_backend;
static JceGpuTier fixture_tier;
static unsigned observer_calls;
static bool queue_reentrant_change;
static bool cold_start;

JceRendererBackend jce_renderer_get_active_backend(void) { return fixture_backend; }
JceGpuTier jce_renderer_get_tier(void) { return fixture_tier; }
uint32_t jce_renderer_get_caps(void)
{
    return JCE_CAP_TEXTURE_FLOAT | JCE_CAP_FRAMEBUFFER_FLOAT;
}
JceRenderRecommendation jce_renderer_get_recommendation(void)
{
    JceRenderRecommendation result;
    memset(&result, 0, sizeof(result));
    result.has_discrete_gpu = fixture_tier >= JCE_GPU_TIER_MEDIUM;
    return result;
}
const char *jce_gpu_tier_name(JceGpuTier tier)
{
    (void)tier;
    return "fixture";
}

static JceRenderPipelineDesc active_desc(void)
{
    JceRenderPipelineDesc result;
    jce_render_pipeline_get(&result);
    return result;
}
static void apply_mid(void)
{
    JceRenderPipelineDesc requested;
    jce_render_pipeline_preset_mid(&requested);
    jce_render_pipeline_apply(&requested);
}
static void observe(const JceRenderPipelineDesc *desc, void *user)
{
    (void)desc;
    (void)user;
    ++observer_calls;
    if (queue_reentrant_change) {
        queue_reentrant_change = false;
        jce_render_pipeline_set_knob("render_scale", 0.8f);
    }
}

void setUp(void)
{
    JceRenderPipelineDesc requested;
    jce_render_pipeline_set_observer(NULL, NULL);
    jce_render_pipeline_end_frame();
    fixture_backend = JCE_BACKEND_OPENGLES;
    fixture_tier = JCE_GPU_TIER_MEDIUM;
    observer_calls = 0;
    queue_reentrant_change = false;
    if (!cold_start) {
        jce_render_pipeline_preset_low(&requested);
        jce_render_pipeline_apply(&requested);
    }
}
void tearDown(void)
{
    jce_render_pipeline_set_observer(NULL, NULL);
    jce_render_pipeline_end_frame();
}

static void test_cold_pending_request_uses_existing_low_defaults(void)
{
    TEST_ASSERT_FALSE(jce_render_pipeline_is_feature_enabled("stylized_look"));
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 1.0f, active_desc().render_scale);
    jce_render_pipeline_set_feature_enabled("bloom", true);
    jce_render_pipeline_end_frame();
    TEST_ASSERT_TRUE(active_desc().enable_bloom);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.00001f, 1.0f, active_desc().render_scale,
        "Cold deferred setters must start from the existing LOW preset, not zero bytes");
    TEST_ASSERT_EQUAL_UINT(512u, active_desc().shadow_resolution);
    TEST_ASSERT_EQUAL_INT(JCE_RP_QUALITY_LOW, active_desc().post_quality);
}
static void test_gles_mid_material_intent_survives_low_bloom_budget(void)
{
    apply_mid();
    TEST_ASSERT_EQUAL_INT(JCE_RP_QUALITY_LOW, active_desc().post_quality);
    TEST_ASSERT_TRUE_MESSAGE(jce_render_pipeline_is_feature_enabled("stylized_look"),
        "Requested MID material look must survive the GLES bloom quality clamp");
}
static void test_explicit_low_material_intent_stays_neutral(void)
{
    TEST_ASSERT_EQUAL_INT(JCE_RP_QUALITY_LOW, active_desc().post_quality);
    TEST_ASSERT_FALSE(jce_render_pipeline_is_feature_enabled("stylized_look"));
}
static void test_low_hardware_floor_does_not_destroy_requested_material_look(void)
{
    fixture_backend = JCE_BACKEND_D3D11;
    fixture_tier = JCE_GPU_TIER_LOW;
    apply_mid();
    TEST_ASSERT_EQUAL_INT(JCE_RP_QUALITY_LOW, active_desc().post_quality);
    TEST_ASSERT_TRUE(jce_render_pipeline_is_feature_enabled("stylized_look"));
    fixture_tier = JCE_GPU_TIER_HIGH;
    jce_render_pipeline_notify_tier_changed();
    TEST_ASSERT_EQUAL_INT(JCE_RP_QUALITY_MID, active_desc().post_quality);
}
static void test_deferred_feature_change_survives_late_tier_reapply(void)
{
    apply_mid();
    jce_render_pipeline_set_feature_enabled("bloom", false);
    TEST_ASSERT_TRUE(active_desc().enable_bloom);
    jce_render_pipeline_end_frame();
    TEST_ASSERT_FALSE(active_desc().enable_bloom);
    fixture_backend = JCE_BACKEND_D3D11;
    fixture_tier = JCE_GPU_TIER_HIGH;
    jce_render_pipeline_notify_tier_changed();
    TEST_ASSERT_FALSE_MESSAGE(active_desc().enable_bloom,
        "Late tier reapply must retain the committed user feature change");
    TEST_ASSERT_EQUAL_INT(JCE_RP_QUALITY_MID, active_desc().post_quality);
}
static void test_deferred_mid_request_is_frame_safe_and_reclamped(void)
{
    jce_render_pipeline_set_knob("post_quality", (float)JCE_RP_QUALITY_MID);
    TEST_ASSERT_FALSE(jce_render_pipeline_is_feature_enabled("stylized_look"));
    TEST_ASSERT_EQUAL_INT(JCE_RP_QUALITY_LOW, active_desc().post_quality);
    jce_render_pipeline_end_frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_RP_QUALITY_LOW, active_desc().post_quality,
        "Deferred commit must retain the effective GLES bloom budget");
    TEST_ASSERT_TRUE(jce_render_pipeline_is_feature_enabled("stylized_look"));
    fixture_backend = JCE_BACKEND_D3D11;
    jce_render_pipeline_notify_tier_changed();
    TEST_ASSERT_EQUAL_INT(JCE_RP_QUALITY_MID, active_desc().post_quality);
}
static void test_deferred_low_request_replaces_mid_even_when_active_quality_matches(void)
{
    apply_mid();
    jce_render_pipeline_set_knob("post_quality", (float)JCE_RP_QUALITY_LOW);
    jce_render_pipeline_end_frame();
    TEST_ASSERT_FALSE(jce_render_pipeline_is_feature_enabled("stylized_look"));
    fixture_backend = JCE_BACKEND_D3D11;
    fixture_tier = JCE_GPU_TIER_HIGH;
    jce_render_pipeline_notify_tier_changed();
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_RP_QUALITY_LOW, active_desc().post_quality,
        "A LOW request must commit even when the old active bloom was already LOW");
}
static void test_pending_request_survives_tier_notification_before_commit(void)
{
    fixture_backend = JCE_BACKEND_D3D11;
    apply_mid();
    jce_render_pipeline_set_knob("post_quality", (float)JCE_RP_QUALITY_HIGH);
    fixture_tier = JCE_GPU_TIER_LOW;
    jce_render_pipeline_notify_tier_changed();
    TEST_ASSERT_EQUAL_INT(JCE_RP_QUALITY_LOW, active_desc().post_quality);
    jce_render_pipeline_end_frame();
    TEST_ASSERT_EQUAL_INT(JCE_RP_QUALITY_LOW, active_desc().post_quality);
    fixture_tier = JCE_GPU_TIER_HIGH;
    jce_render_pipeline_notify_tier_changed();
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_RP_QUALITY_HIGH, active_desc().post_quality,
        "A pending user request must survive a hardware notification");
}
static void test_observer_reentrant_request_is_retained_for_next_frame(void)
{
    fixture_backend = JCE_BACKEND_D3D11;
    apply_mid();
    jce_render_pipeline_set_observer(observe, NULL);
    observer_calls = 0;
    jce_render_pipeline_set_knob("render_scale", 0.75f);
    queue_reentrant_change = true;
    jce_render_pipeline_end_frame();
    TEST_ASSERT_EQUAL_UINT(1u, observer_calls);
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.75f, active_desc().render_scale);
    jce_render_pipeline_end_frame();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2u, observer_calls,
        "Observer's new request must be retained for the following end-frame");
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.8f, active_desc().render_scale);
}
static void test_noop_pending_change_does_not_notify_observer(void)
{
    fixture_backend = JCE_BACKEND_D3D11;
    apply_mid();
    jce_render_pipeline_set_observer(observe, NULL);
    observer_calls = 0;
    jce_render_pipeline_set_feature_enabled("bloom", true);
    jce_render_pipeline_end_frame();
    TEST_ASSERT_EQUAL_UINT(0u, observer_calls);
}
static void test_explicit_full_apply_replaces_older_pending_request(void)
{
    fixture_backend = JCE_BACKEND_D3D11;
    apply_mid();
    jce_render_pipeline_set_knob("render_scale", 0.5f);
    JceRenderPipelineDesc replacement;
    jce_render_pipeline_preset_high(&replacement);
    replacement.render_scale = 0.9f;
    replacement.enable_bloom = false;
    jce_render_pipeline_apply(&replacement);
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.9f, active_desc().render_scale);
    jce_render_pipeline_end_frame();
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.00001f, 0.9f, active_desc().render_scale,
        "A newer explicit complete apply must replace the older pending snapshot");
    TEST_ASSERT_EQUAL_INT(JCE_RP_QUALITY_HIGH, active_desc().post_quality);
    TEST_ASSERT_FALSE(active_desc().enable_bloom);
    jce_render_pipeline_notify_tier_changed();
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.9f, active_desc().render_scale);
}
static void test_boolean_force_off_outranks_requested_mid(void)
{
    apply_mid();
    TEST_ASSERT_EQUAL_INT(JCE_RP_QUALITY_LOW, active_desc().post_quality);
    TEST_ASSERT_FALSE(jce_render_pipeline_is_feature_enabled("stylized_look"));
}
static void test_boolean_force_on_outranks_explicit_low(void)
{
    TEST_ASSERT_EQUAL_INT(JCE_RP_QUALITY_LOW, active_desc().post_quality);
    TEST_ASSERT_TRUE(jce_render_pipeline_is_feature_enabled("stylized_look"));
}
static void test_supported_numeric_force_is_reapplied_at_deferred_commit(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.75f, active_desc().render_scale);
    jce_render_pipeline_set_knob("render_scale", 0.5f);
    jce_render_pipeline_end_frame();
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.00001f, 0.75f, active_desc().render_scale,
        "Supported numeric force must still outrank deferred user settings");
    jce_render_pipeline_notify_tier_changed();
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.75f, active_desc().render_scale);
}

int main(int argc, char **argv)
{
    cold_start = argc > 1 && strcmp(argv[1], "cold") == 0;
    UNITY_BEGIN();
    if (cold_start)
        RUN_TEST(test_cold_pending_request_uses_existing_low_defaults);
    else if (argc > 1 && strcmp(argv[1], "force-off") == 0)
        RUN_TEST(test_boolean_force_off_outranks_requested_mid);
    else if (argc > 1 && strcmp(argv[1], "force-on") == 0)
        RUN_TEST(test_boolean_force_on_outranks_explicit_low);
    else if (argc > 1 && strcmp(argv[1], "force-numeric") == 0)
        RUN_TEST(test_supported_numeric_force_is_reapplied_at_deferred_commit);
    else {
        RUN_TEST(test_gles_mid_material_intent_survives_low_bloom_budget);
        RUN_TEST(test_explicit_low_material_intent_stays_neutral);
        RUN_TEST(test_low_hardware_floor_does_not_destroy_requested_material_look);
        RUN_TEST(test_deferred_feature_change_survives_late_tier_reapply);
        RUN_TEST(test_deferred_mid_request_is_frame_safe_and_reclamped);
        RUN_TEST(test_deferred_low_request_replaces_mid_even_when_active_quality_matches);
        RUN_TEST(test_pending_request_survives_tier_notification_before_commit);
        RUN_TEST(test_observer_reentrant_request_is_retained_for_next_frame);
        RUN_TEST(test_noop_pending_change_does_not_notify_observer);
        RUN_TEST(test_explicit_full_apply_replaces_older_pending_request);
    }
    return UNITY_END();
}
