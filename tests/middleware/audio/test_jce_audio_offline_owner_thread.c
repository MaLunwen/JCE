/* Complete actual audio TU: public offline ownership on a post-mark worker.
 * Only the free primitive is observed; it always forwards to the real allocator. */
#include "unity.h"
#include <jce/middleware/audio/jce_audio.h>
#include <jce/os/core/jce_thread.h>
#include "os/core/jce_memory.h"
#include <stdint.h>
#include <string.h>

static void audio_worker_free(void *ptr);
#define jce__free_tracked audio_worker_free
#ifndef JCE_TEST_AUDIO_OFFLINE_SOURCE
#define JCE_TEST_AUDIO_OFFLINE_SOURCE "middleware/audio/jce_audio.c"
#endif
#include JCE_TEST_AUDIO_OFFLINE_SOURCE
#undef jce__free_tracked

static struct {
    JceAudio *audio;
    uintptr_t address;
    unsigned frees;
    bool worker_is_main, rendered;
    JceSound sound;
    float pcm[2048];
} offline_probe;
static void audio_worker_free(void *ptr)
{
    if (ptr && (uintptr_t)ptr == offline_probe.address) ++offline_probe.frees;
    jce__free_tracked(ptr);
}
static void offline_worker(void *unused)
{
    (void)unused;
    offline_probe.worker_is_main = jce_thread_is_main();
    offline_probe.audio = jce_audio_create_offline();
    if (!offline_probe.audio) return;
    offline_probe.address = (uintptr_t)offline_probe.audio;
    int16_t input[256];
    for (unsigned i = 0; i < 256; ++i) input[i] = 8192;
    offline_probe.sound = jce_audio_load_pcm(offline_probe.audio, input, sizeof(input), 1, 48000, 16);
    if (offline_probe.sound != JCE_SOUND_INVALID) {
        JceVoice voice = jce_audio_play(offline_probe.audio, offline_probe.sound, true, 1, 1);
        if (voice != JCE_VOICE_INVALID) {
            jce_audio_voice_set_3d(offline_probe.audio, voice, false);
            offline_probe.rendered = jce_audio_render_offline(offline_probe.audio, offline_probe.pcm, 1024);
        }
    }
    jce_audio_destroy(offline_probe.audio);
}
void setUp(void) { memset(&offline_probe, 0, sizeof(offline_probe)); }
void tearDown(void)
{
    /* A refused worker destroy remains owned, so the ordinary control can free
     * it on main after its assertion. Never inspect an already freed graph. */
    if (offline_probe.audio && offline_probe.frees == 0) jce_audio_destroy(offline_probe.audio);
}
static void test_post_mark_worker_owns_and_releases_offline_graph(void)
{
    jce_thread_mark_main();
    JceThread *worker = jce_thread_create(offline_worker, NULL, "audio-offline-owner-test");
    TEST_ASSERT_NOT_NULL(worker);
    jce_thread_join(worker);
    TEST_ASSERT_FALSE(offline_probe.worker_is_main);
    TEST_ASSERT_NOT_NULL(offline_probe.audio);
    TEST_ASSERT_TRUE(offline_probe.rendered);
    TEST_ASSERT_EQUAL_UINT(1, offline_probe.frees);
    float energy = 0;
    for (unsigned i = 0; i < 2048; ++i) energy += offline_probe.pcm[i] * offline_probe.pcm[i];
    TEST_ASSERT_GREATER_THAN_FLOAT(0, energy);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_post_mark_worker_owns_and_releases_offline_graph);
    return UNITY_END();
}
