/* test_jce_audio_occlusion.c
 *
 * Pure-CPU unit tests for the audio occlusion solver and its temporal
 * tracker.  Both APIs take a raycast callback, which we satisfy with a
 * tiny stub that lets each test choose its own "hit fraction" and
 * "material absorption" without ever pulling in physics.
 *
 * Covered behaviours:
 *   - default-params values
 *   - clear path yields zero occlusion, bypass lowpass, unity gain
 *   - fully occluded path: occlusion=1, lowpass at floor, atten at min
 *   - source past max_raycast_dist treated as clear
 *   - source coincident with listener (dist~0) yields clear path
 *   - apply_curves: lowpass sweeps geometrically with occlusion in [0,1]
 *   - tracker smoothing converges toward target after enough frames
 *   - tracker GC drops stale voice ids
 *   - tracker re-grows the table past the initial capacity
 *   - tracker_create rejects nothing — small caps are rounded up
 */

#include <jce/middleware/audio/jce_audio_occlusion.h>

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define EPS 1e-3f

typedef struct {
    float hit;          /* what the stub returns */
    float absorption;   /* what the stub writes back */
    int   calls;
} RayStub;

static float stub_raycast(void *ud,
                          jce_vec3 origin, jce_vec3 dir,
                          float max_distance,
                          float *out_absorption)
{
    (void)origin; (void)dir; (void)max_distance;
    RayStub *s = (RayStub *)ud;
    s->calls++;
    if (out_absorption) *out_absorption = s->absorption;
    return s->hit;
}

/* ── defaults ──────────────────────────────────────────────────────── */

static void test_default_params(void)
{
    JceAudioOcclusionParams p = jce_audio_occlusion_default_params();
    TEST_ASSERT_FLOAT_WITHIN(EPS,   400.0f, p.min_lowpass_hz);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 22050.0f, p.max_lowpass_hz);
    TEST_ASSERT_FLOAT_WITHIN(EPS,  0.15f,   p.min_direct_volume);
    TEST_ASSERT_FLOAT_WITHIN(EPS,  0.85f,   p.smoothing);
    TEST_ASSERT_FLOAT_WITHIN(EPS,  200.0f,  p.max_raycast_dist);
}

/* ── stateless solve ───────────────────────────────────────────────── */

static void test_solve_clear_path(void)
{
    JceAudioOcclusionParams p = jce_audio_occlusion_default_params();
    JceAudioOcclusionQuery q = { .source_position = jce_v3(10, 0, 0) };
    RayStub s = { .hit = 1.0f, .absorption = 0.5f };
    jce_audio_occlusion_solve(&p, jce_v3(0, 0, 0), &q, 1, stub_raycast, &s);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, q.occlusion);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, q.attenuation);
    TEST_ASSERT_FLOAT_WITHIN(EPS, p.max_lowpass_hz, q.lowpass_hz);
}

static void test_solve_full_occlusion(void)
{
    JceAudioOcclusionParams p = jce_audio_occlusion_default_params();
    JceAudioOcclusionQuery q = { .source_position = jce_v3(10, 0, 0) };
    RayStub s = { .hit = 0.0f, .absorption = 1.0f };
    jce_audio_occlusion_solve(&p, jce_v3(0, 0, 0), &q, 1, stub_raycast, &s);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, q.occlusion);
    TEST_ASSERT_FLOAT_WITHIN(EPS, p.min_direct_volume, q.attenuation);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, p.min_lowpass_hz, q.lowpass_hz);
}

static void test_solve_past_max_distance_is_clear(void)
{
    JceAudioOcclusionParams p = jce_audio_occlusion_default_params();
    p.max_raycast_dist = 5.0f;
    JceAudioOcclusionQuery q = { .source_position = jce_v3(100, 0, 0) };
    RayStub s = { .hit = 0.0f, .absorption = 1.0f, .calls = 0 };
    jce_audio_occlusion_solve(&p, jce_v3(0, 0, 0), &q, 1, stub_raycast, &s);
    TEST_ASSERT_EQUAL_INT(0, s.calls);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, q.occlusion);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, q.attenuation);
}

static void test_solve_coincident_listener_skips_raycast(void)
{
    JceAudioOcclusionParams p = jce_audio_occlusion_default_params();
    JceAudioOcclusionQuery q = { .source_position = jce_v3(0, 0, 0) };
    RayStub s = { .hit = 0.0f, .absorption = 1.0f, .calls = 0 };
    jce_audio_occlusion_solve(&p, jce_v3(0, 0, 0), &q, 1, stub_raycast, &s);
    TEST_ASSERT_EQUAL_INT(0, s.calls);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, q.occlusion);
}

