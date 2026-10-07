#include "jce_video_frame.h"
#include <jce/os/core/jce_allocator.h>
#include <limits.h>
#include "jce_video_perf.h"
#include "jce_hevc_diagnostics.h"
#include "jce_h265_perf.h"
/* The patented-codec contract, checked at RUNTIME rather than in CMake.
 *
 * Nothing has ever tested MP4 / H.264 / H.265 / AAC decoding, which is how a
 * report of "the release build lost its patented codecs" could stand for weeks
 * without anyone being able to confirm or deny it from a test run.
 *
 * The specific thing asserted here is `JceVideoInfo.metadata_only`: the video
 * module sets it when the container parsed but no decode backend opened. The
 * editor's video pane keys its whole message off that flag, and used to report
 * every metadata_only H.264/H.265 clip as "patent-encumbered and disabled in
 * this build" — including in builds that HAD fdk-aac / OpenH264 / libhevc
 * linked in. So metadata_only for an avc1 clip in a JCE_ENABLE_PATENTED_CODECS
 * build is exactly the state that gets misread as a missing codec, and it is
 * the state worth failing on.
 *
 * Fixture: a committed synthetic H.264 (avc1) MP4. Discovery uses test
 * fixtures rather than consumer-project media. Point JCE_TEST_MP4 at another
 * file for an explicit container/codec regression.
 */

#include "unity.h"

#include <jce/middleware/video/jce_video.h>
#include <jce/api_audio.h>
#include <jce/middleware/video/jce_mp4_parser.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_thread.h>
#include "jce_yuv_convert.h"
#include "jce_video_clock.h"
#include "jce_video_seek_trace.h"
#include "jce_video_timestamp.h"
#include "jce_video_late_policy.h"
#include <math.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Candidate fixtures, relative to the repo root. */
static const char *k_candidates[] = {
    "tests/middleware/video/fixtures/bframes.mp4",
    "tests/middleware/video/fixtures/bframes-fragmented.mp4",
};

static void *s_blob;
static uint64_t s_blob_size;
static char s_blob_path[512];

/* Load `path`. When `require_avc` it is kept only if it advertises an avc1
 * (H.264) track — that filter is for auto-discovery, so a stray royalty-free
 * clip does not get picked as the patented-codec fixture. A path named
 * explicitly via JCE_TEST_MP4 is taken as-is. */
static bool try_fixture(const char *path, bool require_avc)
{
    uint64_t sz = 0;
    void *raw = jce_fs_host_read_all(path, &sz);
    if (!raw) return false;
    if (require_avc) {
        /* The sample-description fourcc lives in moov; scan the head of the
         * file so we do not depend on a parser we are about to test. */
        const char *p = (const char *)raw;
        uint64_t scan = sz < (1u << 20) ? sz : (1u << 20);
        bool avc = false;
        for (uint64_t i = 0; i + 4 <= scan; ++i) {
            if (memcmp(p + i, "avc1", 4) == 0) { avc = true; break; }
        }
        if (!avc) { jce_fs_buffer_free(raw); return false; }
    }
    s_blob = raw;
    s_blob_size = sz;
    snprintf(s_blob_path, sizeof(s_blob_path), "%s", path);
    return true;
}

static bool find_fixture(void)
{
    const char *env = getenv("JCE_TEST_MP4");
    if (env && env[0] && try_fixture(env, false)) return true;

    /* CTest can run six or more levels below the source tree. Prefer the
     * configured test fixture authority instead of guessing that depth. */
    char fixture_path[1024];
    snprintf(fixture_path, sizeof(fixture_path), "%s/bframes.mp4", JCE_TEST_COLOR_DIR);
    if (try_fixture(fixture_path, true)) return true;

    /* ctest runs from the build tree; walk up looking for the repo root. */
    static const char *prefixes[] = { "", "../", "../../", "../../../",
                                      "../../../../", "../../../../../" };
    char buf[640];
    for (size_t d = 0; d < sizeof(prefixes) / sizeof(prefixes[0]); ++d) {
        for (size_t c = 0; c < sizeof(k_candidates) / sizeof(k_candidates[0]);
             ++c) {
            snprintf(buf, sizeof(buf), "%s%s", prefixes[d], k_candidates[c]);
            if (try_fixture(buf, true)) return true;
        }
    }
    return false;
}

static void test_h264_mp4_opens_a_real_decoder(void)
{
    if (!s_blob) {
        TEST_IGNORE_MESSAGE(
            "no H.264 MP4 fixture found (sample media is gitignored) — set "
            "JCE_TEST_MP4 to run this");
        return;
    }

    JceVideo v = jce_video_load_memory(s_blob, (uint32_t)s_blob_size,
                                       s_blob_path);
    TEST_ASSERT_TRUE_MESSAGE(v != JCE_VIDEO_INVALID,
                             "jce_video_load_memory failed on an H.264 MP4");

    JceVideoInfo info;
    memset(&info, 0, sizeof(info));
    TEST_ASSERT_TRUE(jce_video_get_info(v, &info));

    TEST_ASSERT_TRUE_MESSAGE(strcmp(info.video_codec, "avc1") == 0
                                 || strcmp(info.video_codec, "hvc1") == 0
                                 || strcmp(info.video_codec, "hev1") == 0,
                             "fixture is not a patented-codec clip");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, info.width, "zero width");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, info.height, "zero height");

#if defined(JCE_ENABLE_PATENTED_CODECS) && JCE_ENABLE_PATENTED_CODECS
    /* This build links OpenH264, so a real decoder must have opened.
     * metadata_only here is precisely the state the editor used to mislabel
     * "patent-encumbered and disabled in this build". */
    TEST_ASSERT_FALSE_MESSAGE(
        info.metadata_only,
        "H.264 clip parsed but NO decoder opened, in a build that has the "
        "patented codecs compiled in — this is the state the editor reports "
        "as a missing codec");
#else
    /* Royalty-free build: parsing must still succeed and must degrade to
     * metadata-only rather than failing the load outright. */
    TEST_ASSERT_TRUE_MESSAGE(
        info.metadata_only,
        "royalty-free build decoded H.264 — the patent gate is not holding");
