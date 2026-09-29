/*
 * test_jce_audio_voice_priority.c
 *
 * When the 64-voice pool is full, something has to be stolen.  Before this
 * change the only input to that decision was AGE, so a boss cue or a line of
 * dialogue was evicted by whatever happened to start after it, and an author
 * had no way to say otherwise -- JceAudioSourceComponent carried no priority
 * and neither did the voice API.
 *
 * NO DEVICE IS NEEDED and no case may skip: jce_audio_create_offline() runs
 * the real miniaudio node graph, and the allocation policy under test is
 * plain C that does not care whether a device exists.
 *
 * THE TWO HALVES ARE TESTED SEPARATELY BECAUSE EITHER ALONE IS USELESS:
 *
 *   choosing a better victim   without refusal, a protected voice still dies
 *                              once every voice is protected -- case D
 *   refusing                   without victim choice, "protected" only means
 *                              "dies last", which is what age already did
 *
 * A is the plain regression, B is the negative control that keeps A from
 * passing on a build that simply never steals, and C is the one that
 * separates priority from age -- it makes the protected voice the OLDEST, so
 * an age-only policy picks exactly the wrong victim and a priority-aware one
 * cannot.
 *
 * IN THE TREE: tests/ is tracked on this branch and gitignored on `main`.
 * Settle it with `git check-ignore -v <path>`, never from memory.
 */

#include <jce/middleware/audio/jce_audio.h>

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define SR         48000u
#define FRAMES     4800u          /* 100 ms, long enough to still be playing */
#define POOL       64             /* JCE_MAX_VOICES; not public, asserted below */

static int16_t s_pcm[FRAMES];

static void build_pcm(void)
{
    for (uint32_t i = 0; i < FRAMES; ++i)
        s_pcm[i] = (int16_t)((i % 64u) * 256u);
}

static JceSound make_sound(JceAudio *a)
{
    JceSound snd = jce_audio_load_pcm(a, s_pcm, (uint32_t)sizeof s_pcm,
                                      1u, SR, 16u);
    TEST_ASSERT_NOT_EQUAL(JCE_SOUND_INVALID, snd);
    return snd;
}

/* Fill the pool with looping voices at `priority`, returning the first one.
 * Looping so nothing finishes and frees a slot underneath the test. */
static JceVoice fill_pool(JceAudio *a, JceSound snd, int priority, int n)
{
    JceVoice first = JCE_VOICE_INVALID;
    for (int i = 0; i < n; ++i) {
        JceVoice v = jce_audio_play_priority(a, snd, true, 1.0f, 1.0f, priority);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(
            JCE_VOICE_INVALID, v,
            "could not fill the voice pool -- the rig is wrong and every "
            "assertion after this one is meaningless");
        if (i == 0) first = v;
    }
    return first;
}

/* ── The pool really is exhausted by POOL plays.  If the pool were larger,
 *    every case below would allocate a free slot and never exercise the
 *    stealing path at all -- and would pass. ─────────────────────────────── */
static void test_the_pool_is_full_after_sixty_four_voices(void)
{
    build_pcm();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    JceSound snd = make_sound(a);

    fill_pool(a, snd, JCE_AUDIO_PRIORITY_NORMAL, POOL);
    /* The 65th at LOWER priority than everything live must be refused, which
     * is only possible if slots 1..64 are all occupied. */
    JceVoice over = jce_audio_play_priority(a, snd, true, 1.0f, 1.0f, -1);
    jce_audio_destroy(a);

    TEST_ASSERT_EQUAL_MESSAGE(
        JCE_VOICE_INVALID, over,
        "a 65th voice was allocated, so POOL does not match JCE_MAX_VOICES "
        "and none of the stealing cases in this file are reaching the "
        "stealing path");
}

/* ── A: a protected voice survives the pool running out. ────────────────── */
static void test_a_protected_voice_is_not_stolen(void)
{
    build_pcm();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    JceSound snd = make_sound(a);

    /* One important voice first, then fill the rest with ordinary ones. */
    JceVoice vip = jce_audio_play_priority(a, snd, true, 1.0f, 1.0f, 10);
    TEST_ASSERT_NOT_EQUAL(JCE_VOICE_INVALID, vip);
    fill_pool(a, snd, JCE_AUDIO_PRIORITY_NORMAL, POOL - 1);

    /* Now force ten steals with ordinary sounds. */
    for (int i = 0; i < 10; ++i)
        TEST_ASSERT_NOT_EQUAL(
            JCE_VOICE_INVALID,
            jce_audio_play(a, snd, true, 1.0f, 1.0f));

    int prio = jce_audio_voice_get_priority(a, vip);
    jce_audio_destroy(a);

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        10, prio,
        "the protected voice was stolen -- a stale handle reads back priority "
        "0, so this is the boss-cue case the row was opened for");
}

/* ── B: negative control.  Stealing still HAPPENS; an ordinary voice in the
 *    same pool does get taken.  Without this, case A also passes on a build
 *    that refuses every allocation. ──────────────────────────────────────── */
static void test_an_ordinary_voice_is_still_stolen(void)
{
    build_pcm();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    JceSound snd = make_sound(a);

    JceVoice first = fill_pool(a, snd, JCE_AUDIO_PRIORITY_NORMAL, POOL);
    TEST_ASSERT_NOT_EQUAL(JCE_VOICE_INVALID, first);

    /* One more ordinary sound: the oldest ordinary voice is the victim. */
    JceVoice extra = jce_audio_play(a, snd, true, 1.0f, 1.0f);
    jce_audio_destroy(a);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        JCE_VOICE_INVALID, extra,
        "nothing was stolen when the pool was full of equal-priority voices "
        "-- the policy refuses where it should evict, and case A above would "
        "pass for that reason rather than for the right one");
}

