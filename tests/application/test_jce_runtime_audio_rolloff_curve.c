/*
 * test_jce_runtime_audio_rolloff_curve.c
 *
 * An AudioSource must be able to take an AUTHORED volume-over-distance curve
 * instead of one of the four analytic attenuation models.
 *
 * Two ledger rows meet here.  audio.attenuation.custom-rolloff-curve said the
 * models are a closed enum (NONE/INVERSE/LINEAR/EXPONENTIAL) with a scalar
 * rolloff, and that there was "no curve type to author and nothing that could
 * evaluate one".  tooling.curve-editor.no-runtime-reader said the curve type
 * DOES exist and is script-reachable, but that "no built-in system samples a
 * curve -- particles, paths and post-process all still take scalars".  This is
 * the first built-in consumer.
 *
 * WHAT MAKES THIS A CURVE TEST RATHER THAN AN ATTENUATION TEST.  "It gets
 * quieter with distance" is satisfied by the analytic models that were already
 * there, so that cannot be the assertion.  Two cases carry this file, and
 * MUTATION SHOWED THEY CARRY DIFFERENT HALVES -- an earlier draft of this
 * comment called the flat one "the point of the file", and that was wrong:
 *
 *   flat curve, level must NOT fall with distance
 *       killed by leaving the analytic model ON alongside the curve
 *       (0.47 -> 0.02): proves the curve REPLACES the model rather than
 *       stacking with it.  NOT killed by stubbing the gain to 1.0.
 *   falling curve, silent where it is authored 0.0
 *       killed by stubbing the curve gain to a constant 1.0: proves the
 *       curve's VALUE reaches the mix.  NOT killed by the double-attenuation
 *       mutation, which only makes a quiet thing quieter.
 *
 * So neither is redundant and neither alone is sufficient.  Both run against a
 * control with the identical geometry and no curve, which must fall a long
 * way -- without it, "no change with distance" is also what a spatializer
 * that was never switched on looks like.
 *
 * THE LISTENER IS THE ORIGIN ON PURPOSE.  rt_update_audio_3d leaves the
 * listener at (0,0,0) when the scene has no camera, so the distance under test
 * is just the source's own position and nothing has to aim a camera to make
 * the measurement mean something.
 *
 * NO DEVICE IS NEEDED and no case may skip: jce_audio_create_offline() is
 * handed to the runtime through JceRuntimeDesc.audio, so the real graph and
 * the real spatializer exist with no sound hardware.
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

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define SR      48000u
#define FRAMES  4800u
#define RENDER  512u

#define CLIP_PAK_PATH "Audio/rolloff.wav"
#define CURVE_FLAT    "test_rolloff_flat.curve.json"
#define CURVE_DROP    "test_rolloff_drop.curve.json"

/* FLAT AT 1.0 out to 1000 m.  No analytic model can do this: every one of
 * them falls off past min_distance.  Channel is named "volume", which is what
 * the runtime looks for before falling back to channel 0. */
static const char *const DOC_FLAT =
    "{\"tMin\":0,\"tMax\":1000,\"vMin\":0,\"vMax\":1,\"active\":0,"
    " \"channels\":[{\"name\":\"volume\",\"visible\":true,\"color\":[1,0,0],"
    "  \"keys\":[{\"t\":0.0,\"v\":1.0,\"tanIn\":0,\"tanOut\":0,\"interp\":0},"
    "           {\"t\":1000.0,\"v\":1.0,\"tanIn\":0,\"tanOut\":0,\"interp\":0}]}]}";

/* LINEAR 1.0 -> 0.0 across 0..20 m, so 20 m is authored silence. */
static const char *const DOC_DROP =
    "{\"tMin\":0,\"tMax\":20,\"vMin\":0,\"vMax\":1,\"active\":0,"
    " \"channels\":[{\"name\":\"volume\",\"visible\":true,\"color\":[1,0,0],"
    "  \"keys\":[{\"t\":0.0,\"v\":1.0,\"tanIn\":0,\"tanOut\":0,\"interp\":0},"
    "           {\"t\":20.0,\"v\":0.0,\"tanIn\":0,\"tanOut\":0,\"interp\":0}]}]}";

/* ── fixtures ───────────────────────────────────────────────────────── */

static uint8_t s_wav[44 + FRAMES * 2];

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static void put_u16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }

/* A loud steady square wave: the measurement is a peak level, so a clip that
 * is quiet in places would make the reading depend on WHEN the render window
 * happened to land. */
