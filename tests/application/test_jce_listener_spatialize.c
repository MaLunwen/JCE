/*
 * test_jce_listener_spatialize.c — AudioListener.spatialize was authored,
 * serialised, drawn as a checkbox, and deferred in a comment.
 *
 *     `spatialize` (HRTF toggle) has no public engine API yet -> followup.
 *
 * jce_audio_voice_set_3d(audio, voice, spatial) is public API, and the
 * runtime's own voice-start path has been calling it all along -- so the
 * switch that turns 3D positioning off for the whole mix had somewhere to go
 * for as long as that note sat there.
 *
 * It is a GATE, not a replacement: VoiceEntry.spatial still records what each
 * AudioSource authored, so closing the gate flattens everything and reopening
 * it restores each voice to its own flag rather than making everything 3D.
 * That distinction is the whole test -- a gate that "restores to 3D" would
 * start panning a deliberately flat UI sound the first time someone toggled
 * the listener.
 *
 * THE ARGUMENT IS `listener_flat`, NOT `spatialize`.  The runtime stores the
 * gate in the flat sense so that a memset'd JceRuntime already means "3D, as
 * before" -- with the authored sense it would have needed an initialiser in
 * jce_runtime_create, and a default nobody may forget beats a default someone
 * must remember.  Exactly one negation lives on each side (the listener scan
 * writes !spatialize; this predicate reads !listener_flat) and no call site
 * carries one.  Asserting the flat sense here is asserting what the runtime
 * actually holds.
 *
 * WHAT THIS COVERS: the decision, as a pure predicate over the two flags.
 * The miniaudio call it drives needs an audio device and a running runtime,
 * which a unit test has neither of, so what is asserted here is the rule and
 * not the plumbing -- said plainly rather than implied.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include "application/jce_rt_internal.h"

/* THE ENGINE'S function, not a copy of it.  The first draft of this file
 * defined the predicate locally and asserted that -- which tests the test.
 * rt_voice_should_be_3d is exported precisely so the three runtime sites that
 * must agree share one answer and so this can ask the real one. */
#define voice_is_3d rt_voice_should_be_3d

enum { FLAT = 1, NOT_FLAT = 0 };   /* the gate, in the sense the runtime holds */

void setUp(void) {}
void tearDown(void) {}

static void test_the_gate_flattens_everything_when_closed(void)
{
    TEST_ASSERT_FALSE_MESSAGE(voice_is_3d(FLAT, true),
        "a 3D source must be heard flat while the listener's gate is closed");
    TEST_ASSERT_FALSE(voice_is_3d(FLAT, false));
}

static void test_reopening_restores_what_each_source_authored(void)
{
    /* THE DISTINCTION.  A gate that restored everything to 3D would start
     * panning a UI sound that was authored flat -- silently, and only after
     * someone toggled a checkbox they expected to be harmless. */
    TEST_ASSERT_TRUE_MESSAGE(voice_is_3d(NOT_FLAT, true),
        "a 3D source is 3D again once the gate reopens");
    TEST_ASSERT_FALSE_MESSAGE(voice_is_3d(NOT_FLAT, false),
        "...and a source authored FLAT stays flat: the listener gate is a "
        "gate over per-source intent, not a replacement for it");
}

static void test_the_zeroed_runtime_is_the_old_behaviour(void)
{
    /* THE REASON THE FIELD IS STORED INVERTED.  jce_runtime_create memsets the
     * struct, so listener_flat starts at 0 -- and this asserts that 0 is the
     * behaviour every scene had before this existed.  Read the byte out of a
     * zeroed struct rather than writing the constant 0 here: a literal would
     * still pass if someone changed the field's polarity back. */
    JceRuntime *zeroed = (JceRuntime *)calloc(1, sizeof(JceRuntime));
    TEST_ASSERT_NOT_NULL(zeroed);
    TEST_ASSERT_TRUE_MESSAGE(voice_is_3d(zeroed->listener_flat, true),
        "a memset'd runtime must mean 3D: a scene with no AudioListener at "
        "all, and every voice started before the first listener scan");
    TEST_ASSERT_EQUAL_MESSAGE(zeroed->listener_flat, zeroed->listener_flat_applied,
        "and the two must start equal, or the first frame pushes a change "
        "that did not happen");
    free(zeroed);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_gate_flattens_everything_when_closed);
    RUN_TEST(test_reopening_restores_what_each_source_authored);
    RUN_TEST(test_the_zeroed_runtime_is_the_old_behaviour);
    return UNITY_END();
}