/* ── C: priority beats age, which is the whole point.  The protected voice is
 *    deliberately the OLDEST, so an age-only policy picks it first. ─────── */
static void test_priority_outranks_age(void)
{
    build_pcm();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    JceSound snd = make_sound(a);

    JceVoice oldest_vip = jce_audio_play_priority(a, snd, true, 1.0f, 1.0f, 5);
    TEST_ASSERT_NOT_EQUAL(JCE_VOICE_INVALID, oldest_vip);
    fill_pool(a, snd, JCE_AUDIO_PRIORITY_NORMAL, POOL - 1);

    JceVoice extra = jce_audio_play(a, snd, true, 1.0f, 1.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_VOICE_INVALID, extra);

    int prio = jce_audio_voice_get_priority(a, oldest_vip);
    jce_audio_destroy(a);

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        5, prio,
        "the OLDEST voice was stolen even though it was the most important "
        "one -- the victim is still being chosen by age");
}

/* ── D: the refusal.  With every voice outranking it, a new sound is dropped
 *    rather than evicting something more important. ─────────────────────── */
static void test_a_lesser_sound_is_refused_rather_than_stealing(void)
{
    build_pcm();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    JceSound snd = make_sound(a);

    fill_pool(a, snd, 7, POOL);

    JceVoice lesser = jce_audio_play_priority(a, snd, true, 1.0f, 1.0f, 3);
    /* ...and an EQUAL one still gets in, so the rule is "strictly lower is
     * refused" rather than "anything is refused once the pool is full". */
    JceVoice equal = jce_audio_play_priority(a, snd, true, 1.0f, 1.0f, 7);
    jce_audio_destroy(a);

    TEST_ASSERT_EQUAL_MESSAGE(
        JCE_VOICE_INVALID, lesser,
        "a less important sound evicted a more important one -- choosing a "
        "better victim is not enough on its own, because every voice in the "
        "pool being a boss cue is exactly when it matters");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        JCE_VOICE_INVALID, equal,
        "an EQUAL-priority sound was refused -- the refusal is meant to be "
        "strict, and this would silence every sound once the pool filled");
}

/* ── E: re-prioritising a playing voice takes effect. ───────────────────── */
static void test_set_priority_protects_an_already_playing_voice(void)
{
    build_pcm();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    JceSound snd = make_sound(a);

    /* Plays at NORMAL like every pre-existing caller, then is promoted. */
    JceVoice v = jce_audio_play(a, snd, true, 1.0f, 1.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_VOICE_INVALID, v);
    jce_audio_voice_set_priority(a, v, 9);
    TEST_ASSERT_EQUAL_INT(9, jce_audio_voice_get_priority(a, v));

    fill_pool(a, snd, JCE_AUDIO_PRIORITY_NORMAL, POOL - 1);
    for (int i = 0; i < 10; ++i)
        jce_audio_play(a, snd, true, 1.0f, 1.0f);

    int prio = jce_audio_voice_get_priority(a, v);
    jce_audio_destroy(a);

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        9, prio,
        "a voice promoted AFTER it started playing was still stolen -- "
        "jce_audio_voice_set_priority is not reaching the allocator");
}

/* ── F: the victim ORDERING, which nothing above reaches.
 *
 * A mutation replacing the ordered comparison with plain age SURVIVED cases
 * A-E.  The reason is that in all of them the incoming sound is at NORMAL and
 * the protected voice outranks it, so the REFUSAL GUARD removes it from the
 * candidate set before the ordering ever compares anything -- two mechanisms,
 * one of them carrying every test.
 *
 * The ordering only matters when the newcomer OUTRANKS several live voices
 * that differ among themselves: the guard then excludes nobody, and the least
 * important of them has to be the one that dies.  Same discriminating trick
 * as case C -- the voice that must survive is deliberately the OLDEST, so an
 * age-only policy picks exactly it. ─────────────────────────────────────── */
static void test_the_least_important_candidate_is_the_victim(void)
{
    build_pcm();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    JceSound snd = make_sound(a);

    /* Oldest voice in the pool, and the most important of the candidates. */
    JceVoice mid = jce_audio_play_priority(a, snd, true, 1.0f, 1.0f, 5);
    TEST_ASSERT_NOT_EQUAL(JCE_VOICE_INVALID, mid);
    fill_pool(a, snd, JCE_AUDIO_PRIORITY_NORMAL, POOL - 1);

    /* Outranks EVERYTHING live, so the refusal guard excludes nobody and the
     * choice is the ordering's alone. */
    JceVoice top = jce_audio_play_priority(a, snd, true, 1.0f, 1.0f, 10);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        JCE_VOICE_INVALID, top,
        "a sound outranking every live voice was refused -- the guard is "
        "rejecting candidates it should admit");

    int prio = jce_audio_voice_get_priority(a, mid);
    jce_audio_destroy(a);

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        5, prio,
        "the OLDEST voice was stolen even though 63 less important ones were "
        "available -- the victim is being chosen by age, and cases A-E cannot "
        "see it because the refusal guard protects their voices instead");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_pool_is_full_after_sixty_four_voices);
    RUN_TEST(test_a_protected_voice_is_not_stolen);
    RUN_TEST(test_an_ordinary_voice_is_still_stolen);
    RUN_TEST(test_priority_outranks_age);
    RUN_TEST(test_a_lesser_sound_is_refused_rather_than_stealing);
    RUN_TEST(test_set_priority_protects_an_already_playing_voice);
    RUN_TEST(test_the_least_important_candidate_is_the_victim);
    return UNITY_END();
}
