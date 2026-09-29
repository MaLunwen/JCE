/* test_jce_audio_reverb_stages.c
 *
 * A room answers a sound three times: directly, off its nearest surfaces, and
 * as a diffuse tail once everything has smeared together.  The reverb here
 * produced only the third of those, starting immediately, which is why every
 * room sounded like the same room at a different volume.
 *
 * SIX AUTHORED NUMBERS DID NOTHING before this.  jce_audio_set_reverb read
 * five of JceAudioReverbParams's nine fields -- room_size, diffusion, density
 * and pre_delay_ms reached no part of the DSP -- and the scene component's
 * `reflections` and `reflectionsDelay` were parsed, written back, and reached
 * no preset field at all.  None of that could be asserted, because the reverb
 * had no offline entry point; the DSP chain beside it has had one for exactly
 * this reason since it was written.
 *
 * Every case here is an IMPULSE RESPONSE: one sample of 1.0, then silence,
 * and the assertions are about WHERE energy lands in time.  That is the only
 * way to tell "the tail is delayed" from "the tail is quieter", which a
 * loudness measurement cannot.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/audio/jce_audio.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

enum { SR = 48000, N = SR / 4 };   /* 250 ms, mono */

static float *impulse(void)
{
    float *b = (float *)calloc(N, sizeof(float));
    TEST_ASSERT_NOT_NULL(b);
    b[0] = 1.0f;
    return b;
}

/* Peak magnitude in [from, to). */
static float peak(const float *b, int from, int to)
{
    float m = 0.0f;
    if (from < 0) from = 0;
    if (to > N) to = N;
    for (int i = from; i < to; i++) {
        float a = fabsf(b[i]);
        if (a > m) m = a;
    }
    return m;
}

static JceAudioReverbParams base_params(void)
{
    JceAudioReverbParams p;
    memset(&p, 0, sizeof p);
    p.wet_mix       = 1.0f;
    p.dry_mix       = 0.0f;     /* wet only: the direct path would swamp it */
    p.decay_seconds = 1.5f;
    p.damping       = 0.2f;
    p.lowpass_hz    = 22050.0f;
    p.diffusion     = 0.5f;
    return p;
}

/* ── 1. the late tail starts after the pre-delay, not at zero ──────── */
static void test_pre_delay_moves_the_tail(void)
{
    /* 60 ms of pre-delay = 2880 frames.  Before it there must be (almost)
     * nothing; after it the tail must be there.  "Almost" and not "exactly"
     * because the wet path is a filter, not a gate. */
    const int pd_ms = 60;
    const int pd_fr = pd_ms * SR / 1000;

    float *none = impulse();
    JceAudioReverbParams p = base_params();
    TEST_ASSERT_TRUE(jce_audio_reverb_process_offline(&p, none, N, 1, SR));
    const float early_energy_without = peak(none, 1, pd_fr);
    TEST_ASSERT_TRUE_MESSAGE(early_energy_without > 1e-4f,
        "with no pre-delay the tail must already be present in the first 60 ms "
        "-- if it is not, this test cannot tell a delay from silence");

    float *with = impulse();
    p = base_params();
    p.pre_delay_ms = (float)pd_ms;
    TEST_ASSERT_TRUE(jce_audio_reverb_process_offline(&p, with, N, 1, SR));

    const float before = peak(with, 1, pd_fr - 64);
    const float after  = peak(with, pd_fr, N);
    TEST_ASSERT_TRUE_MESSAGE(after > 1e-4f, "the delayed tail never arrived");
    TEST_ASSERT_TRUE_MESSAGE(before < after * 0.05f,
        "energy before the pre-delay is not below 5% of the tail -- the delay "
        "is not delaying");
    free(none);
    free(with);
}

/* ── 2. room_size derives a pre-delay when none is stated ──────────── */
static void test_room_size_derives_the_pre_delay(void)
{
    /* The public header said "room_size (m) scales the pre-delay" for a long
     * time while nothing read the field.  ~0.34 m per ms, so 34 m is ~100 ms. */
    float *b = impulse();
    JceAudioReverbParams p = base_params();
    p.room_size = 34.0f;          /* and pre_delay_ms deliberately 0 */
    TEST_ASSERT_TRUE(jce_audio_reverb_process_offline(&p, b, N, 1, SR));

    const int expect_fr = (int)(34.0f / 0.34f) * SR / 1000;   /* ~100 ms */
    const float before = peak(b, 1, expect_fr - 512);
    const float after  = peak(b, expect_fr, N);
    TEST_ASSERT_TRUE_MESSAGE(after > 1e-4f, "no tail at all");
    TEST_ASSERT_TRUE_MESSAGE(before < after * 0.05f,
        "room_size did not produce a pre-delay");
    free(b);
}

/* ── 3. an explicit pre-delay wins over the derived one ────────────── */
static void test_explicit_pre_delay_beats_room_size(void)
{
    /* Otherwise a preset that states both would be at the mercy of which one
     * the implementation happened to check first. */
    float *b = impulse();
    JceAudioReverbParams p = base_params();
    p.room_size    = 340.0f;      /* would derive ~1000 ms: past the buffer */
    p.pre_delay_ms = 20.0f;
    TEST_ASSERT_TRUE(jce_audio_reverb_process_offline(&p, b, N, 1, SR));

    const int fr20 = 20 * SR / 1000;
    TEST_ASSERT_TRUE_MESSAGE(peak(b, fr20, fr20 + SR / 20) > 1e-4f,
        "the explicit 20 ms pre-delay was overridden by room_size");
    free(b);
}

