/*
 * test_jce_audio_import_settings.c — the per-clip audio options.
 *
 * WHY THIS EXISTS.  The entire cook-time policy for sound was one hardcoded
 * size heuristic in tools/jce_cook.c with no per-asset override in either
 * direction, so a 2.1 MB footstep set paid a decode on every trigger and a
 * 1.9 MB ambience loop was expanded to raw PCM in the PAK.
 *
 * WHAT THE ASSERTIONS ARE FOR.  Every failure this module can have is silent.
 * A sidecar that does not parse, a key read under the wrong name, an
 * out-of-range mode falling through to "cook" -- none of them errors; they
 * all just produce a PAK that is the wrong size, which nobody measures until
 * a build is too big to ship.  So the defaults are asserted to BE today's
 * behaviour, and each override is asserted to change the decision in the
 * direction it names AND to leave the other direction alone.
 */

#include "jce_audio_import_settings.h"

#include <jce/os/core/jce_filesystem.h>

#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "jce_test_file_util.h"

#define CLIP "test_clip.ogg"
#define SIDE "test_clip.ogg.import.json"

void setUp(void) {}
void tearDown(void)
{
    remove(SIDE);
    remove(CLIP);
}

static void write_sidecar(const char *json)
{
    jce_test_write_file(SIDE, json);
}

/* The whole point of a default: a clip nobody has opened must cook to the
 * bytes it cooked before this module existed. */
static void test_defaults_are_todays_behaviour(void)
{
    JceAudioImportSettings s;
    memset(&s, 0xCD, sizeof s);          /* not zero, so a missing write shows */
    jce_audio_import_settings_default(&s);

    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_AUDIO_COOK_AUTO, s.cook_mode,
        "the default is not AUTO -- every clip with no sidecar just changed "
        "how it cooks");
    TEST_ASSERT_FALSE(s.force_mono);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, s.sample_rate,
        "0 must mean 'keep the source rate'; any other default resamples "
        "every clip in every project that never asked");
    TEST_ASSERT_FALSE_MESSAGE(s.present,
        "`present` is true for a struct nobody loaded from a file");

    /* And the heuristic itself, at both sides of its edge. */
    TEST_ASSERT_TRUE(jce_audio_import_should_cook(&s, 1024));
    TEST_ASSERT_TRUE(jce_audio_import_should_cook(
        &s, JCE_AUDIO_COOK_AUTO_MAX_BYTES - 1));
    TEST_ASSERT_FALSE(jce_audio_import_should_cook(
        &s, JCE_AUDIO_COOK_AUTO_MAX_BYTES));
    TEST_ASSERT_FALSE_MESSAGE(jce_audio_import_should_cook(&s, 0),
        "a zero-byte source reported cookable -- that is an unreadable file, "
        "and cooking it produces an empty clip rather than an error");

    /* NULL settings must behave like defaults, not like all-off: the bundle
     * packer passes NULL and must keep cooking what it cooks today. */
    TEST_ASSERT_TRUE_MESSAGE(jce_audio_import_should_cook(NULL, 1024),
        "NULL settings did not fall back to the defaults, so the caller that "
        "has no sidecar to pass silently stopped cooking");
}

/* Both directions, because an override that only ever forces ON is half a
 * feature and the half that is missing is the one that keeps music streamed. */
static void test_the_override_works_in_both_directions(void)
{
    JceAudioImportSettings s;
    jce_audio_import_settings_default(&s);

    s.cook_mode = JCE_AUDIO_COOK_ALWAYS;
    TEST_ASSERT_TRUE_MESSAGE(
        jce_audio_import_should_cook(&s, 50u * 1024u * 1024u),
        "ALWAYS did not override the size heuristic for a large file");

    s.cook_mode = JCE_AUDIO_COOK_NEVER;
    TEST_ASSERT_FALSE_MESSAGE(jce_audio_import_should_cook(&s, 1024),
        "NEVER did not override the size heuristic for a small file -- the "
        "override only works in the direction that grows the PAK");
}

static void test_a_sidecar_is_read_key_by_key(void)
{
    JceAudioImportSettings s;

    write_sidecar("{\"kind\":2,\"cook_mode\":1,\"force_mono\":true,"
                  "\"sample_rate\":22050}");
    TEST_ASSERT_TRUE_MESSAGE(jce_audio_import_settings_load(CLIP, &s),
        "a well-formed audio sidecar did not load");
    TEST_ASSERT_TRUE(s.present);
    TEST_ASSERT_EQUAL_INT(JCE_AUDIO_COOK_ALWAYS, s.cook_mode);
    TEST_ASSERT_TRUE(s.force_mono);
    TEST_ASSERT_EQUAL_UINT32(22050u, s.sample_rate);

    /* A half-filled sidecar leaves the rest at the defaults rather than at
     * zero -- "the author set one thing" is not "the author cleared the
     * others". */
    write_sidecar("{\"kind\":2,\"force_mono\":true}");
    TEST_ASSERT_TRUE(jce_audio_import_settings_load(CLIP, &s));
    TEST_ASSERT_TRUE(s.force_mono);
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_AUDIO_COOK_AUTO, s.cook_mode,
        "an omitted cook_mode did not stay AUTO");
    TEST_ASSERT_EQUAL_UINT32(0u, s.sample_rate);
}

