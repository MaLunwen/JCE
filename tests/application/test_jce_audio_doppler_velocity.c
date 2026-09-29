/* test_jce_audio_doppler_velocity.c
 *
 * DOPPLER NEEDS A VELOCITY AND THE SCENE ONLY CARRIES POSITIONS.
 *
 * The whole Doppler path shipped complete and inert: jce_audio.h declared
 * jce_audio_set_doppler_factor, jce_audio.c pushed it onto every live sound
 * every frame, and jce_rt_audio.c fed it from the AudioListener component --
 * while jce_audio_voice_set_velocity had NO CALLER ANYWHERE and the listener's
 * own velocity was left at {0,0,0} by a memset.  miniaudio's Doppler ratio is
 * 1.0 at zero relative velocity, so the authored factor multiplied nothing.
 *
 * This asks the ENGINE for the velocity rather than recomputing it: the test
 * links rt_audio_velocity, which is the single derivation the runtime uses for
 * both the listener and every voice.  A test that derives the answer a second
 * way is asserting against its own arithmetic, and this tree has paid for that
 * (a hinge axis disagreed with its own clamp by 22 degrees).
 *
 * THE TWO ZERO CASES ARE THE POINT.  Getting the arithmetic right is easy;
 * what makes Doppler usable rather than a bug report is refusing to answer
 * when there is no honest answer.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * The line that stood here said the opposite, and acting on it is why this
 * file sat untracked on a worktree eleven branches share.  Settle it with
 * `git check-ignore -v <path>`, never from memory.
 */
#include "unity.h"

#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <math.h>

/* The runtime's internal seam.  Declared here rather than including
 * jce_rt_internal.h, which drags in the whole runtime's private world. */
jce_vec3 rt_audio_velocity(jce_vec3 now, jce_vec3 then, bool have_then,
                           float dt);

void setUp(void) {}
void tearDown(void) {}

static jce_vec3 v3(float x, float y, float z)
{
    jce_vec3 v; v.x = x; v.y = y; v.z = z; return v;
}

static void test_plain_motion_is_the_delta_over_dt(void)
{
    /* 3 m along +X in 1/60 s = 180 m/s.  Deliberately a speed a game can
     * actually produce, so the teleport clamp below is not what passes it. */
    const jce_vec3 v = rt_audio_velocity(v3(3.0f, 0.0f, 0.0f),
                                         v3(0.0f, 0.0f, 0.0f),
                                         true, 1.0f / 60.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 180.0f, v.x);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, v.y);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, v.z);
}

static void test_the_first_frame_of_a_sound_reports_no_velocity(void)
{
    /* WITHOUT THIS GUARD every sound pitch-bends on the frame it starts: the
     * runtime would differentiate against an implicit origin, so a source
     * 100 m out at 60 fps reads as 6000 m/s.  The position is far from the
     * origin on purpose -- a guard that only worked near it would pass a
     * fixture at (0,0,0) and fail in every real scene. */
    const jce_vec3 v = rt_audio_velocity(v3(100.0f, -40.0f, 25.0f),
                                         v3(0.0f, 0.0f, 0.0f),
                                         false, 1.0f / 60.0f);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, v.x,
        "a voice with no previous position reported a velocity -- every sound "
        "would pitch-bend on the frame it starts");
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.y);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.z);
}

static void test_a_teleport_is_not_motion(void)
{
    /* A scene load, a camera cut, a respawn.  Nothing in a game moves at
     * 343 m/s, and the Doppler model divides by (c - v): past c it does not
     * exaggerate, it INVERTS.  A cut must not be heard as anything. */
    const jce_vec3 v = rt_audio_velocity(v3(5000.0f, 0.0f, 0.0f),
                                         v3(0.0f, 0.0f, 0.0f),
                                         true, 1.0f / 60.0f);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, v.x,
        "a 5 km jump in one frame was reported as motion; past the speed of "
        "sound the Doppler ratio inverts rather than exaggerating");

    /* AND THE CLAMP MUST NOT SWALLOW FAST-BUT-REAL MOTION.  A jet, a bullet
     * tracer, a launched rocket sit just under it; if this one returned zero
     * the guard would be a mute button on exactly the sounds Doppler is for. */
    const jce_vec3 fast = rt_audio_velocity(v3(5.0f, 0.0f, 0.0f),
                                            v3(0.0f, 0.0f, 0.0f),
                                            true, 1.0f / 60.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 300.0f, fast.x,
        "300 m/s -- fast but subsonic -- was clamped away; the teleport guard "
        "must not silence the motion Doppler exists to render");
}

static void test_a_stopped_clock_reports_no_velocity(void)
{
    /* dt == 0 happens on a paused frame and on the first tick of some
     * fixed-step paths.  Dividing there is an infinity handed to miniaudio. */
    const jce_vec3 zero = rt_audio_velocity(v3(1.0f, 2.0f, 3.0f),
                                            v3(0.0f, 0.0f, 0.0f),
                                            true, 0.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, zero.x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, zero.y);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, zero.z);

    const jce_vec3 neg = rt_audio_velocity(v3(1.0f, 2.0f, 3.0f),
                                           v3(0.0f, 0.0f, 0.0f),
                                           true, -0.016f);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, neg.x,
        "a negative dt produced a velocity pointing the wrong way");
}

static void test_standing_still_is_silence_not_noise(void)
{
    /* The common case: most voices do not move.  A derivation that produced
     * float dust here would give every static sound a faint permanent
     * pitch offset. */
    const jce_vec3 v = rt_audio_velocity(v3(12.5f, -3.25f, 7.0f),
                                         v3(12.5f, -3.25f, 7.0f),
                                         true, 1.0f / 60.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.y);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.z);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_plain_motion_is_the_delta_over_dt);
    RUN_TEST(test_the_first_frame_of_a_sound_reports_no_velocity);
    RUN_TEST(test_a_teleport_is_not_motion);
    RUN_TEST(test_a_stopped_clock_reports_no_velocity);
    RUN_TEST(test_standing_still_is_silence_not_noise);
    return UNITY_END();
}
