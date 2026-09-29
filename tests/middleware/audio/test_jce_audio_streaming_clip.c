/*
 * test_jce_audio_streaming_clip.c
 *
 * Every clip was decompress-on-load, with no engine-provided alternative:
 * jce_audio.c decoded the whole file into one s16 allocation and handed that
 * finished buffer to ma_audio_buffer_init.  A five-minute stereo track cost
 * ~50 MB resident and a full decode before the first sample.  There was no
 * MA_SOUND_FLAG_STREAM and no ma_sound_init_from_file anywhere in the file,
 * and jce_audio_play_stream -- which does exist -- is a bare pull callback
 * whose caller must supply its own decoder and its own thread-safe ring.
 *
 * WHAT MAKES THIS A STREAMING TEST RATHER THAN A PLAYBACK TEST.  "It makes
 * sound" is satisfied by the old decode-on-load path too.  The load-bearing
 * assertion is that NOTHING WAS DECODED: jce_audio_get_pcm_data returns NULL
 * for a streaming sound because there is no decoded buffer to hand out, while
 * the same clip loaded normally returns one.  Two loads of one payload,
 * differing only in which entry point was used, is what separates the two.
 *
 * NO DEVICE IS NEEDED and no case may skip: jce_audio_create_offline() plus
 * jce_audio_render_offline() run the real graph, decoder included.
 *
 * IN THE TREE: tests/ is tracked on this branch and gitignored on `main`.
 * Settle it with `git check-ignore -v <path>`, never from memory.
 */

#include <jce/middleware/audio/jce_audio.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/resource/jce_archive_writer.h>
#include <jce/resource/jce_pak_loader.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define SR       48000u
#define FRAMES   4800u            /* 100 ms mono */
#define RENDER   512u

/* A minimal 16-bit mono WAV, built in memory.  WAV because dr_wav is compiled
 * into miniaudio unconditionally -- an .ogg fixture would make this case
 * depend on which codecs this build happens to carry. */
static uint8_t s_wav[44 + FRAMES * 2];
static char    s_wav_path[512];

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8);
}

static void build_wav(void)
{
    const uint32_t data_bytes = FRAMES * 2u;
    memcpy(s_wav, "RIFF", 4);
    put_u32(s_wav + 4, 36u + data_bytes);
    memcpy(s_wav + 8, "WAVEfmt ", 8);
    put_u32(s_wav + 16, 16u);              /* fmt chunk size   */
    put_u16(s_wav + 20, 1u);               /* PCM              */
    put_u16(s_wav + 22, 1u);               /* mono             */
    put_u32(s_wav + 24, SR);
    put_u32(s_wav + 28, SR * 2u);          /* byte rate        */
    put_u16(s_wav + 32, 2u);               /* block align      */
    put_u16(s_wav + 34, 16u);              /* bits per sample  */
    memcpy(s_wav + 36, "data", 4);
    put_u32(s_wav + 40, data_bytes);
    for (uint32_t i = 0; i < FRAMES; ++i) {
        int16_t v = (int16_t)((i % 32u) < 16u ? 12000 : -12000);
        put_u16(s_wav + 44 + i * 2u, (uint16_t)v);
    }
}

/* The loader takes a PAK or a host path; writing the fixture to disk exercises
 * the same branch a loose-project editor run takes. */
static const char *wav_on_disk(void)
{
    if (s_wav_path[0]) return s_wav_path;
    snprintf(s_wav_path, sizeof s_wav_path, "test_stream_fixture.wav");
    FILE *f = fopen(s_wav_path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "could not write the WAV fixture");
    TEST_ASSERT_EQUAL_size_t(sizeof s_wav, fwrite(s_wav, 1, sizeof s_wav, f));
    fclose(f);
    return s_wav_path;
}

/* ── the discriminator ──────────────────────────────────────────────── */

/* A streaming sound holds NO decoded PCM; the same payload loaded normally
 * does.  This is the only assertion that separates streaming from a decode
 * that happens to work. */
