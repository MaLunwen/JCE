/*
 * test_jce_shadow_sun.c
 *
 * Sun-motion quantisation for the CSM cache.
 *
 * Both ways this can be wrong are silent.  Too eager and the cache never hits
 * and the only symptom is frame time.  Too lazy -- or, far worse, comparing
 * against the previous FRAME instead of the held anchor -- and the sun walks
 * away from the shadow map that is still on screen, with no single frame ever
 * showing a jump.  Neither failure produces a log line.
 */

#include "jce_shadow_sun.h"

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* Sun direction from elevation/azimuth, in radians. */
static void sun_dir(float elev, float azim, float out[3])
{
    out[0] = cosf(elev) * cosf(azim);
    out[1] = sinf(elev);
    out[2] = cosf(elev) * sinf(azim);
}

static float angle_between(const float a[3], const float b[3])
{
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (d >  1.0f) d =  1.0f;
    if (d < -1.0f) d = -1.0f;
    return acosf(d);
}

/* ── 1. The quantum is derived, and fails toward re-rendering ───────────  */

static void test_quantum_is_derived_from_texel_size(void)
{
    /* 100 m cascade, 2048 map, 30 m tall casters: one texel is 48.8 mm, and a
     * 30 m caster's shadow slides that far when the sun moves 1.63 mrad. */
    const float q = jce_shadow_sun_quantum(100.0f, 2048u, 30.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, (100.0f / 2048.0f) / 30.0f, q);

    /* A sharper map must tolerate LESS sun motion, and taller casters likewise
     * -- both scale the shadow's slide per unit of sun rotation. */
    TEST_ASSERT_TRUE(jce_shadow_sun_quantum(100.0f, 4096u, 30.0f) < q);
    TEST_ASSERT_TRUE(jce_shadow_sun_quantum(100.0f, 2048u, 60.0f) < q);

    /* Every degenerate input yields 0 = "re-render every frame".  A quantum
     * that erred the other way would freeze the shadows, and nothing in the
     * engine reports a frozen shadow. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_shadow_sun_quantum(100.0f, 0u,    30.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_shadow_sun_quantum(0.0f,   2048u, 30.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_shadow_sun_quantum(100.0f, 2048u, 0.0f));

    /* And it is capped: a 20 m cascade over 1 m props derives ~0.5 rad, which
     * is 28 degrees of sun lag -- texel-snapping stopped describing what the
     * viewer sees a long way before that. */
    TEST_ASSERT_TRUE(jce_shadow_sun_quantum(20.0f, 512u, 1.0f) <= 0.0174533f);
}

/* ── 2. A still sun renders once ───────────────────────────────────────  */

static void test_static_sun_updates_only_once(void)
{
    JceShadowSunHold h;
    memset(&h, 0, sizeof h);
    float d[3], out[3];
    sun_dir(0.7f, 1.2f, d);

    TEST_ASSERT_TRUE(jce_shadow_sun_update(&h, d, 0.002f, out));  /* first: yes */
    for (int i = 0; i < 240; ++i)
        TEST_ASSERT_FALSE(jce_shadow_sun_update(&h, d, 0.002f, out));
}

/* ── 3. THE POINT: sub-quantum motion still accumulates ────────────────
 *
 * This is the test that separates "compare against the held anchor" from
 * "compare against last frame".  A day/night cycle moves the sun ~0.07 mrad per
 * frame at 60 fps, far below any sane quantum, so a per-frame delta NEVER fires
 * -- the sun crosses the whole sky while the shadow map stays anchored to
 * dawn.  There is no frame in which that looks wrong; it is only wrong when
 * compared against where the sun actually is. */

static void test_slow_drift_still_triggers_an_update(void)
{
    JceShadowSunHold h;
    memset(&h, 0, sizeof h);
    const float quantum = 0.01f;               /* ~0.57 degrees */
    const float per_frame = quantum / 50.0f;   /* 50 frames per quantum */

    float d[3], out[3];
    sun_dir(0.5f, 0.0f, d);
    TEST_ASSERT_TRUE(jce_shadow_sun_update(&h, d, quantum, out));

    int updates = 0;
    for (int f = 1; f <= 500; ++f) {
        sun_dir(0.5f, per_frame * (float)f, d);
        if (jce_shadow_sun_update(&h, d, quantum, out)) updates++;
        /* Whatever it decides, the direction handed back must never be further
         * from the true sun than the quantum allows -- that bound IS the
         * contract, and a per-frame delta violates it without ever returning
         * true. */
        TEST_ASSERT_TRUE(angle_between(out, d) <= quantum * 1.05f);
    }
    /* 500 frames at 1/50th of a quantum is 10 quanta of travel. */
    TEST_ASSERT_TRUE(updates >= 8);
    TEST_ASSERT_TRUE(updates <= 12);
}

/* ── 4. Jitter at the threshold must not flip-flop ─────────────────────
 *
 * Grid snapping fails here: a sun sitting on a bin edge with float noise lands
 * in a different bin every frame, so the shadows twitch by a full quantum back
 * and forth AND the cache misses every frame -- strictly worse than not
 * quantising at all.  Deadband re-anchors to where the sun actually is, so
 * once it has moved, the noise is measured from the new anchor. */

