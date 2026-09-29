/*
 * test_jce_audio_mixer.c — Unit tests for jce_audio_mixer.h (L4 audio).
 *
 * Pure bookkeeping (bus tree + voice routing).  No miniaudio touched.
 */

#include "unity.h"

#include <jce/middleware/audio/jce_audio_mixer.h>

void setUp(void)    {}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

static void test_create_has_master_bus(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_UINT32(1u, jce_audio_mixer_bus_count(m));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_audio_mixer_get_volume(m, JCE_AUDIO_BUS_MASTER));
    TEST_ASSERT_FALSE(jce_audio_mixer_is_muted(m, JCE_AUDIO_BUS_MASTER));
    jce_audio_mixer_destroy(m);
    jce_audio_mixer_destroy(NULL);  /* null-safe */
}

/* ------------------------------------------------------------------ */
/* Bus tree                                                             */
/* ------------------------------------------------------------------ */

static void test_add_bus_appends_under_parent(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId sfx = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "SFX", 0.8f);
    TEST_ASSERT_NOT_EQUAL(JCE_AUDIO_BUS_INVALID, sfx);
    TEST_ASSERT_EQUAL_UINT32(2u, jce_audio_mixer_bus_count(m));
    TEST_ASSERT_EQUAL_FLOAT(0.8f, jce_audio_mixer_get_volume(m, sfx));
    TEST_ASSERT_EQUAL_UINT16(JCE_AUDIO_BUS_MASTER, jce_audio_mixer_get_parent(m, sfx));
    TEST_ASSERT_EQUAL_STRING("SFX", jce_audio_mixer_get_name(m, sfx));
    jce_audio_mixer_destroy(m);
}

static void test_add_bus_rejects_bad_parent(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId orphan = jce_audio_mixer_add_bus(m, (JceAudioBusId)999, "Orphan", 1.0f);
    TEST_ASSERT_EQUAL_UINT16(JCE_AUDIO_BUS_INVALID, orphan);
    jce_audio_mixer_destroy(m);
}

static void test_find_bus_by_name(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId music = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 0.5f);
    TEST_ASSERT_EQUAL_UINT16(music, jce_audio_mixer_find_bus(m, "Music"));
    TEST_ASSERT_EQUAL_UINT16(JCE_AUDIO_BUS_INVALID, jce_audio_mixer_find_bus(m, "Ghost"));
    jce_audio_mixer_destroy(m);
}

static void test_remove_bus_frees_slot(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId tmp = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Tmp", 1.0f);
    TEST_ASSERT_TRUE(jce_audio_mixer_remove_bus(m, tmp));
    TEST_ASSERT_EQUAL_UINT32(1u, jce_audio_mixer_bus_count(m));
    TEST_ASSERT_FALSE(jce_audio_mixer_remove_bus(m, JCE_AUDIO_BUS_INVALID));
    jce_audio_mixer_destroy(m);
}

/* ------------------------------------------------------------------ */
/* Volume resolution                                                    */
/* ------------------------------------------------------------------ */

static void test_resolve_volume_multiplies_along_chain(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    jce_audio_mixer_set_volume(m, JCE_AUDIO_BUS_MASTER, 0.5f);
    JceAudioBusId sfx = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "SFX", 0.4f);
    JceAudioBusId gun = jce_audio_mixer_add_bus(m, sfx, "Gun", 0.5f);
    /* 0.5 * 0.4 * 0.5 = 0.1 */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.1f, jce_audio_mixer_resolve_volume(m, gun));
    jce_audio_mixer_destroy(m);
}

static void test_mute_zeroes_branch(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId sfx = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "SFX", 1.0f);
    JceAudioBusId gun = jce_audio_mixer_add_bus(m, sfx, "Gun", 1.0f);
    jce_audio_mixer_set_muted(m, sfx, true);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_mixer_resolve_volume(m, gun));
    TEST_ASSERT_TRUE(jce_audio_mixer_is_muted(m, sfx));
    jce_audio_mixer_set_muted(m, sfx, false);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_audio_mixer_resolve_volume(m, gun));
    jce_audio_mixer_destroy(m);
}

static void test_solo_mutes_non_solo_branches(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId sfx   = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "SFX",   1.0f);
    JceAudioBusId music = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 1.0f);
    jce_audio_mixer_set_solo(m, music, true);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_mixer_resolve_volume(m, sfx));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_audio_mixer_resolve_volume(m, music));
    /* clearing solo restores SFX. */
    jce_audio_mixer_set_solo(m, music, false);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_audio_mixer_resolve_volume(m, sfx));
    jce_audio_mixer_destroy(m);
}

static void test_solo_on_ancestor_keeps_descendant_audible(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId sfx = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "SFX", 1.0f);
    JceAudioBusId gun = jce_audio_mixer_add_bus(m, sfx, "Gun", 1.0f);
    jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 1.0f);
    jce_audio_mixer_set_solo(m, sfx, true);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_audio_mixer_resolve_volume(m, gun));
    jce_audio_mixer_destroy(m);
}

/* ------------------------------------------------------------------ */
/* Voice routing                                                        */
/* ------------------------------------------------------------------ */

static void test_voice_routing_round_trip(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId sfx = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "SFX", 1.0f);
    jce_audio_mixer_assign_voice(m, 42ull, sfx);
    TEST_ASSERT_EQUAL_UINT16(sfx, jce_audio_mixer_get_voice_bus(m, 42ull));
    jce_audio_mixer_unassign_voice(m, 42ull);
    TEST_ASSERT_EQUAL_UINT16(JCE_AUDIO_BUS_INVALID, jce_audio_mixer_get_voice_bus(m, 42ull));
    jce_audio_mixer_destroy(m);
}

static void test_list_buses_returns_creation_order(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId a = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "A", 1.0f);
    JceAudioBusId b = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "B", 1.0f);
    JceAudioBusId out[4] = {0};
    uint32_t n = jce_audio_mixer_list_buses(m, out, 4u);
    TEST_ASSERT_EQUAL_UINT32(3u, n);  /* master + A + B */
    TEST_ASSERT_EQUAL_UINT16(JCE_AUDIO_BUS_MASTER, out[0]);
    TEST_ASSERT_EQUAL_UINT16(a, out[1]);
    TEST_ASSERT_EQUAL_UINT16(b, out[2]);
    jce_audio_mixer_destroy(m);
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_has_master_bus);
    RUN_TEST(test_add_bus_appends_under_parent);
    RUN_TEST(test_add_bus_rejects_bad_parent);
    RUN_TEST(test_find_bus_by_name);
    RUN_TEST(test_remove_bus_frees_slot);
    RUN_TEST(test_resolve_volume_multiplies_along_chain);
    RUN_TEST(test_mute_zeroes_branch);
    RUN_TEST(test_solo_mutes_non_solo_branches);
    RUN_TEST(test_solo_on_ancestor_keeps_descendant_audible);
    RUN_TEST(test_voice_routing_round_trip);
    RUN_TEST(test_list_buses_returns_creation_order);
    return UNITY_END();
}