/* Every refusal path must leave the caller at the DEFAULTS, not at zeros.
 * An import that cannot read its options has to behave like one that has
 * none -- an all-off struct would stop cooking every clip in the project. */
static void test_every_refusal_falls_back_to_defaults(void)
{
    JceAudioImportSettings s;
    struct { const char *what; const char *json; } CASES[] = {
        { "a sidecar for another asset kind", "{\"kind\":1,\"cook_mode\":2}" },
        { "a sidecar with no kind at all",    "{\"cook_mode\":2}" },
        { "bytes that are not JSON",          "not json at all" },
        { "an empty file",                    "" },
    };
    const int n = (int)(sizeof CASES / sizeof CASES[0]);
    int i;

    for (i = 0; i < n; ++i) {
        char msg[192];
        write_sidecar(CASES[i].json);
        snprintf(msg, sizeof msg,
                 "%s was accepted, so `present` and the fields below are "
                 "reporting about a file that says nothing", CASES[i].what);
        TEST_ASSERT_FALSE_MESSAGE(jce_audio_import_settings_load(CLIP, &s), msg);
        snprintf(msg, sizeof msg,
                 "%s left cook_mode at %d instead of AUTO -- a refusal that "
                 "changes the decision is worse than one that errors",
                 CASES[i].what, s.cook_mode);
        TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_AUDIO_COOK_AUTO, s.cook_mode, msg);
        TEST_ASSERT_FALSE(s.force_mono);
        TEST_ASSERT_EQUAL_UINT32(0u, s.sample_rate);
        TEST_ASSERT_FALSE(s.present);
    }

    remove(SIDE);
    TEST_ASSERT_FALSE_MESSAGE(jce_audio_import_settings_load(CLIP, &s),
        "a missing sidecar reported as loaded");
    TEST_ASSERT_EQUAL_INT(JCE_AUDIO_COOK_AUTO, s.cook_mode);

    TEST_ASSERT_FALSE(jce_audio_import_settings_load(NULL, &s));
    TEST_ASSERT_FALSE(jce_audio_import_settings_load(CLIP, NULL));
}

/* A value from a newer editor must degrade to something NAMED, not to
 * whatever the switch's default branch happens to be that week. */
static void test_out_of_range_values_degrade_rather_than_reject(void)
{
    JceAudioImportSettings s;

    write_sidecar("{\"kind\":2,\"cook_mode\":99}");
    TEST_ASSERT_TRUE_MESSAGE(jce_audio_import_settings_load(CLIP, &s),
        "a sidecar carrying a mode this build does not know was refused "
        "outright; unknown VALUES must degrade, like unknown keys do");
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_AUDIO_COOK_AUTO, s.cook_mode,
        "an out-of-range cook_mode did not read as AUTO");

    /* A rate that would make the converter size a buffer from it, and the
     * two spellings of "not authored". */
    write_sidecar("{\"kind\":2,\"sample_rate\":99999999}");
    TEST_ASSERT_TRUE(jce_audio_import_settings_load(CLIP, &s));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, s.sample_rate,
        "an absurd sample rate was accepted; the converter sizes its output "
        "buffer from this number");

    write_sidecar("{\"kind\":2,\"sample_rate\":0}");
    TEST_ASSERT_TRUE(jce_audio_import_settings_load(CLIP, &s));
    TEST_ASSERT_EQUAL_UINT32(0u, s.sample_rate);

    write_sidecar("{\"kind\":2,\"sample_rate\":-48000}");
    TEST_ASSERT_TRUE(jce_audio_import_settings_load(CLIP, &s));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, s.sample_rate,
        "a negative rate did not read as 'keep the source rate'");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_are_todays_behaviour);
    RUN_TEST(test_the_override_works_in_both_directions);
    RUN_TEST(test_a_sidecar_is_read_key_by_key);
    RUN_TEST(test_every_refusal_falls_back_to_defaults);
    RUN_TEST(test_out_of_range_values_degrade_rather_than_reject);
    return UNITY_END();
}