static void test_threshold_jitter_does_not_oscillate(void)
{
    JceShadowSunHold h;
    memset(&h, 0, sizeof h);
    const float quantum = 0.01f;

    float d[3], out[3];
    sun_dir(0.5f, 0.0f, d);
    jce_shadow_sun_update(&h, d, quantum, out);

    /* Oscillate across the threshold 200 times. */
    int updates = 0;
    for (int f = 0; f < 200; ++f) {
        const float az = (f & 1) ? (quantum * 1.01f) : 0.0f;
        sun_dir(0.5f, az, d);
        if (jce_shadow_sun_update(&h, d, quantum, out)) updates++;
    }
    /* Two anchors at a quantum apart: the jitter can trip at most the first
     * crossing, then both extremes sit inside the deadband of the new anchor.
     * Grid snapping would report ~200 here. */
    TEST_ASSERT_TRUE(updates <= 2);
}

/* ── 5. The held direction is what the caller must render with ─────────
 *
 * Handing back the raw sun on a false return is the one combination that is
 * actively wrong: the light VP would move while the depth buffer did not, so
 * every shadow would land offset from its caster. */

static void test_out_dir_is_the_anchor_not_the_input(void)
{
    JceShadowSunHold h;
    memset(&h, 0, sizeof h);
    float anchor[3], moved[3], out[3];
    sun_dir(0.5f, 0.0f, anchor);
    jce_shadow_sun_update(&h, anchor, 0.05f, out);

    sun_dir(0.5f, 0.01f, moved);               /* well inside the deadband */
    TEST_ASSERT_FALSE(jce_shadow_sun_update(&h, moved, 0.05f, out));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, anchor[0], out[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, anchor[1], out[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, anchor[2], out[2]);

    /* Zero quantum means quantisation is off: every frame re-renders, and the
     * output tracks the input exactly. */
    JceShadowSunHold off;
    memset(&off, 0, sizeof off);
    TEST_ASSERT_TRUE(jce_shadow_sun_update(&off, anchor, 0.0f, out));
    TEST_ASSERT_TRUE(jce_shadow_sun_update(&off, moved,  0.0f, out));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, moved[0], out[0]);
}

/* ── 6. Unnormalised and degenerate input ──────────────────────────────  */

static void test_scale_does_not_matter_and_zero_is_refused(void)
{
    JceShadowSunHold h;
    memset(&h, 0, sizeof h);
    float d[3], out[3];
    sun_dir(0.5f, 1.0f, d);
    jce_shadow_sun_update(&h, d, 0.01f, out);

    /* The same direction at 100x length is the same direction: a caller that
     * scaled its sun vector must not trigger a re-render. */
    float scaled[3] = { d[0] * 100.0f, d[1] * 100.0f, d[2] * 100.0f };
    TEST_ASSERT_FALSE(jce_shadow_sun_update(&h, scaled, 0.01f, out));

    /* The anchor is stored normalised, so out_dir is directly usable. */
    const float len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, len);

    /* A zero sun holds rather than anchoring the maps to a degenerate light. */
    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    TEST_ASSERT_FALSE(jce_shadow_sun_update(&h, zero, 0.01f, out));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f,
        sqrtf(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]));
}

/* ── 7. What this actually buys, at production numbers ─────────────────
 *
 * Not a frame-time measurement -- it is the recompute COUNT under a realistic
 * sun, which is the thing the quantiser controls.  Frame cost is downstream of
 * it and belongs to a profiling run, not a unit test.
 *
 * Setup: a 30 m cascade 0 at 2048, a 40 m tall scene, and a day/night cycle
 * compressed to 24 minutes (60x real time) -- faster than most games run it,
 * so this is the pessimistic end. */

static void test_day_cycle_recompute_count(void)
{
    const float q = jce_shadow_sun_quantum(30.0f, 2048u, 40.0f);
    TEST_ASSERT_TRUE(q > 0.0f);

    JceShadowSunHold h;
    memset(&h, 0, sizeof h);

    /* 60x day cycle = 360 degrees per 24 min = 0.25 deg/s; at 60 fps that is
     * 72.7 urad per frame. */
    const float per_frame = 72.7e-6f;
    const int   frames    = 3600;              /* one minute */
    float d[3], out[3];
    int updates = 0;
    for (int f = 0; f < frames; ++f) {
        sun_dir(0.6f, per_frame * (float)f, d);
        if (jce_shadow_sun_update(&h, d, q, out)) updates++;
    }

    /* Without quantisation this is 3600 rebuilds of every cascade.  The bound
     * below is the arithmetic, not a hope: total travel / quantum, with slack
     * for the cos(elevation) foreshortening of azimuth. */
    TEST_ASSERT_TRUE(updates < frames / 4);
    TEST_ASSERT_TRUE(updates > 0);        /* and it must still track the sun */

    /* State the win in the form a reader can check: at least a 4x drop in CSM
     * recomputes for a cycle running 60x faster than real time.  At 1x it is
     * two orders of magnitude, because the sun moves 60x less per frame. */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_quantum_is_derived_from_texel_size);
    RUN_TEST(test_static_sun_updates_only_once);
    RUN_TEST(test_slow_drift_still_triggers_an_update);
    RUN_TEST(test_threshold_jitter_does_not_oscillate);
    RUN_TEST(test_out_dir_is_the_anchor_not_the_input);
    RUN_TEST(test_scale_does_not_matter_and_zero_is_refused);
    RUN_TEST(test_day_cycle_recompute_count);
    return UNITY_END();
}