/* ── 4. the early reflection is a tap, at its own time ─────────────── */
static void test_early_reflection_arrives_before_the_tail(void)
{
    /* This is the field the row was named after, and the one the inspector
     * badged as unread for as long as the DSP had nowhere to put it. */
    const int early_ms = 15, late_ms = 90;
    const int early_fr = early_ms * SR / 1000;
    const int late_fr  = late_ms  * SR / 1000;

    JceAudioReverbParams p = base_params();
    p.pre_delay_ms    = (float)late_ms;
    p.early_delay_ms  = (float)early_ms;
    p.early_mix       = 0.5f;

    float *on = impulse();
    TEST_ASSERT_TRUE(jce_audio_reverb_process_offline(&p, on, N, 1, SR));

    /* Between the early tap and the late tail there must be a clear peak. */
    const float at_early = peak(on, early_fr - 2, early_fr + 8);
    TEST_ASSERT_TRUE_MESSAGE(at_early > 0.1f,
        "no early reflection at its stated delay");

    /* And with early_mix 0 that window must be quiet while the tail is not:
     * the control that separates 'the tap exists' from 'something is loud'. */
    float *off = impulse();
    p.early_mix = 0.0f;
    TEST_ASSERT_TRUE(jce_audio_reverb_process_offline(&p, off, N, 1, SR));
    const float off_early = peak(off, early_fr - 2, early_fr + 8);
    const float off_late  = peak(off, late_fr, N);
    TEST_ASSERT_TRUE_MESSAGE(off_early < at_early * 0.05f,
        "turning the early reflection off did not remove it");
    TEST_ASSERT_TRUE_MESSAGE(off_late > 1e-4f,
        "turning the early reflection off also killed the tail");

    free(on);
    free(off);
}

/* ── 5. the early level is a level ─────────────────────────────────── */
static void test_early_level_scales_the_tap(void)
{
    const int early_fr = 15 * SR / 1000;
    JceAudioReverbParams p = base_params();
    p.pre_delay_ms   = 90.0f;
    p.early_delay_ms = 15.0f;

    float *quiet = impulse();
    p.early_mix = 0.25f;
    TEST_ASSERT_TRUE(jce_audio_reverb_process_offline(&p, quiet, N, 1, SR));
    const float q = peak(quiet, early_fr - 2, early_fr + 8);

    float *loud = impulse();
    p.early_mix = 0.75f;
    TEST_ASSERT_TRUE(jce_audio_reverb_process_offline(&p, loud, N, 1, SR));
    const float l = peak(loud, early_fr - 2, early_fr + 8);

    /* Three times the level, within the slop a filtered path allows. */
    TEST_ASSERT_TRUE_MESSAGE(l > q * 2.5f && l < q * 3.5f,
        "the early reflection level is not proportional to early_mix");
    free(quiet);
    free(loud);
}

/* ── 6. diffusion changes the tail ─────────────────────────────────── */
static void test_diffusion_changes_the_tail(void)
{
    /* diffusion drives the allpass feedback -- how much a reflection is
     * smeared rather than passed through.  It was accepted and ignored, which
     * the header called "for completeness". */
    float *lo = impulse();
    JceAudioReverbParams p = base_params();
    p.diffusion = 0.2f;
    TEST_ASSERT_TRUE(jce_audio_reverb_process_offline(&p, lo, N, 1, SR));

    float *hi = impulse();
    p = base_params();
    p.diffusion = 0.8f;
    TEST_ASSERT_TRUE(jce_audio_reverb_process_offline(&p, hi, N, 1, SR));

    double diff = 0.0;
    for (int i = 0; i < N; i++) diff += fabs((double)lo[i] - (double)hi[i]);
    TEST_ASSERT_TRUE_MESSAGE(diff > 1e-3,
        "diffusion 0.2 and 0.8 produced the same impulse response");

    /* 0.5 is Freeverb's fixed value, so it must reproduce the old sound --
     * otherwise this parameter silently re-tuned every existing preset. */
    float *mid = impulse();
    p = base_params();
    p.diffusion = 0.5f;
    TEST_ASSERT_TRUE(jce_audio_reverb_process_offline(&p, mid, N, 1, SR));
    float *unset = impulse();
    p = base_params();
    p.diffusion = 0.0f;           /* "not authored" */
    TEST_ASSERT_TRUE(jce_audio_reverb_process_offline(&p, unset, N, 1, SR));
    for (int i = 0; i < N; i++)
        TEST_ASSERT_FLOAT_WITHIN(1e-6f, mid[i], unset[i]);

    free(lo); free(hi); free(mid); free(unset);
}

/* ── 7. it is deterministic ────────────────────────────────────────── */
static void test_two_runs_are_identical(void)
{
    /* Every assertion above compares two runs.  If the DSP carried state
     * across calls they would all be measuring the previous test. */
    JceAudioReverbParams p = base_params();
    p.pre_delay_ms = 30.0f;
    p.early_mix    = 0.4f;
    p.early_delay_ms = 12.0f;

    float *a = impulse(), *b = impulse();
    TEST_ASSERT_TRUE(jce_audio_reverb_process_offline(&p, a, N, 1, SR));
    TEST_ASSERT_TRUE(jce_audio_reverb_process_offline(&p, b, N, 1, SR));
    for (int i = 0; i < N; i++)
        TEST_ASSERT_EQUAL_MEMORY(&a[i], &b[i], sizeof(float));
    free(a); free(b);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pre_delay_moves_the_tail);
    RUN_TEST(test_room_size_derives_the_pre_delay);
    RUN_TEST(test_explicit_pre_delay_beats_room_size);
    RUN_TEST(test_early_reflection_arrives_before_the_tail);
    RUN_TEST(test_early_level_scales_the_tap);
    RUN_TEST(test_diffusion_changes_the_tail);
    RUN_TEST(test_two_runs_are_identical);
    return UNITY_END();
}