static void test_a_streaming_clip_holds_no_decoded_pcm(void)
{
    build_wav();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);

    JceSound st = jce_audio_load_streaming(a, NULL, wav_on_disk());
    TEST_ASSERT_NOT_EQUAL_MESSAGE(JCE_SOUND_INVALID, st,
        "the streaming loader refused a plain 16-bit mono WAV");

    uint32_t frames = 0, chans = 0;
    const int16_t *pcm = jce_audio_get_pcm_data(a, st, &frames, &chans);
    TEST_ASSERT_NULL_MESSAGE(pcm,
        "a STREAMING sound handed out a decoded PCM buffer -- it decoded the "
        "clip up front after all, which is the cost this entry point exists "
        "to avoid");

    /* POSITIVE CONTROL on the same payload: loaded the ordinary way it DOES
     * hold PCM, so the NULL above is a property of streaming and not of this
     * fixture or of get_pcm_data being broken. */
    JceSound dec = jce_audio_load_pcm(a, s_wav + 44, FRAMES * 2u, 1u, SR, 16u);
    TEST_ASSERT_NOT_EQUAL(JCE_SOUND_INVALID, dec);
    const int16_t *pcm2 = jce_audio_get_pcm_data(a, dec, &frames, &chans);
    TEST_ASSERT_NOT_NULL_MESSAGE(pcm2,
        "the ordinary load path reports no PCM either, so the assertion above "
        "says nothing about streaming");

    jce_audio_destroy(a);
}

/* ── it actually sounds ─────────────────────────────────────────────── */

static float peak_of(JceAudio *a, JceVoice v)
{
    TEST_ASSERT_NOT_EQUAL(JCE_VOICE_INVALID, v);
    float out[RENDER * 2], peak = 0.0f;
    for (int n = 0; n < 8; ++n) {
        memset(out, 0, sizeof out);
        TEST_ASSERT_TRUE(jce_audio_render_offline(a, out, RENDER));
        for (uint32_t i = 0; i < RENDER * 2u; ++i) {
            float m = fabsf(out[i]);
            if (m > peak) peak = m;
        }
    }
    return peak;
}

static void test_a_streaming_clip_renders_audio(void)
{
    build_wav();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    JceSound st = jce_audio_load_streaming(a, NULL, wav_on_disk());
    TEST_ASSERT_NOT_EQUAL(JCE_SOUND_INVALID, st);

    const float peak = peak_of(a, jce_audio_play(a, st, true, 1.0f, 1.0f));
    jce_audio_destroy(a);

    TEST_ASSERT_TRUE_MESSAGE(peak > 0.01f,
        "a streaming voice rendered silence -- the decoder is in the graph but "
        "producing nothing, which no amount of saved memory makes acceptable");
}

/* The WAV fixture is shared by several cases above, so it is removed once at
 * the end of main() rather than by whichever case happens to run last. */
static void remove_shared_fixture(void)
{
    if (s_wav_path[0]) remove(s_wav_path);
}

/* Two voices on ONE streaming clip need independent read cursors, exactly as
 * two ma_audio_buffers do over shared PCM.  A single shared decoder would let
 * them consume each other's frames; both must be audible together. */
static void test_two_voices_on_one_streaming_clip_both_play(void)
{
    build_wav();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);
    JceSound st = jce_audio_load_streaming(a, NULL, wav_on_disk());
    TEST_ASSERT_NOT_EQUAL(JCE_SOUND_INVALID, st);

    JceVoice v1 = jce_audio_play(a, st, true, 1.0f, 1.0f);
    JceVoice v2 = jce_audio_play(a, st, true, 1.0f, 1.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_VOICE_INVALID, v1);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(JCE_VOICE_INVALID, v2,
        "a second voice on the same streaming clip was refused");
    TEST_ASSERT_TRUE_MESSAGE(v1 != v2, "both plays returned the same voice");

    const float peak = peak_of(a, v1);
    TEST_ASSERT_TRUE_MESSAGE(peak > 0.01f,
        "two voices on one streaming clip rendered silence together");

    /* AND THEIR CURSORS ARE INDEPENDENT, which is the half "both are audible"
     * cannot see: a mutation pointing every voice at voices[0].dec SURVIVED
     * the assertion above, because sharing one cursor still makes sound.
     * v1 has been rendered for eight blocks; v3 starts now, so if the decoder
     * were shared v3 would inherit v1's advanced position instead of
     * beginning at zero. */
    JceVoice v3 = jce_audio_play(a, st, true, 1.0f, 1.0f);
    TEST_ASSERT_NOT_EQUAL(JCE_VOICE_INVALID, v3);
    const float t1 = jce_audio_get_time(a, v1);
    const float t3 = jce_audio_get_time(a, v3);
    jce_audio_destroy(a);

    TEST_ASSERT_TRUE_MESSAGE(t1 > 0.001f,
        "the first voice reports position 0 after eight rendered blocks, so "
        "this case cannot tell a shared cursor from a fresh one");
    TEST_ASSERT_TRUE_MESSAGE(t3 < t1 * 0.5f,
        "a voice started later reports the SAME playback position as one that "
        "has been rendering -- the voices share a decoder, so they consume "
        "each other's frames");
}

