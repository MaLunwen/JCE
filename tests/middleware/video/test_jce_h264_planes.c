/* Real decoder B-frame feed/pending/EOF/flush parity and owned-plane lifetime. */
#include "unity.h"
#include "jce_h264_decode.h"
#include "jce_h264_planes.h"
#include "jce_mp4_source.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_hash.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_owned_odd_planes(void)
{
    uint8_t y[15], u[6], v[6];
    memset(y, 90, sizeof(y)); memset(u, 100, sizeof(u)); memset(v, 110, sizeof(v));
    uint8_t *input[3] = {y, u, v};
    JceH264Planes planes = {0};
    TEST_ASSERT_TRUE(jce_h264_planes_copy(&planes, input, 5, 3, 3, 3));
    TEST_ASSERT_EQUAL_INT(3, planes.frame.y_stride);
    TEST_ASSERT_EQUAL_INT(2, planes.frame.uv_stride);
    memset(y, 0, sizeof(y)); memset(u, 0, sizeof(u));
    TEST_ASSERT_EQUAL_UINT8(90, planes.frame.y[8]);
    TEST_ASSERT_EQUAL_UINT8(100, planes.frame.u[3]);
    TEST_ASSERT_EQUAL_UINT8(110, planes.frame.v[3]);
    TEST_ASSERT_FALSE(jce_h264_planes_copy(&planes, input, 2, 3, 3, 3));
    TEST_ASSERT_FALSE(planes.valid);
    JCE_FREE(planes.bytes);
}

static void test_owned_planes_do_not_impose_4096_resolution_cap(void)
{
    uint8_t *source = JCE_MALLOC(16384u);
    TEST_ASSERT_NOT_NULL(source);
    memset(source, 77, 16384u);
    uint8_t *input[3] = {source, source + 8192u, source + 12288u};
    JceH264Planes planes = {0};
    TEST_ASSERT_TRUE(jce_h264_planes_copy(&planes, input, 8192, 4096, 8192, 1));
    TEST_ASSERT_EQUAL_INT(8192, planes.frame.width);
    TEST_ASSERT_EQUAL_UINT8(77, planes.frame.y[8191]);
    TEST_ASSERT_FALSE(jce_h264_planes_copy(&planes, input, 8192, 4096, UINT32_MAX, 1));
    JCE_FREE(planes.bytes); JCE_FREE(source);
}

#ifdef JCE_ENABLE_PATENTED_CODECS
typedef struct { uint64_t pts[128], hashes[128]; unsigned count; } Frames;

static void append_frame(JceH264Decoder *decoder, bool planar, const uint8_t *rgba,
                         uint32_t width, uint32_t height, Frames *frames)
{
    uint8_t *converted = NULL; size_t capacity = 0;
    TEST_ASSERT_LESS_THAN_UINT(128, frames->count);
    TEST_ASSERT_GREATER_THAN_UINT32(0, width);
    TEST_ASSERT_GREATER_THAN_UINT32(0, height);
    if (planar) {
        JceYuv420Frame frame;
        TEST_ASSERT_NULL(rgba);
        TEST_ASSERT_TRUE(jce_h264_decoder_get_yuv(decoder, &frame));
        TEST_ASSERT_EQUAL_UINT32(width, frame.width);
        TEST_ASSERT_EQUAL_UINT32(height, frame.height);
        TEST_ASSERT_TRUE(jce_yuv_buffer_reserve(&converted, &capacity, (size_t)width * height * 4));
        jce_yuv420_to_rgba_color(frame.y, frame.y_stride, frame.u, frame.uv_stride,
            frame.v, frame.uv_stride, converted, width, height, frame.color);
        rgba = converted;
    } else {
        JceYuv420Frame frame;
        TEST_ASSERT_NOT_NULL(rgba);
        TEST_ASSERT_FALSE(jce_h264_decoder_get_yuv(decoder, &frame));
    }
    unsigned i = frames->count++;
    frames->pts[i] = jce_h264_decoder_frame_timestamp(decoder);
    frames->hashes[i] = jce_fnv1a64(rgba, (size_t)width * height * 4);
    if (i) TEST_ASSERT_GREATER_THAN_UINT64(frames->pts[i-1], frames->pts[i]);
    JCE_FREE(converted);
}