#endif

    if (!info.metadata_only) {
        const uint64_t preview_start = jce_time_ticks_ms();
        while (jce_video_get_frame_counter(v) == 0u
               && jce_time_ticks_ms() - preview_start < 10000u) {
            jce_video_advance(v, 0.0);
            jce_thread_sleep_ms(16);
        }
        TEST_ASSERT_NOT_NULL_MESSAGE(jce_video_get_frame_rgba(v, NULL, NULL, NULL),
                                     "paused restore never published its first frame");
        TEST_ASSERT_DOUBLE_WITHIN(0.000000001, 0.0, jce_video_get_time(v));
        const uint64_t paused_counter = jce_video_get_frame_counter(v);
        for (uint32_t i = 0; i < 8u; ++i) {
            jce_video_advance(v, 0.0);
            jce_thread_sleep_ms(16);
        }
        TEST_ASSERT_EQUAL_UINT64(paused_counter, jce_video_get_frame_counter(v));
        TEST_ASSERT_DOUBLE_WITHIN(0.000000001, 0.0, jce_video_get_time(v));
    }

    if (getenv("JCE_TEST_MP4_SEEK")) {
        JceMp4Parser *parser = jce_mp4_parser_open_memory(
            s_blob, (size_t)s_blob_size, NULL);
        JceMp4VideoTrackInfo track;
        JceMp4SampleInfo middle, last;
        TEST_ASSERT_NOT_NULL(parser);
        TEST_ASSERT_TRUE(jce_mp4_parser_get_video_track_info(parser, &track));
        TEST_ASSERT_TRUE(jce_mp4_parser_get_video_sample(
            parser, track.sample_count / 2u, &middle));
        TEST_ASSERT_TRUE(jce_mp4_parser_get_video_sample(
            parser, track.sample_count - 1u, &last));
        TEST_ASSERT_GREATER_THAN_UINT64(middle.timestamp, last.timestamp);
        if (info.duration * (double)track.timescale > 4294967296.0) {
            TEST_ASSERT_GREATER_THAN_UINT64(4294967295ULL, last.timestamp);
        }
        jce_mp4_parser_close(parser);
        double target = info.duration * 0.5;
        uint64_t start = jce_time_ticks_ms();
        jce_video_seek(v, target, true);
        printf("exact seek %.2fs: %llu ms\n", target,
               (unsigned long long)(jce_time_ticks_ms() - start));
    }
    jce_video_unload(v);
}

/* A fragmented MP4 (CMAF / DASH) keeps an EMPTY stbl in moov and puts the
 * sample records in a moof before every mdat. minimp4 only reads stbl, so
 * before the fragment index existed every such file reported sample_count 0,
 * the first sample read failed, and jce_video closed the decoder it had just
 * successfully opened and fell back to metadata-only. */
static void test_fragmented_mp4_has_a_sample_index(void)
{
    JceMp4Info mi;

    if (!s_blob) {
        TEST_IGNORE_MESSAGE("no MP4 fixture found");
        return;
    }
    memset(&mi, 0, sizeof(mi));
    TEST_ASSERT_TRUE(jce_mp4_parse_memory(s_blob, (size_t)s_blob_size, &mi));

    if (!mi.fragmented) {
        TEST_IGNORE_MESSAGE("fixture is a progressive MP4 — set JCE_TEST_MP4 "
                            "to a fragmented one to exercise this");
        return;
    }
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(
        0u, mi.sample_count,
        "fragmented MP4 yielded an empty sample index — moof/traf/trun was "
        "not parsed, and every decode will fail at sample 0");
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(
        0u, mi.keyframe_count, "no sync sample found in the fragment index");
    TEST_ASSERT_TRUE_MESSAGE(mi.framerate > 0.0,
                             "fragmented MP4 reported a zero frame rate");
}

static void test_load_rejects_non_video_bytes(void)
{
    static const char junk[] = "not an mp4, not even close, just some bytes";
    JceVideo v = jce_video_load_memory(junk, (uint32_t)sizeof(junk), "junk.mp4");
    TEST_ASSERT_TRUE_MESSAGE(v == JCE_VIDEO_INVALID,
                             "garbage bytes produced a valid video handle");
}

static void test_preview_downsample_keeps_centres_and_ignores_row_padding(void)
{
    const uint8_t plane[] = {0, 20, 99, 40, 60, 99};
    uint8_t pixel = 0;
    jce_yuv_plane_downsample(plane, 2, 2, 3, &pixel, 1, 1);
    TEST_ASSERT_EQUAL_UINT8(30, pixel);
    const uint8_t odd_plane[] = {0, 0, 0, 99, 0, 73, 0, 99, 0, 0, 0, 99};
    jce_yuv_plane_downsample(odd_plane, 3, 3, 4, &pixel, 1, 1);
    TEST_ASSERT_EQUAL_UINT8(73, pixel);
    uint8_t ramp[] = {0, 30, 60, 90, 120, 99};
    uint8_t reduced[3] = {0};
    jce_yuv_plane_downsample(ramp, 5, 1, 6, reduced, 3, 1);
    TEST_ASSERT_UINT8_WITHIN(1, 10, reduced[0]);
    TEST_ASSERT_UINT8_WITHIN(1, 60, reduced[1]);
    TEST_ASSERT_UINT8_WITHIN(1, 110, reduced[2]);
    uint8_t constant[15];
    memset(constant, 180, sizeof(constant));
    jce_yuv_plane_downsample(constant, 5, 3, 5, reduced, 3, 1);
    for (uint32_t i = 0; i < 3u; ++i) TEST_ASSERT_EQUAL_UINT8(180, reduced[i]);
    TEST_ASSERT_FALSE(jce_video_set_preview_max_dimension(JCE_VIDEO_INVALID, 1280));
}

static uint8_t reference_channel(int value)
{
    /* Floor division is defined here even for negative intermediates. */
    int rounded = value >= 0 ? value / 256 : -((-value + 255) / 256);
    return (uint8_t)(rounded < 0 ? 0 : rounded > 255 ? 255 : rounded);
}

static uint8_t reference_color_channel(double value)
{
    int rounded = (int)floor(value + 0.5);
    return (uint8_t)(rounded < 0 ? 0 : rounded > 255 ? 255 : rounded);
}