/* A load failure must be visible AT LOAD, not as a silent voice later. */
static void test_an_undecodable_payload_is_refused_at_load(void)
{
    build_wav();
    JceAudio *a = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(a);

    TEST_ASSERT_EQUAL_MESSAGE(
        JCE_SOUND_INVALID,
        jce_audio_load_streaming(a, NULL, "no_such_file_anywhere.wav"),
        "a missing file produced a valid sound handle");

    /* Present but not audio: the decoder must reject it here rather than at
     * first play, where the cause is far away. */
    FILE *f = fopen("test_stream_garbage.bin", "wb");
    TEST_ASSERT_NOT_NULL(f);
    const char junk[64] = { 'n','o','t','a','u','d','i','o' };
    fwrite(junk, 1, sizeof junk, f);
    fclose(f);
    TEST_ASSERT_EQUAL_MESSAGE(
        JCE_SOUND_INVALID,
        jce_audio_load_streaming(a, NULL, "test_stream_garbage.bin"),
        "an undecodable payload was accepted, so the failure would surface as "
        "a silent voice at play time instead of here");

    jce_audio_destroy(a);
    /* Leave no fixtures behind.  Tests run in a SHARED build directory that
     * other sessions also work in, and an unexplained file in `git status` is
     * how a concurrent session detects that someone else is in its checkout --
     * so droppings here cost somebody else a false alarm. */
    remove("test_stream_garbage.bin");
}

/* -- where the bytes live -------------------------------------------
 *
 * "Not decoded" was only half of it.  The first version of the loader read the
 * whole encoded file into a JCE_MALLOC'd block and kept it for the slot's
 * lifetime -- Unity's COMPRESSED IN MEMORY, not its STREAMING, while this
 * row's title says "from disk".  The cases below pin down where the bytes
 * actually are, which "it plays and holds no PCM" cannot see.
 */

/* An INCOMPRESSIBLE payload, so the archive writer stores it raw.  ZSTD is
 * kept only when it beats 95% (compress_keep, jce_archive_writer.c:213), and
 * real music containers never do -- this LCG stands in for that.  Still a
 * valid WAV: dr_wav does not care that the samples are noise. */
static uint8_t s_noise[44 + FRAMES * 2];

static void build_noise_wav(void)
{
    build_wav();
    memcpy(s_noise, s_wav, sizeof s_wav);
    uint32_t st = 0x13579BDFu;
    for (uint32_t i = 0; i < FRAMES; ++i) {
        st = st * 1664525u + 1013904223u;
        put_u16(s_noise + 44 + i * 2u, (uint16_t)(st >> 16));
    }
}

static void *pack_one(const char *path, const void *bytes, size_t n,
                      size_t *out_size)
{
    JceArchiveWriter *w = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_TRUE(jce_archive_writer_add(w, path, bytes, n));
    void *buf = NULL; size_t sz = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, &buf, &sz));
    jce_archive_writer_destroy(w);
    *out_size = sz;
    return buf;
}

#define PAK_CLIP "Audio/clip.wav"

/* A STORED pak entry is a flat pointer into the archive blob, so those bytes
 * are ALREADY resident: copying them would double the very cost this entry
 * point exists to avoid.  The borrow is publicly observable -- only the borrow
 * path takes a reference on the archive, so jce_pak_refcount rises. */
