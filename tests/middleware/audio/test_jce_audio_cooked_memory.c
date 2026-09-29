/* Cooked audio loaded from memory must follow the same representation
 * contract as audio loaded from a PAK. */

#include "unity.h"

#include <jce/middleware/audio/jce_audio.h>
#include <jce/resource/jce_asset_format.h>

#include "resource/jce_asset_cooker.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Desired API. Declared here first so the red test links against the missing
 * implementation before the public header is extended. */
extern JceAudioCpu *jce_audio_decode_cpu_memory(const void *data, size_t size,
                                                 const char *hint_path);

void setUp(void) {}
void tearDown(void) {}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static size_t make_test_wav(uint8_t out[52])
{
    static const int16_t samples[4] = {0, 2048, -2048, 0};
    memcpy(out + 0, "RIFF", 4);
    wr32(out + 4, 44);
    memcpy(out + 8, "WAVEfmt ", 8);
    wr32(out + 16, 16);
    wr16(out + 20, 1);
    wr16(out + 22, 1);
    wr32(out + 24, 8000);
    wr32(out + 28, 16000);
    wr16(out + 32, 2);
    wr16(out + 34, 16);
    memcpy(out + 36, "data", 4);
    wr32(out + 40, sizeof(samples));
    memcpy(out + 44, samples, sizeof(samples));
    return 52;
}

/* A WAV big enough for the JCEA container's fixed overhead to stop dominating.
 * The 52-byte fixture above cooks to 128 bytes -- 76 bytes of header and chunk
 * table -- so it can only ever measure the overhead, never the trade. */
#define BIG_FRAMES 8000
static size_t make_big_wav(uint8_t *out)
{
    const uint32_t data_bytes = (uint32_t)(BIG_FRAMES * sizeof(int16_t));
    memcpy(out + 0, "RIFF", 4);
    wr32(out + 4, 36u + data_bytes);
    memcpy(out + 8, "WAVEfmt ", 8);
    wr32(out + 16, 16);
    wr16(out + 20, 1);
    wr16(out + 22, 1);
    wr32(out + 24, 8000);
    wr32(out + 28, 16000);
    wr16(out + 32, 2);
    wr16(out + 34, 16);
    memcpy(out + 36, "data", 4);
    wr32(out + 40, data_bytes);
    /* A slow ramp: real audio, and compressible, which is why cooking a WAV
     * pays at all.  Random noise would make this test assert the opposite. */
    int16_t *pcm = (int16_t *)(out + 44);
    for (int i = 0; i < BIG_FRAMES; ++i)
        pcm[i] = (int16_t)((i % 256) * 64 - 8192);
    return 44u + data_bytes;
}

static void test_cooked_audio_memory_decodes(void)
{
    uint8_t wav[52];
    size_t wav_size = make_test_wav(wav);
    JceCookOptions opts = JCE_COOK_DEFAULT;
    opts.compression_level = 1;

    /* NULL: no sidecar.  These cook bytes already in memory, with no asset
     * path to look beside -- the same case the bundle packer is in, and the
     * reason the per-asset settings are a PARAMETER rather than a field of
     * JceCookOptions: a caller that has none passes none. */
    JceCookResult cooked = jce_cook_audio(wav, wav_size, &opts, NULL);
    TEST_ASSERT_TRUE(cooked.success);
    TEST_ASSERT_NOT_NULL(cooked.data);
    TEST_ASSERT_GREATER_THAN_UINT32(4u, (uint32_t)cooked.size);
    TEST_ASSERT_EQUAL_MEMORY("JCEA", cooked.data, 4);

    JceAudioCpu *cpu = jce_audio_decode_cpu_memory(
        cooked.data, cooked.size, "audio/test.mp3");
    TEST_ASSERT_NOT_NULL_MESSAGE(
        cpu, "JCEA audio in a bundle must not be sent to the MP3 decoder");

    jce_audio_cpu_free(cpu);
    jce_cook_result_free(&cooked);
}