static void check_yuv_color_matrices(const uint8_t *y, const uint8_t *u, const uint8_t *v)
{
    /* ITU-T H.273 Kr/Kb, independent floating-point inverse equations. */
    static const double kr[] = {0.299, 0.2126, 0.2627};
    static const double kb[] = {0.114, 0.0722, 0.0593};
    uint8_t rgba[65 * 3 * 4 + 16];
    for (int matrix = 0; matrix < 3; ++matrix) {
        for (int full = 0; full < 2; ++full) {
            JceYuvColor color = {(JceYuvMatrix)matrix, full != 0};
            for (uint32_t width = 1; width <= 65; ++width) {
                memset(rgba, 0x5a, sizeof(rgba));
                jce_yuv420_to_rgba_color(y, 72, u, 40, v, 40, rgba, width, 3, color);
                for (uint32_t row = 0; row < 3; ++row) {
                    for (uint32_t col = 0; col < width; ++col) {
                        double yy = ((int)y[row * 72 + col] - (full ? 0 : 16))
                            * (full ? 1.0 : 255.0 / 219.0);
                        double uu = ((int)u[(row / 2) * 40 + col / 2] - 128)
                            * (full ? 1.0 : 255.0 / 224.0);
                        double vv = ((int)v[(row / 2) * 40 + col / 2] - 128)
                            * (full ? 1.0 : 255.0 / 224.0);
                        double r = yy + 2.0 * (1.0 - kr[matrix]) * vv;
                        double b = yy + 2.0 * (1.0 - kb[matrix]) * uu;
                        double g = (yy - kr[matrix] * r - kb[matrix] * b)
                            / (1.0 - kr[matrix] - kb[matrix]);
                        const uint8_t *out = rgba + (row * width + col) * 4;
                        TEST_ASSERT_UINT8_WITHIN(1, reference_color_channel(r), out[0]);
                        TEST_ASSERT_UINT8_WITHIN(1, reference_color_channel(g), out[1]);
                        TEST_ASSERT_UINT8_WITHIN(1, reference_color_channel(b), out[2]);
                        TEST_ASSERT_EQUAL_UINT8(255, out[3]);
                    }
                }
                for (size_t i = width * 3u * 4u; i < sizeof(rgba); ++i)
                    TEST_ASSERT_EQUAL_UINT8(0x5a, rgba[i]);
            }
        }
    }
    TEST_ASSERT_EQUAL_INT(JCE_YUV_BT709, jce_yuv_color_from_cicp(1, false).matrix);
    TEST_ASSERT_EQUAL_INT(JCE_YUV_BT2020_NCL, jce_yuv_color_from_cicp(9, false).matrix);
    TEST_ASSERT_EQUAL_INT(JCE_YUV_BT601, jce_yuv_color_from_cicp(2, false).matrix);
    TEST_ASSERT_TRUE(jce_yuv_color_from_cicp(1, true).full_range);
}

static void test_yuv_dispatch_matches_scalar_color_and_tails(void)
{
    uint8_t y[72 * 3], u[40 * 2], v[40 * 2];
    uint8_t actual[65 * 3 * 4 + 16], expected[sizeof(actual)];
    uint32_t seed = 43u;
    for (size_t i = 0; i < sizeof(y); ++i) {
        seed = seed * 1664525u + 1013904223u;
        y[i] = (uint8_t)(seed >> 24);
    }
    for (size_t i = 0; i < sizeof(u); ++i) {
        seed = seed * 1664525u + 1013904223u;
        u[i] = (uint8_t)(seed >> 24);
        seed = seed * 1664525u + 1013904223u;
        v[i] = (uint8_t)(seed >> 24);
    }
    /* First exercise the automatically selected, hardware-safe path. Then
     * disable AVX2 to exercise SSE2/NEON/scalar without requiring AVX2. */
    for (int path = 0; path < 2; ++path) {
        if (path) jce_yuv_set_avx2(0);
        check_yuv_color_matrices(y, u, v);
        for (uint32_t width = 1; width <= 65; ++width) {
            memset(expected, 0x5a, sizeof(expected));
            memset(actual, 0x5a, sizeof(actual));
            for (uint32_t row = 0; row < 3; ++row) {
                for (uint32_t col = 0; col < width; ++col) {
                    int yy = (int)y[row * 72 + col] - 16;
                    int uu = (int)u[(row / 2) * 40 + col / 2] - 128;
                    int vv = (int)v[(row / 2) * 40 + col / 2] - 128;
                    uint8_t *out = expected + (row * width + col) * 4;
                    out[0] = reference_channel(298 * yy + 409 * vv + 128);
                    out[1] = reference_channel(298 * yy - 100 * uu - 208 * vv + 128);
                    out[2] = reference_channel(298 * yy + 516 * uu + 128);
                    out[3] = 255;
                }
            }
            jce_yuv420_to_rgba(y, 72, u, 40, v, 40, actual, width, 3);
            TEST_ASSERT_EQUAL_MEMORY(expected, actual, sizeof(actual));
        }
    }
}

static bool compare_odd_fixture(const char *stem, const char *extension, int *max_error)
{
    char encoded_path[1024], reference_path[1024];
    uint64_t size = 0, reference_size = 0;
    JceVideo video = JCE_VIDEO_INVALID;
    void *encoded = NULL;
    uint8_t *reference = NULL;
    bool matched = false;
    snprintf(encoded_path, sizeof(encoded_path), "%s/%s.%s", JCE_TEST_COLOR_DIR, stem, extension);
    encoded = jce_fs_host_read_all(encoded_path, &size);
    snprintf(reference_path, sizeof(reference_path), "%s/%s.rgba", JCE_TEST_COLOR_DIR, stem);
    reference = jce_fs_host_read_all(reference_path, &reference_size);
    if (!encoded || !reference || size > UINT32_MAX || reference_size != 67u * 51u * 4u)
        goto done;
    video = jce_video_load_memory(encoded, (uint32_t)size, encoded_path);
    if (video == JCE_VIDEO_INVALID) goto done;
    jce_video_seek(video, 1.0, true);
    const uint64_t start = jce_time_ticks_ms();
    int width = 0, height = 0;
    double pts = -1.0;
    const uint8_t *pixels = NULL;
    while (jce_time_ticks_ms() - start < 10000u) {
        jce_video_advance(video, 0.0);
        pixels = jce_video_get_frame_rgba(video, &width, &height, &pts);
        if (pixels && jce_video_is_ready_to_play(video) && fabs(pts - 1.0) < 0.00001)
            break;
        jce_thread_sleep_ms(5);
    }
    if (!pixels || width != 67 || height != 51 || fabs(pts - 1.0) > 0.00001)
        goto done;
    *max_error = 0;
    for (size_t i = 0; i < (size_t)reference_size; ++i) {
        int error = abs((int)pixels[i] - (int)reference[i]);
        if (error > *max_error) *max_error = error;
    }
    matched = *max_error <= 1;
done:
    if (video != JCE_VIDEO_INVALID) jce_video_unload(video);
    if (encoded) jce_fs_buffer_free(encoded);
    if (reference) jce_fs_buffer_free(reference);
    return matched;
}

