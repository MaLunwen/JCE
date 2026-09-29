/* test_jce_render_pipeline_tier_clamp.c
 *
 * The LOW-tier floor exists so a shipped .rp.json asking for HIGH features
 * cannot hand half-float render targets, TAA, SSR and four shadow cascades to
 * hardware that cannot afford them.  It is implemented as a clamp inside
 * jce_render_pipeline_apply(), which reads jce_renderer_get_tier().
 *
 * That makes it ORDER DEPENDENT, and the dependence is invisible: apply the
 * pipeline before the hardware tier has been resolved and the clamp reads a
 * higher tier, skips, and the unclamped descriptor stands for the rest of the
 * process.  Observed in practice as the same LOW-tier build behaving
 * differently in two hosts -- one logging the clamped floor, the other logging
 * csm=1 shadow=2048 post=high hdr=1 while the caps line said tier LOW.
 *
 * What has to hold is the property, not one call order: whenever the engine
 * settles on the LOW tier, the ACTIVE pipeline is clamped to the floor --
 * whether the tier was known before the pipeline was applied or only after.
 */

#include <jce/renderer/jce_render_pipeline.h>
#include <jce/renderer/jce_renderer_caps.h>

#include "unity.h"
#include <string.h>

void setUp(void)    {}
void tearDown(void) { jce_renderer_clear_tier_override(); }

/* Every field the LOW floor is supposed to hold down. */
static void assert_low_floor_held(const char *when)
{
    JceRenderPipelineDesc d;
    jce_render_pipeline_get(&d);
    char msg[128];

    snprintf(msg, sizeof msg, "%s: hdr_color must be off on the LOW floor", when);
    TEST_ASSERT_FALSE_MESSAGE(d.hdr_color, msg);
    snprintf(msg, sizeof msg, "%s: TAA must be off on the LOW floor", when);
    TEST_ASSERT_FALSE_MESSAGE(d.enable_taa, msg);
    snprintf(msg, sizeof msg, "%s: SSR must be off on the LOW floor", when);
    TEST_ASSERT_FALSE_MESSAGE(d.enable_ssr, msg);
    snprintf(msg, sizeof msg, "%s: cascades must collapse to 1", when);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, d.csm_cascade_count, msg);
    snprintf(msg, sizeof msg, "%s: post quality must drop to LOW", when);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)JCE_RP_QUALITY_LOW, (int)d.post_quality, msg);
}

/* Tier known FIRST, then the project's HIGH descriptor is applied. */
static void test_low_floor_clamps_when_tier_is_known_before_apply(void)
{
    jce_renderer_set_tier_override(JCE_GPU_TIER_LOW);

    JceRenderPipelineDesc high;
    jce_render_pipeline_preset_high(&high);
    jce_render_pipeline_apply(&high);

    assert_low_floor_held("tier before apply");
}

/* The order that actually breaks: the descriptor is applied while the tier
 * still reads high (caps not resolved yet), and the hardware turns out to be
 * LOW only afterwards.  The floor must still end up held. */
static void test_low_floor_clamps_when_tier_is_known_after_apply(void)
{
    jce_renderer_set_tier_override(JCE_GPU_TIER_HIGH);

    JceRenderPipelineDesc high;
    jce_render_pipeline_preset_high(&high);
    jce_render_pipeline_apply(&high);

    /* Hardware turns out to be the low tier after the fact. */
    jce_renderer_set_tier_override(JCE_GPU_TIER_LOW);

    assert_low_floor_held("tier after apply");
}

/* The floor must not leak upward: a HIGH machine keeps what it asked for. */
static void test_high_tier_keeps_the_requested_features(void)
{
    jce_renderer_set_tier_override(JCE_GPU_TIER_HIGH);

    JceRenderPipelineDesc high;
    jce_render_pipeline_preset_high(&high);
    jce_render_pipeline_apply(&high);

    JceRenderPipelineDesc d;
    jce_render_pipeline_get(&d);
    TEST_ASSERT_TRUE(d.hdr_color);
    TEST_ASSERT_TRUE(d.enable_taa);
    TEST_ASSERT_TRUE(d.csm_cascade_count > 1);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_low_floor_clamps_when_tier_is_known_before_apply);
    RUN_TEST(test_low_floor_clamps_when_tier_is_known_after_apply);
    RUN_TEST(test_high_tier_keeps_the_requested_features);
    return UNITY_END();
}
