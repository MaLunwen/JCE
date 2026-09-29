/*
 * test_jce_csm_splits.c -- the cascade-split vec4 the shadow shader reads.
 *
 * This exists because the packing was a plain loop over cascade_count into a
 * zero-initialised array, and csm_shadow.sh reads the result as BOTH an
 * ordered ladder and a shadow distance:
 *
 *     if      (d < u_csmSplits.x) cascade = 0;
 *     else if (d < u_csmSplits.y) cascade = 1;   // 0 => never taken
 *     else if (d < u_csmSplits.z) cascade = 2;   // 0 => never taken
 *     ...                                        // => cascade 3
 *     float shadow_far = u_csmSplits.w;          // 0 => distance fade dead
 *
 * So on the one- and two-cascade tiers -- LOW, MEDIUM, and every integrated
 * GPU, which jce_render_pipeline.c clamps to one cascade outright -- every
 * fragment past the last real split was looked up in cascade 3, whose matrix
 * was never uploaded and whose map was never rendered, with no fade to hide
 * it. The shader compares VIEW-SPACE Z, so that boundary is a plane whose
 * normal is the camera forward axis: it sweeps across the world when the
 * camera turns on the spot.
 *
 * The tests below are written against the shader's reading of the vec4, not
 * against the packing code, so they stay true if the packing is rewritten.
 */
#include "unity.h"

#include <stdint.h>
#include <math.h>

void sr_pack_csm_splits(const float *splits, uint32_t cascade_count,
                        float out[4]);

void setUp(void) {}
void tearDown(void) {}

/* csm_shadow.sh's ladder, transcribed. Returns the cascade the shader would
 * select for a fragment at this view depth. Keeping the shader's logic here
 * rather than asserting on lane values directly is what makes these tests
 * about the CONSEQUENCE instead of about the implementation. */
static int shader_cascade_for(const float s[4], float view_depth)
{
    if      (view_depth < s[0]) return 0;
    else if (view_depth < s[1]) return 1;
    else if (view_depth < s[2]) return 2;
    return 3;
}

/* The shader's distance fade: mix(shadow, 1, smoothstep(w*0.85, w, d)). */
static float shader_fade(const float s[4], float view_depth)
{
    const float far = s[3];
    if (far <= 0.0f) return 0.0f;             /* fade disabled entirely */
    const float lo = far * 0.85f, hi = far;
    if (view_depth <= lo) return 0.0f;
    if (view_depth >= hi) return 1.0f;
    const float t = (view_depth - lo) / (hi - lo);
    return t * t * (3.0f - 2.0f * t);
}

/* ── 1. Four cascades: unchanged, and this is the byte-identity baseline ── */

static void test_four_cascades_are_unchanged(void)
{
    /* splits[0] is the near plane; splits[1..4] are the cascade far planes. */
    const float splits[5] = { 0.1f, 20.0f, 60.0f, 150.0f, 400.0f };
    float out[4];
    sr_pack_csm_splits(splits, 4, out);

    TEST_ASSERT_EQUAL_FLOAT(20.0f,  out[0]);
    TEST_ASSERT_EQUAL_FLOAT(60.0f,  out[1]);
    TEST_ASSERT_EQUAL_FLOAT(150.0f, out[2]);
    TEST_ASSERT_EQUAL_FLOAT(400.0f, out[3]);
}

/* ── 2. THE bug: every lane must be usable, whatever the cascade count ──── */

static void test_no_fragment_falls_into_an_unrendered_cascade(void)
{
    const float splits[5] = { 0.1f, 20.0f, 60.0f, 150.0f, 400.0f };

    for (uint32_t count = 1; count <= 4; ++count) {
        float out[4];
        sr_pack_csm_splits(splits, count, out);

        const float last = splits[count];      /* this config's real range */

        /* Anything the shader can still shade -- i.e. inside the shadow
         * distance -- must select a cascade that was actually rendered. */
        const float probes[] = { 0.5f, 5.0f, 19.0f, 21.0f, 59.0f, 61.0f,
                                 149.0f, 151.0f, 399.0f };
        for (unsigned i = 0; i < sizeof probes / sizeof probes[0]; ++i) {
            const float d = probes[i];
            if (shader_fade(out, d) >= 1.0f) continue;   /* fully faded out */
            const int c = shader_cascade_for(out, d);
            char msg[128];
            snprintf(msg, sizeof msg,
                     "count=%u depth=%.1f selected cascade %d, which was "
                     "never rendered", count, (double)d, c);
            TEST_ASSERT_TRUE_MESSAGE(c < (int)count, msg);
        }

        /* And the shadow distance must be this configuration's real range,
         * not zero -- .w == 0 disables the fade, which is what turned the
         * boundary into a step. */
        char msg2[96];
        snprintf(msg2, sizeof msg2,
                 "count=%u left .w at %.3f: the distance fade is dead",
                 count, (double)out[3]);
        TEST_ASSERT_TRUE_MESSAGE(out[3] > 0.0f, msg2);
        TEST_ASSERT_EQUAL_FLOAT(last, out[3]);
    }
}

/* ── 3. The ladder must be non-decreasing ─────────────────────────────────
 *
 * The shader's chain of `else if` only makes sense on a sorted ladder; a lane
 * that went backwards would make a nearer fragment select a further cascade. */

static void test_ladder_is_monotonic(void)
{
    const float splits[5] = { 0.1f, 20.0f, 60.0f, 150.0f, 400.0f };
    for (uint32_t count = 1; count <= 4; ++count) {
        float out[4];
        sr_pack_csm_splits(splits, count, out);
        for (int i = 1; i < 4; ++i)
            TEST_ASSERT_TRUE_MESSAGE(out[i] >= out[i - 1],
                "the split ladder went backwards");
    }
}

/* ── 4. Zero cascades stays zero, because zero MEANS something ───────────
 *
 * csm_shadow.sh's first line is `if (u_csmSplits.x <= 0.0) return 1.0;` --
 * zero means "there is no shadow map", which is a different statement from
 * "shadows end here". Padding this case with anything positive would invent a
 * shadow distance for a configuration that has none. */

static void test_zero_cascades_means_no_shadow_map(void)
{
    const float splits[5] = { 0.1f, 20.0f, 60.0f, 150.0f, 400.0f };
    float out[4] = { 9, 9, 9, 9 };
    sr_pack_csm_splits(splits, 0, out);
    for (int i = 0; i < 4; ++i) TEST_ASSERT_EQUAL_FLOAT(0.0f, out[i]);

    /* NULL is answerable, not fatal. */
    float out2[4] = { 9, 9, 9, 9 };
    sr_pack_csm_splits(NULL, 4, out2);
    for (int i = 0; i < 4; ++i) TEST_ASSERT_EQUAL_FLOAT(0.0f, out2[i]);
}

/* ── 5. More cascades than lanes cannot overrun ──────────────────────────── */

static void test_count_above_four_is_clamped(void)
{
    const float splits[9] = { 0.1f, 10, 20, 30, 40, 50, 60, 70, 80 };
    float out[4];
    sr_pack_csm_splits(splits, 8, out);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, out[0]);
    TEST_ASSERT_EQUAL_FLOAT(40.0f, out[3]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_four_cascades_are_unchanged);
    RUN_TEST(test_no_fragment_falls_into_an_unrendered_cascade);
    RUN_TEST(test_ladder_is_monotonic);
    RUN_TEST(test_zero_cascades_means_no_shadow_map);
    RUN_TEST(test_count_above_four_is_clamped);
    return UNITY_END();
}
