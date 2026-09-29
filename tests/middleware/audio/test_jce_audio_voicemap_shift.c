/* test_jce_audio_voicemap_shift.c
 *
 * Regression tests for REF-022 (dedup audit): the backward-shift repair that
 * runs when a voice is removed from the mixer's open-addressed voice->bus map.
 *
 * The bug: jce_audio_mixer_unassign_voice() used
 *     ((j - home) & mask) >  ((j - hole) & mask)
 * where the canonical Knuth Algorithm R predicate is `>=`.  The two forms
 * differ exactly when home == hole — i.e. the ordinary "two keys land in one
 * bucket" collision.  A colliding voice was then left stranded past an empty
 * slot, so the probe that looks it up stopped at the hole and reported
 * JCE_AUDIO_BUS_INVALID: the voice's bus routing was silently lost while
 * voices_size still counted it.
 *
 * These tests build the collision explicitly rather than assuming any
 * particular id pair collides, so they stay valid if the hash or the initial
 * capacity ever changes.  Before the fix they FAIL; after it they pass.
 */

#include <jce/middleware/audio/jce_audio_mixer.h>
#include <jce/os/core/jce_hash.h>

#include "unity.h"

#include <stdint.h>

/* The voice map starts at 64 slots and indexes with
 * jce_hash_fmix64(id) & (cap - 1) — mirrored here so the test can search for
 * a genuine collision instead of hard-coding magic ids. */
#define VOICE_MAP_INITIAL_CAP 64u

static uint32_t bucket_of(uint64_t id)
{
    return (uint32_t)jce_hash_fmix64(id) & (VOICE_MAP_INITIAL_CAP - 1u);
}

/* Find two distinct voice ids that hash to the same bucket. */
static bool find_colliding_pair(uint64_t *out_a, uint64_t *out_b)
{
    for (uint64_t a = 1; a < 4096; ++a) {
        for (uint64_t b = a + 1; b < 4096; ++b) {
            if (bucket_of(a) == bucket_of(b)) {
                *out_a = a;
                *out_b = b;
                return true;
            }
        }
    }
    return false;
}

void setUp(void)    {}
void tearDown(void) {}

/* THE regression: remove the voice sitting in the shared home bucket and the
 * one that probed past it must still resolve. */
static void test_unassign_keeps_colliding_voice_findable(void)
{
    uint64_t a = 0, b = 0;
    TEST_ASSERT_TRUE_MESSAGE(find_colliding_pair(&a, &b),
        "no colliding voice-id pair found; test premise broken");
    TEST_ASSERT_EQUAL_UINT32(bucket_of(a), bucket_of(b));

    JceAudioMixer *m = jce_audio_mixer_create();
    TEST_ASSERT_NOT_NULL(m);

    JceAudioBusId sfx   = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "SFX",   1.0f);
    JceAudioBusId music = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 0.7f);
    TEST_ASSERT_NOT_EQUAL_UINT(JCE_AUDIO_BUS_INVALID, sfx);
    TEST_ASSERT_NOT_EQUAL_UINT(JCE_AUDIO_BUS_INVALID, music);

    /* `a` takes the home slot; `b` collides and probes to home+1. */
    jce_audio_mixer_assign_voice(m, a, sfx);
    jce_audio_mixer_assign_voice(m, b, music);
    TEST_ASSERT_EQUAL_UINT(sfx,   jce_audio_mixer_get_voice_bus(m, a));
    TEST_ASSERT_EQUAL_UINT(music, jce_audio_mixer_get_voice_bus(m, b));

    /* Punch the hole at the shared home bucket. */
    jce_audio_mixer_unassign_voice(m, a);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(JCE_AUDIO_BUS_INVALID,
        jce_audio_mixer_get_voice_bus(m, a),
        "unassigned voice must no longer resolve");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(music,
        jce_audio_mixer_get_voice_bus(m, b),
        "colliding voice lost its bus after the other was unassigned "
        "(REF-022 backward-shift regression)");

    jce_audio_mixer_destroy(m);
}

/* Removing the SECOND of the pair must leave the first alone — the shift must
 * not move an entry that is already in its home slot. */
