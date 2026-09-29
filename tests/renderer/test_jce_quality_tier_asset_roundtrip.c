/*
 * test_jce_quality_tier_asset_roundtrip.c
 *
 * MEASUREMENT FIRST.  The Quality tier a user picks in Project Settings is
 * persisted ONLY to <root>/.jce/editor-state.json ("graphics.tier") -- machine-
 * local editor state.  Nothing in engine/, scripts/ or tools/ reads that key,
 * and the build does not export it.  So: pick Ultra, press Apply, watch the
 * viewport change, build the game -- and the game ships whatever
 * Settings/RenderPipeline.rp.json happened to be on disk (a new project gets
 * MID written at creation), with no warning that the tier was dropped.
 *
 * The proposed fix is to have Apply also WRITE that asset, since the build
 * already stages it and the runtime already loads it after bundle mount.  That
 * fix is only honest if a tier preset survives the .rp.json round trip: if the
 * serializer drops fields, writing the asset would carry a DIFFERENT pipeline
 * than the one the editor just applied, and the parity bug would move rather
 * than close.
 *
 * So this measures the round trip before anything is built on it, for all four
 * tiers, field by field -- and it separately asserts the four tiers are
 * actually DISTINCT after the round trip, because a serializer that flattened
 * every tier to the same document would round-trip perfectly and still make
 * the tier meaningless.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/renderer/jce_quality_preset.h>
#include <jce/renderer/jce_render_pipeline.h>

#include <stdio.h>
#include <string.h>

#define RP_FILE "jce_quality_tier_roundtrip.rp.json"

static const JceQualityTier K_TIERS[4] = {
    JCE_QUALITY_LOW, JCE_QUALITY_MED, JCE_QUALITY_HIGH, JCE_QUALITY_ULTRA
};
static const char *const K_NAMES[4] = { "LOW", "MED", "HIGH", "ULTRA" };

void setUp(void)    {}
void tearDown(void) { remove(RP_FILE); }

/* Field-by-field, so a failure names the field instead of "the structs
 * differ".  memcmp would also trip on padding the serializer cannot see. */