static void build_wav(void)
{
    const uint32_t data_bytes = FRAMES * 2u;
    memcpy(s_wav, "RIFF", 4);
    put_u32(s_wav + 4, 36u + data_bytes);
    memcpy(s_wav + 8, "WAVEfmt ", 8);
    put_u32(s_wav + 16, 16u);
    put_u16(s_wav + 20, 1u);
    put_u16(s_wav + 22, 1u);
    put_u32(s_wav + 24, SR);
    put_u32(s_wav + 28, SR * 2u);
    put_u16(s_wav + 32, 2u);
    put_u16(s_wav + 34, 16u);
    memcpy(s_wav + 36, "data", 4);
    put_u32(s_wav + 40, data_bytes);
    for (uint32_t i = 0; i < FRAMES; ++i)
        put_u16(s_wav + 44 + i * 2u,
                (uint16_t)(int16_t)((i % 32u) < 16u ? 24000 : -24000));
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), f));
    fclose(f);
}

static void *build_clip_pak(size_t *out_size)
{
    build_wav();
    JceArchiveWriter *w = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_TRUE(jce_archive_writer_add(w, CLIP_PAK_PATH,
                                            s_wav, sizeof s_wav));
    void *buf = NULL; size_t sz = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(w, &buf, &sz));
    jce_archive_writer_destroy(w);
    *out_size = sz;
    return buf;
}

/* ── the measurement ────────────────────────────────────────────────── */

/* Build a one-source scene, place the source at `dist` metres down +Z from
 * the listener at the origin, run the runtime, and report the peak the mix
 * puts out.  `curve` NULL or "" leaves the analytic model in charge. */
static float level_at(JcePakArchive *pak, const char *curve, float dist)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity e = jce_scene_create_entity(s, "src");
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.position.z = dist;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);

    JceAudioSourceComponent as;
    memset(&as, 0, sizeof as);
    snprintf(as.clip_path, sizeof as.clip_path, "%s", CLIP_PAK_PATH);
    as.volume        = 1.0f;
    as.pitch         = 1.0f;
    as.spatial_blend = 1.0f;     /* spatial, or no distance law applies */
    as.loop          = true;
    as.play_on_awake = true;
    /* STREAMING because it is SYNCHRONOUS by construction: the
     * decompress-on-load path hands the clip to a worker and the voice only
     * exists once that decode retires, so a fixed number of steps would make
     * this file's result depend on how fast the machine is that ran it.
     * rt_audio_source_start says so in its own comment.  The load type is
     * orthogonal to the distance law under test -- and the no-curve control
     * uses the same one, so if it were not, the control would say so. */
    as.load_type     = 1;
    if (curve && curve[0])
        snprintf(as.rolloff_curve, sizeof as.rolloff_curve, "%s", curve);
    jce_scene_set_audio_source(s, e, &as);

    JceAudio *audio = jce_audio_create_offline();
    TEST_ASSERT_NOT_NULL(audio);

    JceRuntimeDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.scene = s;
    rd.pak   = pak;
    rd.audio = audio;

    JceRuntime *rt = jce_runtime_create(&rd);
    TEST_ASSERT_NOT_NULL(rt);

    /* Step first: the gain is applied by rt_update_audio_3d, not at spawn. */
    float out[RENDER * 2], peak = 0.0f;
    for (int n = 0; n < 8; ++n) {
        jce_runtime_step(rt, 1.0f / 60.0f);
        memset(out, 0, sizeof out);
        TEST_ASSERT_TRUE(jce_audio_render_offline(audio, out, RENDER));
        for (uint32_t i = 0; i < RENDER * 2u; ++i) {
            const float m = fabsf(out[i]);
            if (m > peak) peak = m;
        }
    }

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
    jce_audio_destroy(audio);
    return peak;
}

/* ── cases ──────────────────────────────────────────────────────────── */

/* THE CONTROL, and it runs first because every other case is read against it.
 * With no curve the analytic model attenuates, so distance must cost level.
 * If it did not, "the curve held the level up" would be unfalsifiable. */
