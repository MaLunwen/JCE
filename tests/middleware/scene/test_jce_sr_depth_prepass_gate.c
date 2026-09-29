/*
 * test_jce_sr_depth_prepass_gate.c — a render pipeline may ask for the camera
 * depth pre-pass, and until now asking did nothing.
 *
 * JceRenderPipelineDesc.depth_prepass is set by the HIGH and ULTRA presets,
 * serialised in .rp.json, toggled in the editor's Render Pipeline viewer, and
 * jce_render_pipeline_is_feature_enabled() has always answered for
 * "depth_prepass".  Nothing asked.  The pass ran iff SSAO, SSR, TAA velocity
 * or water wanted it, so the sampleable scene depth this engine offers -- its
 * _CameraDepthTexture -- existed only by coincidence, and a pipeline that
 * needs one outright (soft particles, depth-aware effects, a depth-driven
 * cull) had no way to say so.
 *
 * The gate is now sr_wants_depth_prepass(), which is why this file can exist:
 * it needs no renderer, no bgfx and no window, only an applied pipeline
 * descriptor.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "middleware/scene/jce_sr_internal.h"

#include "unity.h"

#include <jce/renderer/jce_render_pipeline.h>
#include <jce/renderer/jce_renderer_caps.h>

/* The LOW-tier floor CLEARS depth_prepass -- the pre-pass is a full
 * depth-only draw of the visible set and the iGPU floor exists to drop
 * exactly that.  Correct, and it means "the flag reaches the gate" can only
 * be asked above the floor, so the tier is pinned rather than inherited from
 * whatever GPU happens to be running the test. */
static void apply_with_prepass(bool on)
{
    jce_renderer_set_tier_override(JCE_GPU_TIER_HIGH);
    JceRenderPipelineDesc d;
    jce_render_pipeline_preset_low(&d);   /* preset LOW leaves it false */
    d.depth_prepass = on;
    jce_render_pipeline_apply(&d);
}

void setUp(void) {}
void tearDown(void) { jce_renderer_clear_tier_override(); }

static void test_the_pipeline_flag_is_sufficient_on_its_own(void)
{
    /* THE WHOLE DEFECT.  Every scene reason off, the pipeline asking: before
     * this the answer was no, and the flag decided nothing at all. */
    apply_with_prepass(true);
    TEST_ASSERT_TRUE_MESSAGE(
        sr_wants_depth_prepass(false, false, false, false, false, false),
        "a pipeline that declares depth_prepass must get the pre-pass even "
        "with SSAO, SSR, TAA velocity and water all off");
}

static void test_it_is_not_forced_on_when_nobody_asks(void)
{
    /* The negative half, and the reason this is a flag rather than a
     * default: the pass costs a depth-only draw of the whole visible set. */
    apply_with_prepass(false);
    TEST_ASSERT_FALSE_MESSAGE(
        sr_wants_depth_prepass(false, false, false, false, false, false),
        "no requester, no pass -- the pre-pass is not free");
}

static void test_every_scene_reason_still_stands_alone(void)
{
    /* The four original requesters must keep working with the pipeline flag
     * off, or wiring the fifth one quietly took the other four hostage. */
    apply_with_prepass(false);
    TEST_ASSERT_TRUE_MESSAGE(sr_wants_depth_prepass(true, false, false, false, false, false),
                             "SSAO alone still wants it");
    TEST_ASSERT_TRUE_MESSAGE(sr_wants_depth_prepass(false, true, false, false, false, false),
                             "SSR alone still wants it");
    TEST_ASSERT_TRUE_MESSAGE(sr_wants_depth_prepass(false, false, true, false, false, false),
                             "TAA velocity alone still wants it");
    TEST_ASSERT_TRUE_MESSAGE(sr_wants_depth_prepass(false, false, false, true, false, false),
                             "water depth alone still wants it");
    TEST_ASSERT_TRUE_MESSAGE(sr_wants_depth_prepass(false, false, false, false, true, false),
                             "SSGI alone still wants it");
}

