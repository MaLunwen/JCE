/*
 * test_jce_motion_blur_intensity.c — the "0 means the engine default" rule.
 *
 * WHY THIS IS WORTH A TEST FILE.  enable_motion_blur has existed since the
 * descriptor did: set by three editor UIs, serialised to .rp.json, defaulted ON
 * by the ULTRA preset, cooked into the shipped PAK — and read by no render pass
 * at all.  Now that a pass reads it, the trail length arrives beside it, and
 * EVERY .rp.json in existence predates that key.  A missing key memsets to 0.
 *
 * So the failure this file exists to prevent is precise and silent: a preset
 * whose checkbox says ON renders with a trail length of zero, i.e. exactly the
 * unblurred image the feature was written to stop shipping — and it would look
 * identical to the bug that was just fixed, on the same projects, with the
 * toggle still saying yes.
 *
 * The rule therefore lives in ONE function, and this asserts that function
 * rather than the field, because a caller that reads the field directly has to
 * repeat the rule and the first one to forget it reintroduces the defect.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/renderer/jce_render_pipeline.h>
#include <jce/renderer/jce_renderer_caps.h>

#include <string.h>

/* The LOW-tier floor inside apply() force-disables motion blur, and a headless
 * unit test has no GPU, so without pinning the tier every case here would be
 * asserting the floor rather than the rule under test -- and the first one
 * would fail for a reason that has nothing to do with the trail length. */
void setUp(void)    { jce_renderer_set_tier_override(JCE_GPU_TIER_HIGH); }
void tearDown(void) { jce_renderer_clear_tier_override(); }

static JceRenderPipelineDesc ultra(void)
{
    JceRenderPipelineDesc d;
    memset(&d, 0, sizeof d);
    jce_render_pipeline_preset_ultra(&d);
    return d;
}

static JceRenderPipelineDesc high(void)
{
    JceRenderPipelineDesc d;
    memset(&d, 0, sizeof d);
    jce_render_pipeline_preset_high(&d);
    return d;
}

static void test_a_descriptor_that_never_heard_of_the_key_still_blurs(void)
{
    /* Exactly what a .rp.json written before the key existed parses to: the
     * parser starts from a zeroed desc and only overwrites keys the file
     * names, so motion_blur_intensity stays 0. */
    JceRenderPipelineDesc d = ultra();
    d.motion_blur_intensity = 0.0f;
    jce_render_pipeline_apply(&d);

    TEST_ASSERT_TRUE_MESSAGE(jce_render_pipeline_is_feature_enabled("motion_blur"),
        "the ULTRA preset is supposed to turn motion blur on; if it does not, "
        "the rest of this file is testing nothing");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(JCE_RP_MOTION_BLUR_DEFAULT,
        jce_render_pipeline_motion_blur_intensity(),
        "a descriptor that does not state a trail length resolved to 0, which "
        "renders an UNBLURRED frame while the toggle says ON -- the exact "
        "defect this feature was written to close, reintroduced through the "
        "one key every existing .rp.json is missing");
}

static void test_a_stated_length_is_the_one_used(void)
{
    JceRenderPipelineDesc d = ultra();
    d.motion_blur_intensity = 0.35f;
    jce_render_pipeline_apply(&d);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.35f,
        jce_render_pipeline_motion_blur_intensity(),
        "an authored trail length was replaced by the default; the 0-means-"
        "default rule is swallowing real values");

    /* And it is not one-way: a second apply has to move it, or the resolver is
     * reading a cached first answer. */
    d.motion_blur_intensity = 1.75f;
    jce_render_pipeline_apply(&d);
    TEST_ASSERT_EQUAL_FLOAT(1.75f, jce_render_pipeline_motion_blur_intensity());
}

static void test_a_negative_length_is_not_treated_as_authored(void)
{
    /* A hand-edited file, or a UI that lets a slider go under its own minimum.
     * A negative trail smears FORWARD along the motion, which looks like the
     * image is being dragged by something that has not happened yet. */
    JceRenderPipelineDesc d = ultra();
    d.motion_blur_intensity = -2.0f;
    jce_render_pipeline_apply(&d);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(JCE_RP_MOTION_BLUR_DEFAULT,
        jce_render_pipeline_motion_blur_intensity(),
        "a negative trail length was passed through; it reaches a shader loop "
        "and smears the image forward along the motion");
}

static void test_the_toggle_and_the_length_are_separate_facts(void)
{
    /* Turning the feature off must not silently zero the authored length: the
     * next time it is switched on, the artist's number has to still be there.
     * The two live in different fields precisely so this holds. */
    JceRenderPipelineDesc d = high();
    d.enable_motion_blur    = false;
    d.motion_blur_intensity = 0.6f;
    jce_render_pipeline_apply(&d);

    TEST_ASSERT_FALSE(jce_render_pipeline_is_feature_enabled("motion_blur"));
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.6f,
        jce_render_pipeline_motion_blur_intensity(),
        "switching the feature off discarded the authored trail length");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_descriptor_that_never_heard_of_the_key_still_blurs);
    RUN_TEST(test_a_stated_length_is_the_one_used);
    RUN_TEST(test_a_negative_length_is_not_treated_as_authored);
    RUN_TEST(test_the_toggle_and_the_length_are_separate_facts);
    return UNITY_END();
}