static void test_odd_video_chroma_queue_matches_reference(void)
{
    int error = -1;
    TEST_ASSERT_TRUE_MESSAGE(compare_odd_fixture("odd-av1", "mp4", &error),
                             "67x51 AV1 queue lost color or crop-edge chroma");
    TEST_ASSERT_TRUE_MESSAGE(compare_odd_fixture("odd-vp9", "webm", &error),
                             "67x51 VP9 queue lost color or crop-edge chroma");
}

/* A 60 Hz consumer of a 10 ms device cursor must not inherit its jitter. */
static void test_late_presentation_preserves_generation_seek_and_final_picture(void)
{
    TEST_ASSERT_FALSE(jce_video_presentation_skip(NULL, 7, 1.0, .02, 10.0, false));
    JceVideoPresentationTarget t = {5.0, 7, 0, true, 0};
    for (unsigned i = 0; i < 30; ++i) {
        TEST_ASSERT_EQUAL(i % 3 != 2,
            jce_video_presentation_skip(&t, 7, 1.0, .02, 10.0, false));
    }
    TEST_ASSERT_EQUAL_UINT64(20, t.skipped);
    TEST_ASSERT_FALSE(jce_video_presentation_skip(&t, 8, 1.0, .02, 10.0, false));
    TEST_ASSERT_EQUAL_UINT32(0, t.consecutive);
    TEST_ASSERT_TRUE(jce_video_presentation_obsolete(&t, 7, 1.0, .02, 10.0, false));
    TEST_ASSERT_FALSE(jce_video_presentation_obsolete(&t, 8, 1.0, .02, 10.0, false));
    TEST_ASSERT_FALSE(jce_video_presentation_obsolete(&t, 7, 1.0, .02, 10.0, true));
    t.active = false;
    TEST_ASSERT_FALSE(jce_video_presentation_obsolete(&t, 7, 1.0, .02, 10.0, false));
    t.active = true;
    TEST_ASSERT_FALSE(jce_video_presentation_obsolete(&t, 7, 4.99, .02, 10.0, false));
    TEST_ASSERT_FALSE(jce_video_presentation_obsolete(&t, 7, 1.0, .02, 0.0, false));
    t.seconds = 20.0;
    TEST_ASSERT_FALSE(jce_video_presentation_obsolete(&t, 7, 9.98, .02, 10.0, false));
    TEST_ASSERT_FALSE(jce_video_presentation_obsolete(&t, 7, NAN, .02, 10.0, false));
    t.seconds = NAN;
    TEST_ASSERT_FALSE(jce_video_presentation_obsolete(&t, 7, 1.0, .02, 10.0, false));
}

static void test_timestamp_normalization_preserves_seek_and_repeated_pts(void)
{
    bool base_set = false;
    double base = 0.0;
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0,
        jce_video_timestamp_normalize(0, 0.0, 60.0, false,
            &base_set, &base, 100.0, true));
    TEST_ASSERT_TRUE(base_set);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 100.0, base);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.0 / 60.0,
        jce_video_timestamp_normalize(1, 0.0, 60.0, false,
            &base_set, &base, 100.0, true));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.25,
        jce_video_timestamp_normalize(8, 1.0, 60.0, true,
            &base_set, &base, 100.25, true));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.0 + 1.0 / 30.0,
        jce_video_timestamp_normalize(8, 1.0, 0.0, false,
            &base_set, &base, -1.0, false));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0,
        jce_video_timestamp_normalize(0, 0.0, 30.0, false,
            &base_set, &base, -1.0, false));
    TEST_ASSERT_FALSE(base_set);
}

static void test_seek_trace_is_opt_in_and_generation_bound(void)
{
    JceVideoSeekTrace trace = {0};
    jce_video_seek_trace_begin(&trace, 7, 3.0, 100);
    jce_video_seek_trace_picture(&trace, 2.0);
    TEST_ASSERT_FALSE(trace.active);
    TEST_ASSERT_EQUAL_UINT64(0, trace.pictures);
    trace.enabled = true;
    jce_video_seek_trace_begin(&trace, 7, 3.0, 100);
    jce_video_seek_trace_picture(&trace, 2.0);
    jce_video_seek_trace_picture(&trace, 3.0);
    TEST_ASSERT_EQUAL_UINT64(2, trace.pictures);
    TEST_ASSERT_EQUAL_UINT64(1, trace.preroll_pictures);
    TEST_ASSERT_TRUE(jce_video_seek_trace_current(&trace, 7, false, false));
    TEST_ASSERT_FALSE(jce_video_seek_trace_current(&trace, 8, false, false));
    TEST_ASSERT_FALSE(jce_video_seek_trace_current(&trace, 7, true, false));
    TEST_ASSERT_FALSE(jce_video_seek_trace_current(&trace, 7, false, true));
    TEST_ASSERT_FALSE(jce_video_seek_trace_current(NULL, 7, false, false));
    trace.index_ms = 20;
    jce_video_seek_trace_begin(&trace, 8, 4.0, 200);
    TEST_ASSERT_EQUAL_UINT64(0, trace.pictures);
    TEST_ASSERT_EQUAL_UINT64(0, trace.index_ms);
    TEST_ASSERT_FALSE(trace.keyframe_known);
    TEST_ASSERT_EQUAL_UINT64(25, jce_video_seek_trace_elapsed(100, 125));
    TEST_ASSERT_EQUAL_UINT64(0, jce_video_seek_trace_elapsed(125, 100));
}

static void test_audio_clock_preserves_picture_cadence(void)
{
    const double dt = 1.0 / 60.0;
    double clock = 0.0;
    int previous = 0, direct_previous = 0;
    int skipped = 0, direct_skipped = 0;
    for (int i = 1; i <= 600; ++i) {
        const double audio = floor((i * dt + 0.000000001) / 0.01) * 0.01;
        const double next = jce_video_clock_follow_audio(clock, dt, audio);
        const int picture = (int)((next + 0.0005) / dt);
        const int direct_picture = (int)((audio + 0.0005) / dt);
        TEST_ASSERT_TRUE(next >= clock);
        TEST_ASSERT_TRUE(next <= audio + 0.020000001);
        if (picture > previous + 1) skipped += picture - previous - 1;
        if (direct_picture > direct_previous + 1)
            direct_skipped += direct_picture - direct_previous - 1;
        clock = next;
        previous = picture;
        direct_previous = direct_picture;
    }
    /* Negative control: reverting to the raw device clock skips 200 frames. */
    TEST_ASSERT_TRUE(direct_skipped > 150);
    TEST_ASSERT_INT_WITHIN(1, 600, previous);
    TEST_ASSERT_INT_WITHIN(1, 0, skipped);
}