static void test_without_a_curve_distance_still_attenuates(void)
{
    size_t sz = 0;
    void *blob = build_clip_pak(&sz);
    JcePakArchive *pak = jce_pak_open(blob, sz);
    TEST_ASSERT_NOT_NULL(pak);

    const float near_db = level_at(pak, NULL, 1.0f);
    const float far_db  = level_at(pak, NULL, 20.0f);

    jce_pak_close(pak);
    jce_free(blob);

    TEST_ASSERT_TRUE_MESSAGE(near_db > 0.01f,
        "the source is silent at 1 m with no curve, so this file measures "
        "nothing at all -- the clip, the pak or the spatializer is broken");
    TEST_ASSERT_TRUE_MESSAGE(far_db < near_db * 0.7f,
        "distance did not attenuate an ordinary source, so the flat-curve "
        "case below cannot claim the curve is what held the level up");
}

/* THE CURVE REPLACES THE MODEL.  A curve flat at 1.0 defeats distance --
 * which no analytic model can do -- so the level at 20 m matches the level at
 * 1 m.  This fails if the curve is ignored (the control above shows what that
 * looks like) AND if the curve is applied ON TOP of the analytic model
 * instead of replacing it, which would attenuate twice. */
static void test_a_flat_curve_defeats_distance(void)
{
    write_file(CURVE_FLAT, DOC_FLAT);
    size_t sz = 0;
    void *blob = build_clip_pak(&sz);
    JcePakArchive *pak = jce_pak_open(blob, sz);
    TEST_ASSERT_NOT_NULL(pak);

    const float near_db = level_at(pak, CURVE_FLAT, 1.0f);
    const float far_db  = level_at(pak, CURVE_FLAT, 20.0f);

    jce_pak_close(pak);
    jce_free(blob);
    remove(CURVE_FLAT);

    TEST_ASSERT_TRUE_MESSAGE(near_db > 0.01f,
        "a source with a curve authored at 1.0 is silent at 1 m");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(near_db * 0.15f, near_db, far_db,
        "a curve authored FLAT at 1.0 still lost level with distance -- "
        "either the curve is not being read and the analytic model is still "
        "deciding, or it is being applied on top of that model instead of "
        "replacing it, so the source is attenuated twice");
}

/* And the curve's SHAPE is what is read, not merely its presence: a curve
 * authored to reach 0.0 at 20 m must actually silence the source there, while
 * the same source is loud at 1 m. */
static void test_a_falling_curve_silences_the_source_where_it_reaches_zero(void)
{
    write_file(CURVE_DROP, DOC_DROP);
    size_t sz = 0;
    void *blob = build_clip_pak(&sz);
    JcePakArchive *pak = jce_pak_open(blob, sz);
    TEST_ASSERT_NOT_NULL(pak);

    const float near_db = level_at(pak, CURVE_DROP, 1.0f);
    const float far_db  = level_at(pak, CURVE_DROP, 20.0f);

    jce_pak_close(pak);
    jce_free(blob);
    remove(CURVE_DROP);

    TEST_ASSERT_TRUE_MESSAGE(near_db > 0.01f,
        "the falling curve is already silent at 1 m, where it is authored at "
        "1.0 -- the curve is being sampled at the wrong distance, or not at "
        "all");
    TEST_ASSERT_TRUE_MESSAGE(far_db < 0.01f,
        "the curve reaches 0.0 at 20 m and the source is still audible there, "
        "so the curve's VALUE is not reaching the mix even though its presence "
        "changed something");
}

/* A NAMED CURVE THAT WILL NOT LOAD must not silence the source.  Returning 0
 * for a missing document would make "the author drew silence" and "the file is
 * gone" the same sound, and the second one is a bug the first one hides. */
static void test_a_missing_curve_falls_back_instead_of_silencing(void)
{
    size_t sz = 0;
    void *blob = build_clip_pak(&sz);
    JcePakArchive *pak = jce_pak_open(blob, sz);
    TEST_ASSERT_NOT_NULL(pak);

    const float lvl = level_at(pak, "no_such_curve_anywhere.json", 1.0f);

    jce_pak_close(pak);
    jce_free(blob);

    TEST_ASSERT_TRUE_MESSAGE(lvl > 0.01f,
        "naming a curve that does not exist silenced the source -- a missing "
        "document now sounds exactly like a curve authored at zero");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_without_a_curve_distance_still_attenuates);
    RUN_TEST(test_a_flat_curve_defeats_distance);
    RUN_TEST(test_a_falling_curve_silences_the_source_where_it_reaches_zero);
    RUN_TEST(test_a_missing_curve_falls_back_instead_of_silencing);
    return UNITY_END();
}