static void test_a_stored_pak_clip_is_borrowed_not_copied(void)
{
    build_noise_wav();
    size_t sz = 0;
    void *blob = pack_one(PAK_CLIP, s_noise, sizeof s_noise, &sz);
    JcePakArchive *pak = jce_pak_open(blob, sz);
    TEST_ASSERT_NOT_NULL(pak);

    /* THE FIXTURE MUST HAVE ACHIEVED WHAT IT INTENDED.  If ZSTD happened to
     * shrink this payload the entry would be compressed, the copy path would
     * run, and the assertion below would fail for a reason that has nothing
     * to do with the loader. */
    const JcePakAsset *a = jce_pak_find(pak, PAK_CLIP);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE_MESSAGE(a->flags & JCE_PAK_ASSET_STORED,
        "the noise fixture was compressed by the archive writer, so this case "
        "is exercising the copy path and proves nothing about borrowing");
    TEST_ASSERT_NOT_NULL_MESSAGE((void *)a->compressed_data,
        "a STORED entry exposed no flat pointer");

    const int before = jce_pak_refcount(pak);

    JceAudio *au = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(au);
    JceSound st = jce_audio_load_streaming(au, pak, PAK_CLIP);
    TEST_ASSERT_NOT_EQUAL(JCE_SOUND_INVALID, st);

    const int during = jce_pak_refcount(pak);

    /* It plays from the borrowed bytes, not merely loads. */
    const float peak = peak_of(au, jce_audio_play(au, st, true, 1.0f, 1.0f));

    jce_audio_unload(au, st);
    const int after = jce_pak_refcount(pak);

    jce_audio_destroy(au);
    jce_pak_close(pak);
    jce_free(blob);

    TEST_ASSERT_EQUAL_INT_MESSAGE(before + 1, during,
        "loading a STORED pak clip took no reference on the archive -- it "
        "copied the encoded bytes instead of pointing at the blob, so the "
        "clip is resident TWICE in a shipped single-file exe");
    TEST_ASSERT_EQUAL_INT_MESSAGE(before, after,
        "unloading the clip did not release the archive reference it took");
    TEST_ASSERT_TRUE_MESSAGE(peak > 0.01f,
        "a clip playing from borrowed pak bytes rendered silence");
}

/* NEGATIVE CONTROL.  A COMPRESSED entry has no flat pointer, so it must be
 * expanded once and held -- and must NOT take a reference.  Without this the
 * case above is also satisfied by acquiring unconditionally. */
static void test_a_compressed_pak_clip_is_copied(void)
{
    build_wav();                      /* square wave: ZSTD crushes it */
    size_t sz = 0;
    void *blob = pack_one(PAK_CLIP, s_wav, sizeof s_wav, &sz);
    JcePakArchive *pak = jce_pak_open(blob, sz);
    TEST_ASSERT_NOT_NULL(pak);

    const JcePakAsset *a = jce_pak_find(pak, PAK_CLIP);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_FALSE_MESSAGE(a->flags & JCE_PAK_ASSET_STORED,
        "the square-wave fixture was stored raw, so this is a second copy of "
        "the borrow case rather than a control for it");

    const int before = jce_pak_refcount(pak);

    JceAudio *au = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(au);
    JceSound st = jce_audio_load_streaming(au, pak, PAK_CLIP);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(JCE_SOUND_INVALID, st,
        "a COMPRESSED pak entry was refused by the streaming loader");

    const int during = jce_pak_refcount(pak);
    const float peak = peak_of(au, jce_audio_play(au, st, true, 1.0f, 1.0f));

    jce_audio_destroy(au);
    jce_pak_close(pak);
    jce_free(blob);

    TEST_ASSERT_EQUAL_INT_MESSAGE(before, during,
        "a COMPRESSED entry took an archive reference, so the refcount in the "
        "borrow case says nothing about which path ran");
    TEST_ASSERT_TRUE_MESSAGE(peak > 0.01f,
        "a compressed-in-memory clip rendered silence");
}

/* THE HOST PATH HOLDS NOTHING.  Proving that needs the file to stop existing:
 * if the loader had read it into memory, playback would carry on regardless.
 * This is the same trade Unity's Streaming load type makes -- the asset has to
 * still be there at play time -- and it is the only way to tell "streams from
 * disk" from "read the whole thing at load". */