static void test_audio_clock_stalls_and_recovery_are_bounded(void)
{
    const double dt = 1.0 / 60.0;
    double clock = 2.0;
    for (int i = 0; i < 120; ++i) {
        double next = jce_video_clock_follow_audio(clock, dt, 2.0);
        TEST_ASSERT_TRUE(next >= clock && next <= 2.020000001);
        clock = next;
    }
    TEST_ASSERT_EQUAL_DOUBLE(clock,
        jce_video_clock_follow_audio(clock, 0.0, 100.0));
    TEST_ASSERT_EQUAL_DOUBLE(clock,
        jce_video_clock_follow_audio(clock, -dt, 100.0));
    /* Resume with a large audio lead: catch up, without a one-tick jump. */
    for (int i = 0; i < 60; ++i) {
        double next = jce_video_clock_follow_audio(clock, dt, 3.0 + i * dt);
        TEST_ASSERT_TRUE(next >= clock && next <= clock + 2.0 * dt + 0.000000001);
        clock = next;
    }
    TEST_ASSERT_TRUE(fabs(clock - (3.0 + 59 * dt)) < 0.050);
}

static uint32_t probe_audio_pull(void *ud, int16_t *out, uint32_t frames)
{
    return jce_video_audio_pull(*(JceVideo *)ud, out, frames);
}

static bool probe_resume_after_seek(JceVideo *video, JceAudio *audio,
                                    JceVoice *voice, unsigned seconds, unsigned tick_ms)
{
    if (!audio) return false;
    const uint64_t ready_start = jce_time_ticks_ms();
    const double base = jce_video_get_time(*video);
    while (!jce_video_is_ready_to_play(*video)
           && jce_time_ticks_ms() - ready_start < 10000u) {
        jce_video_advance(*video, 0.0);
        if (fabs(jce_video_get_time(*video) - base) > 0.000001) return false;
        jce_thread_sleep_ms(16);
    }
    uint32_t channels = 0, rate = 0;
    if (!jce_video_is_ready_to_play(*video)
        || !jce_video_get_audio_format(*video, &channels, &rate, NULL)) return false;
    const char *mute = getenv("JCE_TEST_VIDEO_AUDIO_MUTE");
    *voice = jce_audio_play_stream(audio, probe_audio_pull, video,
        (uint16_t)channels, rate, mute && mute[0] == '1' ? 0.0f : 0.1f, 1.0f);
    if (*voice == JCE_VOICE_INVALID) return false;
    const uint64_t start = jce_time_ticks_ms();
    uint64_t last = start, next_log = 0;
    while (jce_time_ticks_ms() - start < (uint64_t)seconds * 1000u) {
        uint64_t now = jce_time_ticks_ms();
        jce_video_advance(*video, (double)(now - last) / 1000.0);
        if (now - start >= next_log) {
            double picture = -1.0;
            jce_video_get_frame_rgba(*video, NULL, NULL, &picture);
            printf("resume wall=%.3f media=%.3f picture=%.3f audio=%.3f frame=%llu\n",
                   (double)(now - start) / 1000.0, jce_video_get_time(*video), picture,
                   jce_video_audio_get_time(*video),
                   (unsigned long long)jce_video_get_frame_counter(*video));
            next_log += 50u;
        }
        last = now;
        uint64_t done = jce_time_ticks_ms();
        if (done - now < tick_ms)
            jce_thread_sleep_ms((uint32_t)(tick_ms - (done - now)));
    }
    printf("probe resumed seconds=%u start=%.6f end=%.6f\n",
           seconds, base, jce_video_get_time(*video));
    jce_audio_pause(audio, *voice);
    return jce_video_get_time(*video) >= base + seconds - 0.3;
}

static void test_hevc_diagnostic_options_are_exact_and_bounded(void)
{
    const char *invalid[] = { NULL, "", "0", "9", "01", "1junk", " ", "1 ", "-1" };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        TEST_ASSERT_EQUAL_UINT32(4u, jce_hevc_diagnostic_cores(invalid[i], 4u));
        TEST_ASSERT_EQUAL_INT(invalid[i] && strcmp(invalid[i], "0") == 0 ? 0 : 1,
            jce_hevc_diagnostic_keep_threads(invalid[i], 1));
        TEST_ASSERT_FALSE(jce_hevc_diagnostic_keep_threads(invalid[i], 0));
    }
    TEST_ASSERT_EQUAL_UINT32(1u, jce_hevc_diagnostic_cores("1", 4u));
    TEST_ASSERT_EQUAL_UINT32(8u, jce_hevc_diagnostic_cores("8", 4u));
    TEST_ASSERT_FALSE(jce_hevc_diagnostic_keep_threads("0", 1));
    TEST_ASSERT_TRUE(jce_hevc_diagnostic_keep_threads("1", 0));
    TEST_ASSERT_FALSE(jce_hevc_diagnostic_keep_threads("8", 0));
}

static unsigned perf_clock_calls;
static uint64_t perf_fake_clock(void)
{
    return ++perf_clock_calls * 100u;
}
static void test_private_perf_disabled_never_reads_clock(void)
{
    perf_clock_calls = 0;
    TEST_ASSERT_EQUAL_UINT64(0u, jce_video_perf_begin(false, perf_fake_clock));
    TEST_ASSERT_EQUAL_UINT64(0u, jce_video_perf_begin(false, perf_fake_clock));
    TEST_ASSERT_EQUAL_UINT(0u, perf_clock_calls);
    TEST_ASSERT_EQUAL_UINT64(100u, jce_video_perf_begin(true, perf_fake_clock));
    TEST_ASSERT_EQUAL_UINT(1u, perf_clock_calls);
    double ema = 0.0, last = 0.0;
    jce_video_perf_ema_update(&ema, &last, 20.0);
    TEST_ASSERT_EQUAL_DOUBLE(20.0, ema);
    jce_video_perf_ema_update(&ema, &last, 40.0);
    TEST_ASSERT_EQUAL_DOUBLE(22.0, ema);
    TEST_ASSERT_EQUAL_DOUBLE(40.0, last);
    uint64_t frequency = 1000u;
    TEST_ASSERT_EQUAL_DOUBLE(100000.0, jce_video_perf_us(&frequency, 100u, 200u));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, jce_video_perf_us(&frequency, 200u, 100u));
}