static Frames decode_clip(JceH264Decoder *decoder, JceMp4Parser *parser,
                         const JceMp4VideoTrackInfo *track, const uint8_t *blob,
                         uint64_t bytes, bool planar)
{
    Frames frames = {0};
    for (uint32_t i = 0; i < track->sample_count; ++i) {
        JceMp4SampleInfo sample; uint64_t pts;
        TEST_ASSERT_TRUE(jce_mp4_parser_get_video_sample(parser, i, &sample));
        TEST_ASSERT_TRUE(jce_mp4_parser_video_presentation_time(parser, i, &pts));
        TEST_ASSERT_TRUE(sample.offset <= bytes && sample.size_bytes <= bytes - sample.offset);
        const uint8_t *rgba = NULL; uint32_t w = 0, h = 0;
        jce_h264_decoder_set_timestamp(decoder, pts);
        if (jce_h264_decode_frame(decoder, blob + sample.offset, sample.size_bytes,
                                 track->nal_length_size, &rgba, &w, &h))
            append_frame(decoder, planar, rgba, w, h, &frames);
        while (jce_h264_decoder_drain_pending(decoder, &rgba, &w, &h))
            append_frame(decoder, planar, rgba, w, h, &frames);
    }
    const uint8_t *rgba = NULL; uint32_t w = 0, h = 0;
    while (jce_h264_decoder_drain(decoder, &rgba, &w, &h))
        append_frame(decoder, planar, rgba, w, h, &frames);
    TEST_ASSERT_EQUAL_UINT32(track->sample_count, frames.count);
    return frames;
}

static void test_real_reordered_planes_match_legacy_and_flush(void)
{
    uint64_t bytes = 0;
    void *blob = jce_fs_host_read_all(JCE_TEST_BFRAME_DIR "/bframes.mp4", &bytes);
    TEST_ASSERT_NOT_NULL(blob);
    JceMp4Info info; JceMp4VideoTrackInfo track;
    JceMp4Parser *parser = jce_mp4_parser_open_memory(blob, (size_t)bytes, &info);
    TEST_ASSERT_NOT_NULL(parser);
    TEST_ASSERT_TRUE(jce_mp4_parser_get_video_track_info(parser, &track));
    JceH264Decoder *legacy = jce_h264_decoder_open(track.decoder_config, track.decoder_config_bytes);
    JceH264Decoder *planar = jce_h264_decoder_open(track.decoder_config, track.decoder_config_bytes);
    TEST_ASSERT_NOT_NULL(legacy); TEST_ASSERT_NOT_NULL(planar);
    jce_h264_decoder_set_yuv_output(planar, true);
    Frames left = decode_clip(legacy, parser, &track, blob, bytes, false);
    Frames right = decode_clip(planar, parser, &track, blob, bytes, true);
    TEST_ASSERT_EQUAL_UINT(90, left.count);
    TEST_ASSERT_EQUAL_MEMORY(left.pts, right.pts, left.count * sizeof(uint64_t));
    TEST_ASSERT_EQUAL_MEMORY(left.hashes, right.hashes, left.count * sizeof(uint64_t));
    jce_h264_decoder_flush(planar);
    JceYuv420Frame absent;
    TEST_ASSERT_FALSE(jce_h264_decoder_get_yuv(planar, &absent));
    right = decode_clip(planar, parser, &track, blob, bytes, true);
    TEST_ASSERT_EQUAL_MEMORY(left.pts, right.pts, left.count * sizeof(uint64_t));
    TEST_ASSERT_EQUAL_MEMORY(left.hashes, right.hashes, left.count * sizeof(uint64_t));
    jce_h264_decoder_close(planar); jce_h264_decoder_close(legacy);
    jce_mp4_parser_close(parser); JCE_FREE(blob);
}
#endif

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_owned_odd_planes);
    RUN_TEST(test_owned_planes_do_not_impose_4096_resolution_cap);
#ifdef JCE_ENABLE_PATENTED_CODECS
    RUN_TEST(test_real_reordered_planes_match_legacy_and_flush);
#endif
    return UNITY_END();
}