static void test_a_host_clip_holds_nothing_resident(void)
{
    build_wav();
    const char *p = "test_stream_vanishing.wav";
    FILE *f = fopen(p, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_size_t(sizeof s_wav, fwrite(s_wav, 1, sizeof s_wav, f));
    fclose(f);

    JceAudio *au = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(au);
    JceSound st = jce_audio_load_streaming(au, NULL, p);
    TEST_ASSERT_NOT_EQUAL(JCE_SOUND_INVALID, st);

    /* POSITIVE CONTROL: while the file is there it plays.  Without this, the
     * silence below is also what a broken loader produces. */
    const JceVoice live_v = jce_audio_play(au, st, true, 1.0f, 1.0f);
    const float    live   = peak_of(au, live_v);
    TEST_ASSERT_TRUE_MESSAGE(live > 0.01f,
        "the clip rendered silence while its file still existed, so the case "
        "below cannot attribute silence to the file being gone");

    /* A PLAYING streaming voice holds the file open -- that is what streaming
     * is -- so it has to be stopped before the file can go.  Which makes the
     * remove() below a second real assertion: if stopping a voice did not
     * release its handle, a game could never replace or patch an audio file
     * again for the life of the process, and on POSIX (where an open file can
     * still be unlinked) that leak would never show up at all. */
    jce_audio_stop(au, live_v);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, remove(p),
        "the fixture could not be deleted after its only voice was stopped, "
        "so a streaming voice leaks its file handle -- and nothing below is "
        "being measured either");

    /* A voice started NOW has no file to open.  The old copy-into-RAM loader
     * would have played on happily. */
    const JceVoice v = jce_audio_play(au, st, true, 1.0f, 1.0f);
    float out[RENDER * 2], peak = 0.0f;
    for (int n = 0; n < 8; ++n) {
        memset(out, 0, sizeof out);
        TEST_ASSERT_TRUE(jce_audio_render_offline(au, out, RENDER));
        for (uint32_t i = 0; i < RENDER * 2u; ++i) {
            const float m = fabsf(out[i]);
            if (m > peak) peak = m;
        }
    }
    /* CONTROL FOR THE NEGATIVE RESULT.  Silence and a refused voice are also
     * what an exhausted voice pool or a wedged engine produce, and both would
     * pass the assertion below while saying nothing about the file.  A clip
     * loaded now must still play. */
    JceSound fresh = jce_audio_load_pcm(au, s_wav + 44, FRAMES * 2u, 1u, SR, 16u);
    TEST_ASSERT_NOT_EQUAL(JCE_SOUND_INVALID, fresh);
    const float still_works =
        peak_of(au, jce_audio_play(au, fresh, true, 1.0f, 1.0f));

    jce_audio_destroy(au);

    TEST_ASSERT_TRUE_MESSAGE(still_works > 0.01f,
        "the engine renders silence for a freshly loaded clip too, so the "
        "result below is about this engine being wedged, not about the file");
    TEST_ASSERT_TRUE_MESSAGE(v == JCE_VOICE_INVALID || peak <= 0.01f,
        "a host-path streaming clip still played after its file was deleted, "
        "so the encoded bytes were resident all along -- that is "
        "compressed-in-memory, not streaming from disk");
}

/* Optional real-file check for the editor waveform's decoded-PCM input. */
static void test_external_audio_waveform_source(void)
{
    const char *path = getenv("JCE_TEST_AUDIO_FILE");
    uint64_t bytes = 0u;
    void *data = jce_fs_host_read_all(path, &bytes);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_TRUE(bytes > 0u && bytes <= UINT32_MAX);
    JceAudio *audio = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(audio);
    JceSound sound = jce_audio_load_memory(audio, data, (uint32_t)bytes, path);
    TEST_ASSERT_NOT_EQUAL(JCE_SOUND_INVALID, sound);
    uint32_t frames = 0u, channels = 0u;
    const int16_t *pcm = jce_audio_get_pcm_data(audio, sound,
                                                &frames, &channels);
    TEST_ASSERT_NOT_NULL(pcm);
    TEST_ASSERT_TRUE(frames > 0u && channels > 0u);
    TEST_ASSERT_TRUE(jce_audio_get_duration(audio, sound) > 0.0f);
    {
        const uint64_t count = (uint64_t)frames * channels;
        const uint64_t step = count / 131072u + 1u;
        bool audible = false;
        for (uint64_t i = 0u; i < count; i += step) {
            if (pcm[i] != 0) { audible = true; break; }
        }
        TEST_ASSERT_TRUE_MESSAGE(audible, "decoded waveform source is silent");
    }
    printf("audio fixture: %u frames, %u channels, %.2fs\n", frames,
           channels, jce_audio_get_duration(audio, sound));
    jce_audio_destroy(audio);
    jce_fs_buffer_free(data);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_streaming_clip_holds_no_decoded_pcm);
    RUN_TEST(test_a_streaming_clip_renders_audio);
    RUN_TEST(test_two_voices_on_one_streaming_clip_both_play);
    RUN_TEST(test_an_undecodable_payload_is_refused_at_load);
    RUN_TEST(test_a_stored_pak_clip_is_borrowed_not_copied);
    RUN_TEST(test_a_compressed_pak_clip_is_copied);
    RUN_TEST(test_a_host_clip_holds_nothing_resident);
    if (getenv("JCE_TEST_AUDIO_FILE"))
        RUN_TEST(test_external_audio_waveform_source);
    remove_shared_fixture();
    return UNITY_END();
}