/* ── Cooking audio is a trade, and the bundle packer must not lose it ──
 *
 * jce_cook_audio decodes to raw s16 PCM.  For a .wav that is a small win --
 * the container overhead goes -- and for a .ogg/.mp3/.opus it is a ~10x LOSS:
 * three minutes of 44.1 kHz stereo is 31 MB of PCM whatever it was encoded
 * from, and PCM does not compress its way back.  The main asset pak has always
 * shipped compressed sources verbatim (measured on the dogfood project: its
 * music entries store with compression NONE, ratio 1.00), while the bundle
 * packer decoded them -- so the SAME FILE shipped encoded through one route
 * and decoded through the other, with nothing saying so.
 *
 * jce_bundle_pack now keeps the source whenever cooking would make it bigger.
 * These two cases are what that rests on. */
static void test_cooking_a_real_wav_pays(void)
{
    /* THE CASE THAT KEEPS THE COOK PATH ALIVE.  A WAV is already PCM, so
     * cooking it can only win by compressing the payload -- measured on the
     * dogfood project's one bundled clip, 491,394 -> 349,109 bytes (0.7x).  If
     * this ever inverted, the packer's size guard would silently stop cooking
     * anything at all: still correct, but a whole path would go dead with no
     * signal, which is the failure mode this repository keeps finding. */
    static uint8_t wav[44 + BIG_FRAMES * sizeof(int16_t)];
    const size_t wav_size = make_big_wav(wav);
    JceCookOptions opts = JCE_COOK_DEFAULT;
    opts.compression_level = 1;

    JceCookResult cooked = jce_cook_audio(wav, wav_size, &opts, NULL);
    TEST_ASSERT_TRUE(cooked.success);
    TEST_ASSERT_NOT_NULL(cooked.data);
    TEST_ASSERT_TRUE_MESSAGE(cooked.size < wav_size,
        "cooking a realistic WAV must SHRINK it -- that is the whole reason "
        "the bundle packer still cooks audio rather than always keeping the "
        "source");
    jce_cook_result_free(&cooked);
}

static void test_cooking_a_tiny_clip_inflates_and_the_packer_must_notice(void)
{
    /* And the guard's other side.  The JCEA container costs a fixed 76 bytes
     * of header and chunk table, so anything small enough cooks BIGGER --
     * measured here at 52 -> 128.  That is the same shape as an .ogg, where
     * the loss is ~10x instead of 2.5x, and it is why the packer compares
     * sizes rather than switching on the file extension: an extension list
     * goes stale the first time somebody adds a format. */
    uint8_t wav[52];
    const size_t wav_size = make_test_wav(wav);
    JceCookOptions opts = JCE_COOK_DEFAULT;
    opts.compression_level = 1;

    JceCookResult cooked = jce_cook_audio(wav, wav_size, &opts, NULL);
    TEST_ASSERT_TRUE(cooked.success);
    TEST_ASSERT_TRUE_MESSAGE(cooked.size > wav_size,
        "this fixture is the INFLATING case; if it stops inflating it no "
        "longer exercises the guard and must be replaced, not deleted");
    jce_cook_result_free(&cooked);
}

static void test_an_uncooked_source_still_decodes(void)
{
    /* THE LOAD-BEARING CLAIM.  Keeping the source is only safe because the
     * runtime decodes an encoded bundle entry through the same call: without
     * this, the packer's size guard would ship a file nothing can open. */
    uint8_t wav[52];
    const size_t wav_size = make_test_wav(wav);

    JceAudioCpu *cpu = jce_audio_decode_cpu_memory(wav, wav_size,
                                                   "audio/kept.wav");
    TEST_ASSERT_NOT_NULL_MESSAGE(cpu,
        "an UNCOOKED source out of a bundle must decode -- this is what makes "
        "keeping the source instead of inflating it safe");
    jce_audio_cpu_free(cpu);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cooked_audio_memory_decodes);
    RUN_TEST(test_cooking_a_real_wav_pays);
    RUN_TEST(test_cooking_a_tiny_clip_inflates_and_the_packer_must_notice);
    RUN_TEST(test_an_uncooked_source_still_decodes);
    return UNITY_END();
}