static void expect_same_desc(const JceRenderPipelineDesc *a,
                             const JceRenderPipelineDesc *b,
                             const char *tier)
{
    char m[128];
#define SAME_B(f)                                                             \
    snprintf(m, sizeof m, "%s: " #f " lost in the .rp.json round trip", tier); \
    TEST_ASSERT_EQUAL_MESSAGE((int)a->f, (int)b->f, m)
#define SAME_F(f)                                                             \
    snprintf(m, sizeof m, "%s: " #f " lost in the .rp.json round trip", tier); \
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(a->f, b->f, m)

    SAME_B(enable_csm);            SAME_B(enable_ssao);
    SAME_B(enable_ssr);            SAME_B(enable_taa);
    SAME_B(enable_bloom);          SAME_B(enable_volumetric_fog);
    SAME_B(enable_gpu_particles);  SAME_B(enable_motion_blur);
    SAME_B(enable_cloth);          SAME_B(enable_stylized_sky);
    SAME_B(shadow_resolution);     SAME_B(csm_cascade_count);
    SAME_B(shadow_filter_quality); SAME_B(msaa_samples);
    SAME_B(post_quality);          SAME_B(hdr_color);
    SAME_B(depth_prepass);
    SAME_F(render_scale);

    for (int i = 0; i < (int)JCE_RP_PERF_COUNT; ++i) {
        snprintf(m, sizeof m, "%s: perf[%d] lost in the round trip", tier, i);
        TEST_ASSERT_EQUAL_MESSAGE((int)a->perf[i], (int)b->perf[i], m);
    }
#undef SAME_B
#undef SAME_F
}

static void test_every_tier_survives_the_asset_round_trip(void)
{
    for (int i = 0; i < 4; ++i) {
        JceQualityPreset p;
        memset(&p, 0, sizeof p);
        jce_quality_preset_get(K_TIERS[i], &p);

        TEST_ASSERT_TRUE_MESSAGE(jce_render_pipeline_save(RP_FILE, &p.rp),
            "the tier's pipeline must be writable as the asset the build "
            "stages -- if it is not, Apply has nothing to carry");

        JceRenderPipelineDesc back;
        memset(&back, 0xA5, sizeof back);   /* poison: a field the loader never
                                             * touches must not read as equal */
        TEST_ASSERT_TRUE_MESSAGE(jce_render_pipeline_load(RP_FILE, &back),
            "and the runtime must be able to load it back");

        expect_same_desc(&p.rp, &back, K_NAMES[i]);
        remove(RP_FILE);
    }
}

static void test_the_four_tiers_are_distinct_on_disk(void)
{
    /* A serializer that flattened every tier to the same document would pass
     * the round trip above and still make the tier choice meaningless.  Six
     * pairs, each of which must differ somewhere. */
    JceRenderPipelineDesc d[4];
    for (int i = 0; i < 4; ++i) {
        JceQualityPreset p;
        memset(&p, 0, sizeof p);
        jce_quality_preset_get(K_TIERS[i], &p);
        TEST_ASSERT_TRUE(jce_render_pipeline_save(RP_FILE, &p.rp));
        memset(&d[i], 0, sizeof d[i]);
        TEST_ASSERT_TRUE(jce_render_pipeline_load(RP_FILE, &d[i]));
        remove(RP_FILE);
    }
    for (int a = 0; a < 4; ++a)
        for (int b = a + 1; b < 4; ++b) {
            char m[128];
            snprintf(m, sizeof m,
                     "%s and %s round-trip to the SAME pipeline -- writing the "
                     "asset would not carry the tier", K_NAMES[a], K_NAMES[b]);
            TEST_ASSERT_TRUE_MESSAGE(memcmp(&d[a], &d[b], sizeof d[a]) != 0, m);
        }
}

static void test_the_extra_knobs_are_read_by_nobody(void)
{
    /* The three fields beside `rp` -- mip_lod_bias, max_draw_distance,
     * max_active_lights -- are authored per tier, shown in the panel's table,
     * cached by jce_quality_preset_apply, and read by NOTHING: the three
     * jce_quality_get_*() accessors have zero callers in engine/ or editor/.
     *
     * This asserts only the half that is a contract: apply() caches what the
     * preset carried, so a future consumer reads the tier's value and not a
     * stale one.  That they reach no consumer is not something a unit test can
     * state -- it is a grep, and it is recorded in the commit message. */
    JceQualityPreset lo, hi;
    memset(&lo, 0, sizeof lo);
    memset(&hi, 0, sizeof hi);
    jce_quality_preset_get(JCE_QUALITY_LOW,   &lo);
    jce_quality_preset_get(JCE_QUALITY_ULTRA, &hi);

    TEST_ASSERT_TRUE_MESSAGE(lo.max_active_lights < hi.max_active_lights,
        "the tiers must actually differ, or the table in the panel is decor");
    TEST_ASSERT_TRUE_MESSAGE(lo.max_draw_distance < hi.max_draw_distance,
        "same for draw distance");
    TEST_ASSERT_TRUE_MESSAGE(lo.mip_lod_bias > hi.mip_lod_bias,
        "and mip bias goes the other way: negative is sharper");

    jce_quality_preset_apply(&hi);
    TEST_ASSERT_EQUAL_INT_MESSAGE(hi.max_active_lights,
        jce_quality_get_max_active_lights(),
        "apply must cache what the preset carried");
    TEST_ASSERT_EQUAL_FLOAT(hi.max_draw_distance,
                            jce_quality_get_max_draw_distance());
    TEST_ASSERT_EQUAL_FLOAT(hi.mip_lod_bias, jce_quality_get_mip_lod_bias());

    jce_quality_preset_apply(&lo);
    TEST_ASSERT_EQUAL_INT_MESSAGE(lo.max_active_lights,
        jce_quality_get_max_active_lights(),
        "and a second apply must replace them, not keep the first");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_tier_survives_the_asset_round_trip);
    RUN_TEST(test_the_four_tiers_are_distinct_on_disk);
    RUN_TEST(test_the_extra_knobs_are_read_by_nobody);
    return UNITY_END();
}
