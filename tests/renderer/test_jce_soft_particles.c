/*
 * test_jce_soft_particles.c — Unity's QualitySettings.softParticlesEnabled.
 *
 * Project Settings > Quality has carried a per-level "Soft Particles"
 * checkbox for as long as the tab has existed.  It had a default, a
 * serializer, a deserializer and a widget, and the engine had no depth fade
 * in either particle path to hand it to -- so every smoke and dust billboard
 * cut the floor at a hard straight seam, which is the artefact that gives
 * untreated particles away, and the box was a box.
 *
 * What is asserted here is the part that has no GPU in it: the process-global
 * knob both hosts write, the authored field that carries it into a build, and
 * -- the one that matters most -- that turning it on is SUFFICIENT, because
 * the fade reads the camera depth pre-pass and a feature that works only when
 * SSAO happens to be on as well is worse than one that does not work at all.
 *
 * The shader half is evidenced separately: fs_particle_{dx11,glsl,spv}.bin all
 * carry u_particle_soft and s_sceneDepth after the change and none of them
 * carries either before it.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/renderer/jce_particles.h>
#include <jce/renderer/jce_render_settings.h>
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/renderer/jce_renderer_caps.h>

#include "middleware/scene/jce_sr_depth_prepass.h"

#include <stdio.h>

void setUp(void) { jce_particles_set_soft_fade_distance(0.0f); }
void tearDown(void)
{
    jce_particles_set_soft_fade_distance(0.0f);
    jce_renderer_clear_tier_override();
}

static void test_off_is_the_default_and_the_old_behaviour(void)
{
    /* THE LOAD-BEARING CASE.  Every project that never opened the Quality tab
     * must render particles exactly as before: fade 0 makes the shader skip
     * its whole soft block, so sampler stage 1 is never even read. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_particles_get_soft_fade_distance());

    JceRenderSettings rs = jce_render_settings_default();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rs.soft_particles,
        "an unauthored project must default to the hard billboard edge");
}

static void test_the_knob_takes_a_distance_and_refuses_nonsense(void)
{
    jce_particles_set_soft_fade_distance(0.75f);
    TEST_ASSERT_EQUAL_FLOAT(0.75f, jce_particles_get_soft_fade_distance());

    /* Negative or zero is "off", not a negative fade: the shader divides by
     * this, and a negative distance would invert the fade so particles would
     * be solid where they touch geometry and invisible in open air. */
    jce_particles_set_soft_fade_distance(-1.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_particles_get_soft_fade_distance());
    jce_particles_set_soft_fade_distance(0.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_particles_get_soft_fade_distance());

    TEST_ASSERT_TRUE_MESSAGE(JCE_PARTICLES_SOFT_FADE_DEFAULT > 0.0f,
        "the constant both hosts map the checkbox to must actually enable it");
}

static void test_turning_it_on_is_sufficient_on_its_own(void)
{
    /* THE WIRING THAT MAKES IT A FEATURE.  The fade reads the camera depth
     * pre-pass.  Before soft particles became one of its requesters, ticking
     * the box did nothing unless SSAO, SSR, TAA velocity or water happened to
     * want the pass too -- a control that works on Tuesdays. */
    jce_renderer_set_tier_override(JCE_GPU_TIER_HIGH);
    JceRenderPipelineDesc d;
    jce_render_pipeline_preset_low(&d);      /* depth_prepass = false */
    jce_render_pipeline_apply(&d);

    TEST_ASSERT_FALSE_MESSAGE(
        sr_wants_depth_prepass(false, false, false, false, false, false),
        "control: with the fade off and nothing else asking, no pre-pass");

    jce_particles_set_soft_fade_distance(JCE_PARTICLES_SOFT_FADE_DEFAULT);
    TEST_ASSERT_TRUE_MESSAGE(
        sr_wants_depth_prepass(false, false, false, false, false, false),
        "soft particles must REQUEST the depth pre-pass, or the checkbox only "
        "works when some other effect happens to be on");
}

static void test_the_authored_value_survives_a_round_trip(void)
{
    /* A setting that does not reach the cooked render_settings.json is a
     * setting that works in the editor and not in the build -- the exact
     * runtime-parity failure this chain exists to avoid. */
    const char *path = "test_soft_particles_rs.json";
    JceRenderSettings w = jce_render_settings_default();
    w.soft_particles = 1;
    TEST_ASSERT_TRUE(jce_render_settings_save_json(path, &w));

    JceRenderSettings r = jce_render_settings_default();
    TEST_ASSERT_TRUE(jce_render_settings_load_json(path, &r));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, r.soft_particles,
        "softParticles must survive save -> load, or it dies at cook");
    remove(path);
}

static void test_a_file_written_before_this_loads_as_off(void)
{
    /* Every render_settings.json on disk predates the key.  Absent must mean
     * off, or this change repaints particles in every existing project. */
    const char *json = "{\"schema\":\"jce.rendersettings.v2\",\"vsync\":1}";
    JceRenderSettings r = jce_render_settings_default();
    r.soft_particles = 1;   /* poisoned, so a no-op parse cannot pass */
    TEST_ASSERT_TRUE(jce_render_settings_load_json_mem(json, 0, &r));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.soft_particles,
        "a file with no softParticles key must load as OFF");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_off_is_the_default_and_the_old_behaviour);
    RUN_TEST(test_the_knob_takes_a_distance_and_refuses_nonsense);
    RUN_TEST(test_turning_it_on_is_sufficient_on_its_own);
    RUN_TEST(test_the_authored_value_survives_a_round_trip);
    RUN_TEST(test_a_file_written_before_this_loads_as_off);
    return UNITY_END();
}