static void test_a_planar_probe_asks_for_it_on_its_own(void)
{
    /* THE SIXTH REQUESTER, and the reason it is one.
     *
     * A planar reflection probe composites in screen space: it reconstructs
     * world position from this pass's depth and reads its G-buffer normal to
     * decide which pixels lie on the mirror.  Without the pass it draws
     * nothing -- and a probe that draws nothing looks exactly like a probe
     * out of range, so the failure would have been silent and would have
     * depended on whether the scene happened to have SSR on.
     *
     * That is the half-wired shape this ledger exists to catch, which is why
     * it gets an assertion rather than a comment. */
    apply_with_prepass(false);
    TEST_ASSERT_TRUE_MESSAGE(
        sr_wants_depth_prepass(false, false, false, false, false, true),
        "a planar reflection probe alone must get the pre-pass: its "
        "composite has no depth and no normal without it, and would "
        "silently draw nothing");
}

static void test_the_probe_does_not_force_the_pass_when_absent(void)
{
    /* The negative half.  Wiring a requester that answers yes unconditionally
     * would buy the pass for every scene in the project, and nothing would
     * look wrong -- it would just cost a depth-only draw of the visible set
     * on a machine chosen to avoid exactly that. */
    apply_with_prepass(false);
    TEST_ASSERT_FALSE_MESSAGE(
        sr_wants_depth_prepass(false, false, false, false, false, false),
        "no probe, no pass");
}

static void test_the_high_preset_asks_for_it(void)
{
    /* Not a tautology: the presets are where depth_prepass has been sitting
     * unread, so this pins that the flag those presets set is the same one
     * the gate reads.  LOW and MID leave it off; HIGH and ULTRA turn it on. */
    jce_renderer_set_tier_override(JCE_GPU_TIER_HIGH);
    JceRenderPipelineDesc d;
    jce_render_pipeline_preset_high(&d);
    TEST_ASSERT_TRUE(d.depth_prepass);
    jce_render_pipeline_apply(&d);
    TEST_ASSERT_TRUE(sr_wants_depth_prepass(false, false, false, false, false, false));

    jce_render_pipeline_preset_low(&d);
    TEST_ASSERT_FALSE(d.depth_prepass);
    jce_render_pipeline_apply(&d);
    TEST_ASSERT_FALSE(sr_wants_depth_prepass(false, false, false, false, false, false));
}

static void test_the_low_tier_floor_still_drops_it(void)
{
    /* The floor's promise is a cost promise: on integrated graphics an
     * ULTRA .rp.json does not get to add a full depth-only pass.  Wiring the
     * flag must not have created a way around that -- if this goes red, a
     * shipped pipeline just bought itself a pass on the hardware the floor
     * exists to protect. */
    jce_renderer_set_tier_override(JCE_GPU_TIER_LOW);
    JceRenderPipelineDesc d;
    jce_render_pipeline_preset_ultra(&d);
    TEST_ASSERT_TRUE_MESSAGE(d.depth_prepass,
        "ULTRA asks for it, so the clamp below is what is being measured");
    jce_render_pipeline_apply(&d);
    TEST_ASSERT_FALSE_MESSAGE(
        sr_wants_depth_prepass(false, false, false, false, false, false),
        "the LOW-tier floor must still clear depth_prepass");
    /* ...and the four scene reasons are NOT floored: SSAO on a LOW-tier
     * machine still needs its depth, or AO would silently stop working. */
    TEST_ASSERT_TRUE(sr_wants_depth_prepass(true, false, false, false, false, false));
}

/* SSGI is the sixth requester and had to be a parameter of its own: it reads
 * the pre-pass's NORMAL target, which SSR does not, so answering for both
 * under want_ssr would claim something unchecked.  This is the assertion that
 * would fail if someone folded them back together. */
static void test_ssgi_asks_for_it_on_its_own(void)
{
    apply_with_prepass(false);
    TEST_ASSERT_FALSE_MESSAGE(
        sr_wants_depth_prepass(false, false, false, false, false, false),
        "nobody asked: the pass must stay off");
    TEST_ASSERT_TRUE_MESSAGE(
        sr_wants_depth_prepass(false, false, false, false, true, false),
        "SSGI alone must turn the depth/normal pre-pass on");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_pipeline_flag_is_sufficient_on_its_own);
    RUN_TEST(test_it_is_not_forced_on_when_nobody_asks);
    RUN_TEST(test_every_scene_reason_still_stands_alone);
    RUN_TEST(test_a_planar_probe_asks_for_it_on_its_own);
    RUN_TEST(test_the_probe_does_not_force_the_pass_when_absent);
    RUN_TEST(test_the_high_preset_asks_for_it);
    RUN_TEST(test_the_low_tier_floor_still_drops_it);
    RUN_TEST(test_ssgi_asks_for_it_on_its_own);
    return UNITY_END();
}