static void test_frame_preparation_rejects_invalid_planes_and_stride(void)
{
    uint8_t y[16] = {0}, u[4] = {0}, v[4] = {0};
    JceVideoFrameProcessor p = {0};
    p.perf_checked = true;
    JceYuv420Frame input = {y, u, v, 4, 2, 4, 4, jce_yuv_color_from_cicp(1, false)};
    TEST_ASSERT_TRUE(jce_video_frame_prepare(&p, &input, 2u));
    uint8_t *saved = p.rgba;
    size_t cap = p.capacity;
    uint8_t before[16];
    memcpy(before, p.rgba, sizeof(before));
    input.y_stride = 1;
    TEST_ASSERT_FALSE(jce_video_frame_prepare(&p, &input, 2u));
    input.y_stride = 4;
    input.uv_stride = 1;
    TEST_ASSERT_FALSE(jce_video_frame_prepare(&p, &input, 2u));
    input.uv_stride = 2;
    input.y = NULL;
    TEST_ASSERT_FALSE(jce_video_frame_prepare(&p, &input, 2u));
    TEST_ASSERT_EQUAL_PTR(saved, p.rgba);
    TEST_ASSERT_EQUAL_UINT64(cap, p.capacity);
    TEST_ASSERT_EQUAL_MEMORY(before, p.rgba, sizeof(before));
    TEST_ASSERT_EQUAL_INT(2, p.width);
    TEST_ASSERT_EQUAL_INT(2, p.height);
    jce_video_frame_destroy(&p);
}

static void test_hevc_perf_stage_counts_and_disabled_state(void)
{
    JceHevcPerf p = {0};
    p.frequency = 1000u;
    jce_hevc_perf_record(&p, JCE_HEVC_PERF_IO, 100u, 200u);
    TEST_ASSERT_EQUAL_UINT64(0u, p.count[JCE_HEVC_PERF_IO]);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, p.ema[JCE_HEVC_PERF_IO]);
    p.enabled = true;
    jce_hevc_perf_record(&p, JCE_HEVC_PERF_IO, 100u, 200u);
    jce_hevc_perf_record(&p, JCE_HEVC_PERF_IO, 200u, 400u);
    TEST_ASSERT_EQUAL_UINT64(2u, p.count[JCE_HEVC_PERF_IO]);
    TEST_ASSERT_EQUAL_DOUBLE(110000.0, p.ema[JCE_HEVC_PERF_IO]);
    TEST_ASSERT_EQUAL_DOUBLE(200000.0, p.last[JCE_HEVC_PERF_IO]);
    TEST_ASSERT_EQUAL_UINT64(0u, p.count[JCE_HEVC_PERF_CODEC]);
    jce_hevc_perf_record(&p, (JceHevcPerfStage)-1, 100u, 200u);
    jce_hevc_perf_record(&p, JCE_HEVC_PERF_COUNT, 100u, 200u);
    TEST_ASSERT_EQUAL_UINT64(2u, p.count[JCE_HEVC_PERF_IO]);
    jce_hevc_perf_record(&p, JCE_HEVC_PERF_PUBLISH, 300u, 200u);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, p.last[JCE_HEVC_PERF_PUBLISH]);
    TEST_ASSERT_EQUAL_UINT64(1u, p.count[JCE_HEVC_PERF_PUBLISH]);
}

static void test_display_preparation_restores_buffer_on_success_and_failure(void)
{
    size_t bytes = 17u;
    TEST_ASSERT_FALSE(jce_video_frame_rgba_bytes(2, 2, NULL));
    TEST_ASSERT_FALSE(jce_video_frame_rgba_bytes(-1, 2, &bytes));
    TEST_ASSERT_EQUAL_UINT64(17u, bytes);
    TEST_ASSERT_TRUE(jce_video_frame_rgba_bytes(2, 2, &bytes));
    TEST_ASSERT_EQUAL_UINT64(16u, bytes);
    TEST_ASSERT_EQUAL_INT(SIZE_MAX > UINT32_MAX,
        jce_video_frame_rgba_bytes(INT_MAX, INT_MAX, &bytes));
    JceVideoFrameProcessor processor = {0};
    uint8_t *pixels = NULL;
    size_t capacity = 0;
    int width = 0, height = 0;
    uint8_t y[4] = {16, 80, 160, 235}, u[1] = {128}, v[1] = {128};
    JceYuv420Frame input = {y, u, v, 2, 1, 2, 2, {JCE_YUV_BT601, false}};
    TEST_ASSERT_TRUE(jce_video_frame_prepare_display(&processor, &input, 0,
        &pixels, &capacity, &width, &height));
    TEST_ASSERT_NULL(processor.rgba);
    TEST_ASSERT_EQUAL_INT(2, width);
    TEST_ASSERT_EQUAL_INT(2, height);
    uint8_t expected[16];
    jce_yuv420_to_rgba_color(y, 2, u, 1, v, 1, expected, 2, 2, input.color);
    TEST_ASSERT_EQUAL_MEMORY(expected, pixels, sizeof(expected));
    uint8_t *saved = pixels;
    size_t saved_capacity = capacity;
    TEST_ASSERT_FALSE(jce_video_frame_prepare_display(&processor, NULL, 0,
        &pixels, &capacity, &width, &height));
    TEST_ASSERT_EQUAL_PTR(saved, pixels);
    TEST_ASSERT_EQUAL_UINT64(saved_capacity, capacity);
    TEST_ASSERT_NULL(processor.rgba);
    TEST_ASSERT_EQUAL_MEMORY(expected, pixels, sizeof(expected));
    TEST_ASSERT_TRUE(jce_video_frame_prepare_display(&processor, &input, 0,
        &pixels, &capacity, &width, &height));
    TEST_ASSERT_EQUAL_PTR(saved, pixels);
    jce_video_frame_destroy(&processor);
    jce_allocator_t allocator = jce_allocator_default();
    JCE_AFREE(allocator, pixels);
}

