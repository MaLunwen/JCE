/*
 * test_jce_runtime_mixer_from_pak.c
 *
 * The runtime's audio_mixer.json must be readable from the MOUNTED PAK, not
 * only from the host filesystem.
 *
 * rt_mixer_read_config called jce_fs_host_read_all and nothing else.  In a
 * single-file exe the config lives inside the PAK, so that read missed and the
 * runtime seeded the DEFAULT bus tree instead: authored insert effects absent,
 * and -- once device-side aux sends existed -- authored sends absent too.  Both
 * silently, in the only build a player ever runs.  jce_build_manager said so in
 * its own comment ("a pure single-exe build does not consume this copy") and
 * the parity row for the insert chain recorded it as a known gap; adding send
 * routing put a second feature into the same blind spot, which is what made it
 * worth closing rather than noting again.
 *
 * WHAT IS OBSERVED, AND WHY IT IS THE DEVICE.  Not "the file was read" -- that
 * is not a capability.  The observable is jce_audio_bus_get_send on the LIVE
 * audio engine: a non-zero send from SFX into a bus the default tree does not
 * contain can only exist if the PAK copy was read, parsed, and routed all the
 * way to the node graph.  It proves the whole chain rather than any one link.
 *
 * THE PATH IS THE COOKED ONE AND THE DESC IS LEFT NULL ON PURPOSE.  That is
 * the shipped shape: jce_default_main only sets mixer_config_path when the
 * file exists on the HOST, so a single-file exe hands the runtime NULL.  A
 * test that supplied a path would prove the PAK branch works and say nothing
 * about whether anything ever reaches it.
 *
 * NO DEVICE IS NEEDED: jce_audio_create_offline() is handed to the runtime
 * through JceRuntimeDesc.audio, so the real node graph exists with no sound
 * hardware and no case may skip.
 *
 * IN THE TREE: tests/ is tracked on this branch and gitignored on `main`.
 * Settle it with `git check-ignore -v <path>`, never from memory.
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/audio/jce_audio.h>
#include <jce/resource/jce_archive_writer.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/os/core/jce_alloc.h>

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* A bus name no default tree seeds, so a send INTO it can only come from the
 * packed config having been consumed. */
#define MARKER_BUS "PakOnlyReturn"
#define SEND_AMOUNT 0.4f

/* The real schema, read off jce_audio_mixer_config.c rather than guessed:
 * buses carry an integer `id`, `parent` is an id (0 => Master), and sends are
 * { "dest", "amount" } objects nested INSIDE the source bus.
 *
 * ID 1 IS MASTER (JCE_AUDIO_BUS_MASTER), not a free id.  A first draft gave
 * SFX id 1; the parser took it for Master, discarded the name, and hung the
 * send off Master -- which the device mirror deliberately skips -- so the
 * route silently failed and the break looked like it was in the PAK read. */
static const char *k_cfg =
    "{\"buses\":["
      "{\"id\":2,\"name\":\"SFX\",\"parent\":1,\"volume\":1.0,"
        "\"sends\":[{\"dest\":3,\"amount\":0.4}]},"
      "{\"id\":3,\"name\":\"" MARKER_BUS "\",\"parent\":1,\"volume\":1.0}"
    "]}";

static void *build_cfg_pak(const char *path, size_t *out_size)
{
    JceArchiveWriter *w = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_TRUE(jce_archive_writer_add(w, path, k_cfg, strlen(k_cfg)));
    void *buf = NULL; size_t sz = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, &buf, &sz));
    TEST_ASSERT_NOT_NULL(buf);
    jce_archive_writer_destroy(w);
    *out_size = sz;
    return buf;
}

/* Spawn a runtime over an empty scene with `pak` mounted and an offline audio
 * engine, and report the device-side send amount SFX -> MARKER_BUS. */
static float routed_send(JcePakArchive *pak)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceAudio *audio = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(audio);

    JceRuntimeDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.scene = s;
    rd.pak   = pak;
    rd.audio = audio;

    JceRuntime *rt = jce_runtime_create(&rd);
    TEST_ASSERT_NOT_NULL(rt);

    const float got = jce_audio_bus_get_send(audio, "SFX", MARKER_BUS);

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
    jce_audio_destroy(audio);
    return got;
}

/* NEGATIVE CONTROL FIRST: with NO pak and no host config, nothing is routed.
 * Without it, "a send exists" is also satisfied by a default tree that happens
 * to contain one, and the case below would prove nothing. */
static void test_nothing_is_routed_without_a_config(void)
{
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(
        0.0f, routed_send(NULL),
        "a send into a bus only the packed config names was routed with no "
        "config readable at all -- the default tree already produces it, so "
        "this file cannot tell a successful read from a fallback");
}

/* THE REGRESSION: the packed config is consumed all the way to the graph. */
static void test_the_packed_mixer_config_reaches_the_device(void)
{
    size_t sz = 0;
    void *blob = build_cfg_pak("Settings/audio_mixer.json", &sz);
    JcePakArchive *pak = jce_pak_open(blob, sz);
    TEST_ASSERT_NOT_NULL_MESSAGE(pak, "the test PAK did not open");

    const float got = routed_send(pak);

    jce_pak_close(pak);
    jce_free(blob);

    TEST_ASSERT_TRUE_MESSAGE(got > 0.0f,
        "the runtime did not read audio_mixer.json from the mounted PAK, so a "
        "single-file exe still falls back to the default bus tree -- authored "
        "insert effects and aux sends both silently absent in the only build "
        "a player ever runs");
    /* The AMOUNT survives too, not merely the fact of a send: resolve_send
     * folds the source bus's own volume in, which is 1.0 here. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(
        0.05f, SEND_AMOUNT, got,
        "the send was routed but at the wrong gain -- the authored amount is "
        "being dropped somewhere between the packed config and the graph");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_nothing_is_routed_without_a_config);
    RUN_TEST(test_the_packed_mixer_config_reaches_the_device);
    return UNITY_END();
}