static void test_solve_null_or_zero_count_is_safe(void)
{
    JceAudioOcclusionParams p = jce_audio_occlusion_default_params();
    jce_audio_occlusion_solve(NULL, jce_v3(0,0,0), NULL, 0, NULL, NULL);
    jce_audio_occlusion_solve(&p, jce_v3(0,0,0), NULL, 5, stub_raycast, NULL);
}

static void test_lowpass_monotonic_with_occlusion(void)
{
    JceAudioOcclusionParams p = jce_audio_occlusion_default_params();
    JceAudioOcclusionQuery q1 = { .source_position = jce_v3(10, 0, 0) };
    JceAudioOcclusionQuery q2 = q1;
    JceAudioOcclusionQuery q3 = q1;
    RayStub s1 = { .hit = 1.0f, .absorption = 0.0f }; /* occ=0   */
    RayStub s2 = { .hit = 0.5f, .absorption = 0.5f }; /* occ=.25 */
    RayStub s3 = { .hit = 0.0f, .absorption = 1.0f }; /* occ=1   */
    jce_audio_occlusion_solve(&p, jce_v3(0,0,0), &q1, 1, stub_raycast, &s1);
    jce_audio_occlusion_solve(&p, jce_v3(0,0,0), &q2, 1, stub_raycast, &s2);
    jce_audio_occlusion_solve(&p, jce_v3(0,0,0), &q3, 1, stub_raycast, &s3);
    TEST_ASSERT_TRUE(q1.lowpass_hz > q2.lowpass_hz);
    TEST_ASSERT_TRUE(q2.lowpass_hz > q3.lowpass_hz);
    TEST_ASSERT_TRUE(q1.attenuation > q2.attenuation);
    TEST_ASSERT_TRUE(q2.attenuation > q3.attenuation);
}

/* ── tracker ───────────────────────────────────────────────────────── */

static void test_tracker_create_destroy_minimum_capacity(void)
{
    JceAudioOcclusionTracker *t = jce_audio_occlusion_tracker_create(0);
    TEST_ASSERT_NOT_NULL(t);
    jce_audio_occlusion_tracker_destroy(t);
    jce_audio_occlusion_tracker_destroy(NULL);
}

static void test_tracker_smoothing_converges(void)
{
    JceAudioOcclusionTracker *t = jce_audio_occlusion_tracker_create(16);
    JceAudioOcclusionParams p = jce_audio_occlusion_default_params();
    p.smoothing = 0.5f;
    jce_audio_occlusion_tracker_set_params(t, &p);

    JceAudioOcclusionQuery q = { .source_position = jce_v3(10,0,0) };
    uint64_t id = 7;
    RayStub  s = { .hit = 0.0f, .absorption = 1.0f }; /* target occ = 1 */
    for (int i = 0; i < 30; ++i) {
        q.source_position = jce_v3(10,0,0);
        jce_audio_occlusion_tracker_solve(t, jce_v3(0,0,0), &id, &q, 1,
                                          stub_raycast, &s);
    }
    TEST_ASSERT_TRUE(q.occlusion > 0.95f);

    /* Switch target to 0 and watch it relax. */
    s.hit = 1.0f;
    s.absorption = 0.0f;
    for (int i = 0; i < 30; ++i) {
        q.source_position = jce_v3(10,0,0);
        jce_audio_occlusion_tracker_solve(t, jce_v3(0,0,0), &id, &q, 1,
                                          stub_raycast, &s);
    }
    TEST_ASSERT_TRUE(q.occlusion < 0.05f);

    jce_audio_occlusion_tracker_destroy(t);
}

static void test_tracker_gc_evicts_inactive(void)
{
    JceAudioOcclusionTracker *t = jce_audio_occlusion_tracker_create(16);
    JceAudioOcclusionParams p = jce_audio_occlusion_default_params();
    p.smoothing = 0.0f;
    jce_audio_occlusion_tracker_set_params(t, &p);

    /* Insert two voices, then keep only the first alive. */
    uint64_t ids[2] = { 1, 2 };
    JceAudioOcclusionQuery q[2] = {
        { .source_position = jce_v3(5, 0, 0) },
        { .source_position = jce_v3(5, 0, 0) },
    };
    RayStub s = { .hit = 0.0f, .absorption = 1.0f };
    jce_audio_occlusion_tracker_solve(t, jce_v3(0,0,0), ids, q, 2,
                                      stub_raycast, &s);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, q[0].occlusion);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, q[1].occlusion);

    uint64_t active[1] = { 1 };
    jce_audio_occlusion_tracker_gc(t, active, 1);

    /* Recreate id=2 with a clear-path stub; smoothed_occ should start
     * fresh at 0 (raw occ 0), not the stale 1.0. */
    s.hit = 1.0f;
    s.absorption = 0.0f;
    q[1].source_position = jce_v3(5, 0, 0);
    jce_audio_occlusion_tracker_solve(t, jce_v3(0,0,0), &ids[1], &q[1], 1,
                                      stub_raycast, &s);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, q[1].occlusion);

    jce_audio_occlusion_tracker_destroy(t);
}