int main(void)
{
    const char *probe = getenv("JCE_TEST_VIDEO_PROBE");
    if (probe && probe[0]) {
        JceReadSource *source=jce_read_source_open_file(probe);
        if (!source) return 2;
        JceVideo video=jce_video_load_source(source,probe);
        if (video == JCE_VIDEO_INVALID) return 3;
        JceVideoInfo vi;
        memset(&vi, 0, sizeof(vi));
        jce_video_get_info(video, &vi);
        const char *preview = getenv("JCE_TEST_VIDEO_PREVIEW_MAX");
        uint32_t preview_max = preview ? (uint32_t)strtoul(preview, NULL, 10) : 0u;
        if (!jce_video_set_preview_max_dimension(video, preview_max)) return 4;
        uint64_t preview_start = jce_time_ticks_ms();
        while (jce_video_get_frame_counter(video) == 0u
               && jce_time_ticks_ms() - preview_start < 10000u) {
            jce_video_advance(video, 0.0);
            jce_thread_sleep_ms(16);
        }
        double initial_pts = -1.0;
        if (!jce_video_get_frame_rgba(video, NULL, NULL, &initial_pts)
            || jce_video_get_time(video) != 0.0 || fabs(initial_pts) > 0.000001)
            return 12;
        uint64_t paused_counter = jce_video_get_frame_counter(video);
        for (uint32_t i = 0; i < 8u; ++i) {
            jce_video_advance(video, 0.0);
            jce_thread_sleep_ms(16);
        }
        if (jce_video_get_frame_counter(video) != paused_counter
            || jce_video_get_time(video) != 0.0) return 13;
        printf("probe paused preview frame=%llu clock=%.9f\n",
               (unsigned long long)paused_counter, jce_video_get_time(video));
        JceAudio *audio = NULL;
        JceVoice voice = JCE_VOICE_INVALID;
        const char *audio_probe = getenv("JCE_TEST_VIDEO_AUDIO_DEVICE");
        if (audio_probe && audio_probe[0] == '1') {
            uint32_t channels = 0, samplerate = 0;
            double audio_duration = 0.0;
            uint64_t ready_start = jce_time_ticks_ms();
            while (jce_video_get_audio_status(video) == JCE_VIDEO_AUDIO_STATUS_DECODING
                   && jce_time_ticks_ms() - ready_start < 10000u)
                jce_thread_sleep_ms(5);
            if (!jce_video_get_audio_format(video, &channels, &samplerate, &audio_duration))
                return 12;
            audio = jce_audio_create();
            if (!audio) return 13;
            const char *mute = getenv("JCE_TEST_VIDEO_AUDIO_MUTE");
            voice = jce_audio_play_stream(audio, probe_audio_pull, &video,
                                          (uint16_t)channels, samplerate,
                                          mute && mute[0] == '1' ? 0.0f : 0.1f, 1.0f);
            if (voice == JCE_VOICE_INVALID) return 14;
            printf("probe audio device=%uHz channels=%u duration=%.3f\n",
                   samplerate, channels, audio_duration);
        }
        unsigned seconds = 4u;
        const char *seconds_env = getenv("JCE_TEST_VIDEO_SECONDS");
        if (seconds_env && atoi(seconds_env) > 0 && atoi(seconds_env) <= 120)
            seconds = (unsigned)atoi(seconds_env);
        unsigned tick_ms = 16u;
        const char *tick_env = getenv("JCE_TEST_VIDEO_TICK_MS");
        if (tick_env && atoi(tick_env) > 0 && atoi(tick_env) <= 1000)
            tick_ms = (unsigned)atoi(tick_env);
        const char *loop_env = getenv("JCE_TEST_VIDEO_LOOP");
        const char *resume_env = getenv("JCE_TEST_VIDEO_RESUME_SECONDS");
        unsigned resume_seconds = resume_env ? (unsigned)atoi(resume_env) : 0u;
        if (resume_seconds > 5u) return 21;
        const bool loop = loop_env && loop_env[0] == '1';
        unsigned wraps = 0u;
        double previous_time = 0.0;
        if (loop) jce_video_set_loop(video, true);
        printf("probe update interval=%ums loop=%d\n", tick_ms, loop ? 1 : 0);
        uint64_t next_log = 0;
        uint64_t start = jce_time_ticks_ms();
        uint64_t last = start;
        while (jce_time_ticks_ms() - start < (uint64_t)seconds * 1000u) {
            uint64_t now = jce_time_ticks_ms();
            jce_video_advance(video, (double)(now - last) / 1000.0);
            const double media_time = jce_video_get_time(video);
            if (media_time + 0.25 < previous_time) ++wraps;
            previous_time = media_time;
            if (now - start >= next_log) {
                JceVideoPerfStats sample = {0};
                double pts = -1.0;
                jce_video_get_perf_stats(video, &sample);
                jce_video_get_frame_rgba(video, NULL, NULL, &pts);
                printf("timeline wall=%.3f media=%.3f picture=%.3f audio=%.3f displayed=%llu dropped=%llu queue=%d\n",
                       (double)(now - start) / 1000.0, jce_video_get_time(video),
                       pts, jce_video_audio_get_time(video),
                       (unsigned long long)sample.frames_displayed,
                       (unsigned long long)sample.frames_dropped, sample.q_count);
                next_log += 1000u;
            }
            last = now;
            uint64_t done = jce_time_ticks_ms();
            if (done - now < tick_ms)
                jce_thread_sleep_ms((uint32_t)(tick_ms - (done - now)));
        }
        JceVideoPerfStats stats;
        memset(&stats, 0, sizeof(stats));
        jce_video_get_perf_stats(video, &stats);
        int output_w = 0, output_h = 0;
        if (!jce_video_get_frame_rgba(video, &output_w, &output_h, NULL)) return 5;
        if (preview_max && (output_w > (int)preview_max || output_h > (int)preview_max)) return 6;
        if (!preview_max && (output_w != vi.width || output_h != vi.height)) return 7;
        printf("probe output=%dx%d preview_max=%u\n", output_w, output_h, preview_max);
        printf("probe %s %dx%d %.1ffps elapsed=%llu ms media=%.3f dec=%llu disp=%llu drop=%llu decode=%.3f ms convert=%.3f ms pop=%.3f ms queue=%d\n",
               vi.video_codec, vi.width, vi.height, vi.framerate,
               (unsigned long long)(jce_time_ticks_ms() - start),
               jce_video_get_time(video),
               (unsigned long long)stats.frames_decoded,
               (unsigned long long)stats.frames_displayed,
               (unsigned long long)stats.frames_dropped,
               stats.decode_us_ema / 1000.0,
               stats.convert_us_ema / 1000.0,
               stats.pop_us_ema / 1000.0, stats.q_count);
        if (loop) {
            printf("probe loop wraps=%u ended=%d\n", wraps, jce_video_has_ended(video));
            if (wraps < 2u || jce_video_has_ended(video)) return 16;
        } else if ((double)seconds > vi.duration + 0.25 && vi.duration > 0.0) {
            if (!jce_video_has_ended(video)) return 15;
            const char *tail = getenv("JCE_TEST_VIDEO_REQUIRE_FINAL_FRAME");
            if (tail && tail[0] == '1' && vi.framerate > 0.0) {
                double final_pts = -1.0;
                jce_video_get_frame_rgba(video, NULL, NULL, &final_pts);
                printf("probe final pts=%.6f duration=%.6f\n", final_pts, vi.duration);
                const char *oracle = getenv("JCE_TEST_VIDEO_FINAL_PTS");
                if (oracle && oracle[0]) {
                    if (fabs(final_pts - atof(oracle)) > .5 / vi.framerate + .001) return 17;
                } else if (final_pts < vi.duration - 1.5 / vi.framerate - .01) return 17;
            }
        }
        printf("probe ended=%d audio_eof=%d\n", jce_video_has_ended(video) ? 1 : 0,
               jce_video_audio_eof(video) ? 1 : 0);
        if (audio) {
            if (resume_seconds) {
                /* Mirror the public transport lifecycle: retire resampler/
                 * source state before seeking, then create a fresh voice
                 * only after the new picture and PCM are both ready. */
                jce_audio_stop(audio, voice);
                voice = JCE_VOICE_INVALID;
            } else jce_audio_pause(audio, voice);
        }
        double target = vi.duration * 0.5;
        uint64_t before_frame = jce_video_get_frame_counter(video);
        uint64_t seek_start = jce_time_ticks_ms();
        jce_video_seek(video, target, true);
        printf("probe seek %.9fs call=%llu ms\n", target,
               (unsigned long long)(jce_time_ticks_ms() - seek_start));
        while ((!jce_video_is_ready_to_play(video)
                || jce_video_get_frame_counter(video) == before_frame)
               && jce_time_ticks_ms() - seek_start < 10000u) {
            jce_video_advance(video, 0.016);
            if (fabs(jce_video_get_time(video) - target) > 0.000001) return 18;
            jce_thread_sleep_ms(16);
        }
        if (!jce_video_is_ready_to_play(video)) return 19;
        double landed = -1.0;
        (void)jce_video_get_frame_rgba(video, NULL, NULL, &landed);
        jce_video_get_perf_stats(video, &stats);
        printf("probe seek frame=%llu wait=%llu ms pts=%.9f\n",
               (unsigned long long)jce_video_get_frame_counter(video),
               (unsigned long long)(jce_time_ticks_ms() - seek_start), landed);
        printf("probe seek stats dec=%llu disp=%llu drop=%llu q=%d eof=%d\n",
               (unsigned long long)stats.frames_decoded,
               (unsigned long long)stats.frames_displayed,
               (unsigned long long)stats.frames_dropped,
               stats.q_count, stats.worker_eof ? 1 : 0);
        bool seek_ok = jce_video_get_frame_counter(video) > before_frame
            && landed >= target - 0.05 && landed <= target + 0.25;
        /* Separate opt-in pixel witness, never enabled in cadence timings. */
        const char *picture_out = getenv("JCE_TEST_VIDEO_SEEK_RGBA");
        if (picture_out && picture_out[0] && seek_ok) {
            int picture_w = 0, picture_h = 0;
            double picture_pts = -1.0;
            const uint8_t *picture = jce_video_get_frame_rgba(video, &picture_w, &picture_h, &picture_pts);
            if (!picture || picture_w <= 0 || picture_h <= 0 ||
                (uint64_t)picture_w * picture_h > SIZE_MAX / 4u ||
                !jce_fs_host_write_all(picture_out, picture, (size_t)picture_w * picture_h * 4u)) return 20;
            printf("probe seek picture=%dx%d pts=%.9f bytes=%llu\n", picture_w, picture_h, picture_pts,
                   (unsigned long long)((uint64_t)picture_w * picture_h * 4u));
        }
        if (preview_max && seek_ok) {
            if (jce_video_set_preview_max_dimension(video, 1u)) return 9;
            if (!jce_video_set_preview_max_dimension(video, 0u)) return 10;
            jce_video_seek(video,target,true);
            uint64_t previous = jce_video_get_frame_counter(video);
            uint64_t restore_start = jce_time_ticks_ms();
            jce_video_advance(video, 0.0);
            while (jce_video_get_frame_counter(video) == previous
                   && jce_time_ticks_ms() - restore_start < 10000u) {
                jce_video_advance(video, 0.0);
                jce_thread_sleep_ms(16);
            }
            jce_video_get_frame_rgba(video, &output_w, &output_h, NULL);
            if (output_w != vi.width || output_h != vi.height) return 11;
            printf("probe restored native output=%dx%d\n", output_w, output_h);
        }
        if (resume_seconds && (!seek_ok || !probe_resume_after_seek(
                &video, audio, &voice, resume_seconds, tick_ms))) return 22;
        if (audio) jce_audio_destroy(audio);
        jce_video_unload(video);
        JceReadSourceStats input={0};
        jce_read_source_get_stats(source,&input);
        printf("probe input size=%llu backend_reads=%llu bytes_read=%llu owned=%llu\n",
            (unsigned long long)input.size,(unsigned long long)input.read_calls,
            (unsigned long long)input.bytes_read,(unsigned long long)input.owned_bytes);
        jce_read_source_close(source);
        return seek_ok ? 0 : 8;
    }
    if (find_fixture()) {
        printf("fixture: %s (%llu bytes)\n", s_blob_path,
               (unsigned long long)s_blob_size);
    } else {
        printf("fixture: none found\n");
    }

    UNITY_BEGIN();
    RUN_TEST(test_display_preparation_restores_buffer_on_success_and_failure);
    RUN_TEST(test_private_perf_disabled_never_reads_clock);
    RUN_TEST(test_hevc_perf_stage_counts_and_disabled_state);
    RUN_TEST(test_frame_preparation_rejects_invalid_planes_and_stride);
    RUN_TEST(test_hevc_diagnostic_options_are_exact_and_bounded);
    RUN_TEST(test_late_presentation_preserves_generation_seek_and_final_picture);
    RUN_TEST(test_timestamp_normalization_preserves_seek_and_repeated_pts);
    RUN_TEST(test_seek_trace_is_opt_in_and_generation_bound);
    RUN_TEST(test_audio_clock_preserves_picture_cadence);
    RUN_TEST(test_audio_clock_stalls_and_recovery_are_bounded);
    RUN_TEST(test_h264_mp4_opens_a_real_decoder);
    RUN_TEST(test_fragmented_mp4_has_a_sample_index);
    RUN_TEST(test_load_rejects_non_video_bytes);
    RUN_TEST(test_preview_downsample_keeps_centres_and_ignores_row_padding);
    RUN_TEST(test_odd_video_chroma_queue_matches_reference);
    RUN_TEST(test_yuv_dispatch_matches_scalar_color_and_tails);
    int rc = UNITY_END();

    if (s_blob) jce_fs_buffer_free(s_blob);
    return rc;
}
