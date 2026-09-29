/* Master-mix architecture contract (see .docs/AUDIO_MASTER_MIX_DESIGN.md):
 * every JceAudio is a device-less engine summed by ONE shared output device,
 * and jce_audio_master_tap_set() observes that final mix as a GAPLESS
 * 48 kHz / 2 ch / f32 stream — silence keeps flowing when engines are idle
 * or gone, engines can be created/destroyed mid-tap without breaking the
 * stream, and clearing the tap guarantees no further callbacks.
 *
 * Needs a real output device; self-ignores on machines without one (CI). */

#include "unity.h"

/* Atomics + sleep come from JCE's own threading facade, NOT raw SDL: the test
 * used to include <SDL3/SDL.h> without declaring any SDL dependency, which
 * only compiled because jce_core leaked SDL's include dirs PUBLIC-ly to
 * everything (see engine/CMakeLists.txt, audit cmake-jce-core-public-sdl). */
#include <jce/middleware/audio/jce_audio.h>
#include <jce/os/core/jce_thread.h>

#include <stdint.h>

void setUp(void) {}
void tearDown(void) {}

static JceAtomicI32 *s_chunks;
static JceAtomicI32 *s_frames;
static JceAtomicI32 *s_bad_format;

static void tap_cb(void *ud, const float *pcm, uint32_t frames,
                   uint32_t sample_rate, uint32_t channels)
{
    (void)ud;
    if (!pcm || sample_rate != 48000u || channels != 2u)
        jce_atomic_i32_store(s_bad_format, 1);
    jce_atomic_i32_add(s_chunks, 1);
    jce_atomic_i32_add(s_frames, (int)frames);
}

static void test_master_tap_gapless_across_engine_lifecycle(void)
{
    JceAudio *a = jce_audio_create();
    TEST_ASSERT_NOT_NULL(a);
    JceAudio *b = jce_audio_create();
    TEST_ASSERT_NOT_NULL(b);

    if (!jce_audio_master_tap_set(tap_cb, NULL)) {
        jce_audio_destroy(b);
        jce_audio_destroy(a);
        TEST_IGNORE_MESSAGE("no audio output device — master tap unavailable");
    }

    /* Two live engines: the tap must deliver at a steady cadence. */
    jce_thread_sleep_ms(300);
    int c1 = jce_atomic_i32_load(s_chunks);
    int f1 = jce_atomic_i32_load(s_frames);
    TEST_ASSERT_GREATER_THAN_INT(0, c1);
    /* ~300 ms at 48 kHz = 14400 frames; accept half to be scheduler-safe. */
    TEST_ASSERT_GREATER_THAN_INT(7200, f1);
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(s_bad_format));

    /* Destroy one engine mid-tap: stream must continue. */
    jce_audio_destroy(a);
    jce_thread_sleep_ms(200);
    int c2 = jce_atomic_i32_load(s_chunks);
    TEST_ASSERT_GREATER_THAN_INT(c1, c2);

    /* Destroy the last engine: device stays resident, silence keeps flowing
     * (this is what makes the recording stream gapless by construction). */
    jce_audio_destroy(b);
    jce_thread_sleep_ms(200);
    int c3 = jce_atomic_i32_load(s_chunks);
    TEST_ASSERT_GREATER_THAN_INT(c2, c3);

    /* Clearing the tap must fence: zero callbacks afterwards. */
    TEST_ASSERT_TRUE(jce_audio_master_tap_set(NULL, NULL));
    int c4 = jce_atomic_i32_load(s_chunks);
    jce_thread_sleep_ms(200);
    TEST_ASSERT_EQUAL_INT(c4, jce_atomic_i32_load(s_chunks));
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(s_bad_format));
}

static void test_engine_recreate_after_teardown(void)
{
    /* Engines must be creatable again after all were destroyed (slot reuse). */
    JceAudio *a = jce_audio_create();
    TEST_ASSERT_NOT_NULL(a);
    jce_audio_destroy(a);
}

int main(void)
{
    s_chunks     = jce_atomic_i32_create(0);
    s_frames     = jce_atomic_i32_create(0);
    s_bad_format = jce_atomic_i32_create(0);

    UNITY_BEGIN();
    RUN_TEST(test_master_tap_gapless_across_engine_lifecycle);
    RUN_TEST(test_engine_recreate_after_teardown);
    const int rc = UNITY_END();

    jce_atomic_i32_destroy(s_bad_format);
    jce_atomic_i32_destroy(s_frames);
    jce_atomic_i32_destroy(s_chunks);
    return rc;
}