static void test_tracker_grows_past_initial_capacity(void)
{
    JceAudioOcclusionTracker *t = jce_audio_occlusion_tracker_create(16);
    JceAudioOcclusionParams p = jce_audio_occlusion_default_params();
    p.smoothing = 0.0f;
    jce_audio_occlusion_tracker_set_params(t, &p);

    enum { N = 64 };
    uint64_t ids[N];
    JceAudioOcclusionQuery q[N];
    for (int i = 0; i < N; ++i) {
        ids[i] = (uint64_t)(i + 1);
        q[i].source_position = jce_v3(5, 0, 0);
    }
    RayStub s = { .hit = 0.0f, .absorption = 1.0f };
    jce_audio_occlusion_tracker_solve(t, jce_v3(0,0,0), ids, q, N,
                                      stub_raycast, &s);
    for (int i = 0; i < N; ++i)
        TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, q[i].occlusion);

    jce_audio_occlusion_tracker_destroy(t);
}

static void test_tracker_null_safety(void)
{
    jce_audio_occlusion_tracker_set_params(NULL, NULL);
    jce_audio_occlusion_tracker_solve(NULL, jce_v3(0,0,0), NULL, NULL, 0,
                                      NULL, NULL);
    jce_audio_occlusion_tracker_gc(NULL, NULL, 0);
}

/* The smoothing coefficient is authored as a per-update retention factor, so
 * applying it once per rendered frame made a source duck faster on a faster
 * machine.  What has to hold is that the retention over a wall-clock interval
 * is the same however many updates it is split across. */
static void test_retention_is_frame_rate_independent(void)
{
    const float authored = 0.85f;   /* the shipped default */

    /* Two 1/120 s steps must retain exactly as much as one 1/60 s step. */
    const float one_60  = jce_audio_occlusion_retention_for_dt(authored,
                                                               1.0f / 60.0f);
    const float one_120 = jce_audio_occlusion_retention_for_dt(authored,
                                                               1.0f / 120.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, one_60, one_120 * one_120);

    /* Three 1/180 s steps likewise. */
    const float one_180 = jce_audio_occlusion_retention_for_dt(authored,
                                                               1.0f / 180.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, one_60, one_180 * one_180 * one_180);

    /* At the reference rate the authored value is reproduced unchanged, so
     * existing content sounds exactly as it did before. */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, authored, one_60);

    /* A longer step retains less (converges further), never more. */
    TEST_ASSERT_TRUE(
        jce_audio_occlusion_retention_for_dt(authored, 1.0f / 30.0f) < one_60);

    /* A hitch is clamped rather than snapping the filter wide open. */
    const float hitch = jce_audio_occlusion_retention_for_dt(authored, 5.0f);
    TEST_ASSERT_TRUE(hitch > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(
        1e-6f, jce_audio_occlusion_retention_for_dt(authored, 0.25f), hitch);

    /* Degenerate dt leaves the authored value alone rather than dividing. */
    TEST_ASSERT_EQUAL_FLOAT(authored,
                            jce_audio_occlusion_retention_for_dt(authored, 0.0f));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_params);
    RUN_TEST(test_solve_clear_path);
    RUN_TEST(test_solve_full_occlusion);
    RUN_TEST(test_solve_past_max_distance_is_clear);
    RUN_TEST(test_solve_coincident_listener_skips_raycast);
    RUN_TEST(test_solve_null_or_zero_count_is_safe);
    RUN_TEST(test_lowpass_monotonic_with_occlusion);
    RUN_TEST(test_tracker_create_destroy_minimum_capacity);
    RUN_TEST(test_tracker_smoothing_converges);
    RUN_TEST(test_tracker_gc_evicts_inactive);
    RUN_TEST(test_tracker_grows_past_initial_capacity);
    RUN_TEST(test_tracker_null_safety);
    RUN_TEST(test_retention_is_frame_rate_independent);
    return UNITY_END();
}