static void test_unassign_second_keeps_first(void)
{
    uint64_t a = 0, b = 0;
    TEST_ASSERT_TRUE(find_colliding_pair(&a, &b));

    JceAudioMixer *m = jce_audio_mixer_create();
    TEST_ASSERT_NOT_NULL(m);
    JceAudioBusId sfx = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "SFX", 1.0f);

    jce_audio_mixer_assign_voice(m, a, sfx);
    jce_audio_mixer_assign_voice(m, b, sfx);

    jce_audio_mixer_unassign_voice(m, b);
    TEST_ASSERT_EQUAL_UINT(JCE_AUDIO_BUS_INVALID, jce_audio_mixer_get_voice_bus(m, b));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(sfx, jce_audio_mixer_get_voice_bus(m, a),
        "removing the later colliding voice disturbed the home-slot entry");

    jce_audio_mixer_destroy(m);
}

/* A three-deep collision chain: removing the middle entry must keep BOTH
 * survivors reachable. */
static void test_three_way_collision_chain(void)
{
    uint64_t ids[3];
    int found = 0;
    uint32_t target = bucket_of(1);
    for (uint64_t v = 1; v < 200000 && found < 3; ++v) {
        if (bucket_of(v) == target) ids[found++] = v;
    }
    if (found < 3) {
        TEST_IGNORE_MESSAGE("could not build a 3-deep collision chain");
        return;
    }

    JceAudioMixer *m = jce_audio_mixer_create();
    TEST_ASSERT_NOT_NULL(m);
    JceAudioBusId sfx   = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "SFX",   1.0f);
    JceAudioBusId music = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 0.5f);

    jce_audio_mixer_assign_voice(m, ids[0], sfx);
    jce_audio_mixer_assign_voice(m, ids[1], music);
    jce_audio_mixer_assign_voice(m, ids[2], sfx);

    jce_audio_mixer_unassign_voice(m, ids[1]);   /* remove the middle link */

    TEST_ASSERT_EQUAL_UINT_MESSAGE(sfx, jce_audio_mixer_get_voice_bus(m, ids[0]),
        "head of the collision chain became unreachable");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(sfx, jce_audio_mixer_get_voice_bus(m, ids[2]),
        "tail of the collision chain became unreachable after the middle "
        "entry was removed (REF-022 backward-shift regression)");

    jce_audio_mixer_destroy(m);
}

/* jce_audio_mixer_remove_bus() used to blank matching slots in place with no
 * backward-shift repair, stranding any voice that probed past a cleared slot.
 * It now routes every drop through jce_audio_mixer_unassign_voice(). */
static void test_remove_bus_keeps_other_voices_findable(void)
{
    uint64_t ids[3];
    int found = 0;
    uint32_t target = bucket_of(1);
    for (uint64_t v = 1; v < 200000 && found < 3; ++v)
        if (bucket_of(v) == target) ids[found++] = v;
    if (found < 3) { TEST_IGNORE_MESSAGE("no 3-deep collision chain"); return; }

    JceAudioMixer *m = jce_audio_mixer_create();
    TEST_ASSERT_NOT_NULL(m);
    JceAudioBusId doomed = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Doomed", 1.0f);
    JceAudioBusId keep   = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Keep",   1.0f);

    /* Head and tail survive; the middle one dies with its bus. */
    jce_audio_mixer_assign_voice(m, ids[0], keep);
    jce_audio_mixer_assign_voice(m, ids[1], doomed);
    jce_audio_mixer_assign_voice(m, ids[2], keep);

    TEST_ASSERT_TRUE(jce_audio_mixer_remove_bus(m, doomed));

    TEST_ASSERT_EQUAL_UINT_MESSAGE(JCE_AUDIO_BUS_INVALID,
        jce_audio_mixer_get_voice_bus(m, ids[1]),
        "voice on the removed bus must be dropped");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(keep, jce_audio_mixer_get_voice_bus(m, ids[0]),
        "head of the collision chain lost its bus when another bus was removed");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(keep, jce_audio_mixer_get_voice_bus(m, ids[2]),
        "voice behind the removed one became unreachable "
        "(remove_bus punched a hole without backward-shift repair)");

    jce_audio_mixer_destroy(m);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_unassign_keeps_colliding_voice_findable);
    RUN_TEST(test_unassign_second_keeps_first);
    RUN_TEST(test_three_way_collision_chain);
    RUN_TEST(test_remove_bus_keeps_other_voices_findable);
    return UNITY_END();
}
