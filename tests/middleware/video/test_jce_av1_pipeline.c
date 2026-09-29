/* Exercise backpressure, delayed PTS and EOF with an actual AV1 MP4. */
#include "unity.h"
#include "jce_av1_packet.h"
#include <jce/middleware/video/jce_mp4_parser.h>
#include <jce/os/core/jce_filesystem.h>
#include <stdlib.h>
#include <string.h>

static void *s_data;
static uint64_t s_size;
static JceMp4Parser *s_parser;
static JceMp4VideoTrackInfo s_track;

void setUp(void) {}
void tearDown(void) {}

static uint64_t frame_hash(const uint8_t *y, ptrdiff_t stride,
                           uint32_t w, uint32_t h)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    /* Sample every row at several x positions; includes padded-stride handling. */
    for (uint32_t row = 0; row < h; ++row)
        for (uint32_t x = 0; x < w; x += w / 31u + 1u)
            hash = (hash ^ y[row * stride + x]) * UINT64_C(1099511628211);
    return hash;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8u * i));
}

static void check_ivf(const uint64_t *gold, uint32_t count)
{
    size_t cfg_size = s_track.decoder_config_bytes > 4
        ? s_track.decoder_config_bytes - 4 : 0;
    size_t size = 32u + cfg_size;
    for (uint32_t i = 0; i < count; ++i) {
        JceMp4SampleInfo si;
        TEST_ASSERT_TRUE(jce_mp4_parser_get_video_sample(s_parser, i, &si));
        size += 12u + si.size_bytes;
    }
    uint8_t *ivf = calloc(size, 1);
    TEST_ASSERT_NOT_NULL(ivf);
    memcpy(ivf, "DKIF", 4);
    ivf[6] = 32;
    memcpy(ivf + 8, "AV01", 4);
    ivf[12] = (uint8_t)s_track.width;
    ivf[13] = (uint8_t)(s_track.width >> 8);
    ivf[14] = (uint8_t)s_track.height;
    ivf[15] = (uint8_t)(s_track.height >> 8);
    put_u32(ivf + 16, 60);
    put_u32(ivf + 20, 1);
    put_u32(ivf + 24, count);
    size_t pos = 32;
    for (uint32_t i = 0; i < count; ++i) {
        JceMp4SampleInfo si;
        TEST_ASSERT_TRUE(jce_mp4_parser_get_video_sample(s_parser, i, &si));
        put_u32(ivf + pos, si.size_bytes + (uint32_t)(i == 0 ? cfg_size : 0));
        put_u32(ivf + pos + 4, i);
        pos += 12;
        if (i == 0 && cfg_size) {
            memcpy(ivf + pos, (const uint8_t *)s_track.decoder_config + 4, cfg_size);
            pos += cfg_size;
        }
        memcpy(ivf + pos, (const uint8_t *)s_data + si.offset, si.size_bytes);
        pos += si.size_bytes;
    }
    JceAv1Decoder *dec = jce_av1_open_memory(ivf, size, NULL);
    TEST_ASSERT_NOT_NULL(dec);
    const uint8_t *y, *u, *v;
    ptrdiff_t ys, uvs;
    uint32_t w, h, received = 0;
    while (jce_av1_decode_next(dec, &y, &ys, &u, &uvs, &v, &w, &h)) {
        TEST_ASSERT_LESS_THAN_UINT32(count, received);
        TEST_ASSERT_EQUAL_UINT64(gold[received], frame_hash(y, ys, w, h));
        ++received;
    }
    TEST_ASSERT_EQUAL_UINT32(count, received);
    jce_av1_close(dec);
    JceReadSource *source = jce_read_source_open_memory(ivf, size, false);
    TEST_ASSERT_NOT_NULL(source);
    dec = jce_av1_open_source(source, NULL);
    jce_read_source_close(source); /* Decoder must retain its source. */
    TEST_ASSERT_NOT_NULL(dec);
    received = 0;
    while (jce_av1_decode_next(dec, &y, &ys, &u, &uvs, &v, &w, &h)) {
        TEST_ASSERT_LESS_THAN_UINT32(count, received);
        TEST_ASSERT_EQUAL_UINT64(gold[received], frame_hash(y, ys, w, h));
        ++received;
    }
    TEST_ASSERT_EQUAL_UINT32(count, received);
    jce_av1_close(dec);
    free(ivf);
}

static void test_invalid_arguments(void)
{
    JceAv1PacketFrame f;
    TEST_ASSERT_EQUAL_INT(-1, jce_av1_packet_receive(NULL, &f));
    TEST_ASSERT_EQUAL_INT(-1, jce_av1_packet_send(NULL, "x", 1, 0));
}

static void test_parallel_matches_serial_and_drains(void)
{
    if (!s_parser) {
        TEST_IGNORE_MESSAGE("set JCE_TEST_AV1_MP4 to a real AV1 MP4");
        return;
    }
    uint32_t count = s_track.sample_count;
    TEST_ASSERT_GREATER_THAN_UINT32(0, count);
    uint64_t *gold = calloc(count, sizeof(*gold));
    uint64_t *pts = calloc(count, sizeof(*pts));
    TEST_ASSERT_NOT_NULL(gold);
    TEST_ASSERT_NOT_NULL(pts);
    JceAv1Decoder *serial = jce_av1_open_packet();
    JceAv1Decoder *parallel = jce_av1_packet_open_parallel();
    TEST_ASSERT_NOT_NULL(serial);
    TEST_ASSERT_NOT_NULL(parallel);
    const uint8_t *y, *u, *v;
    ptrdiff_t ys, uvs;
    uint32_t w, h;
    if (s_track.decoder_config_bytes > 4) {
        const uint8_t *cfg = (const uint8_t *)s_track.decoder_config + 4;
        uint32_t sz = s_track.decoder_config_bytes - 4;
        (void)jce_av1_decode_packet(serial, cfg, sz, &y, &ys, &u, &uvs,
                                   &v, &w, &h);
        TEST_ASSERT_EQUAL_INT(1, jce_av1_packet_send(parallel, cfg, sz, 0));
    }
    for (uint32_t i = 0; i < count; ++i) {
        JceMp4SampleInfo si;
        TEST_ASSERT_TRUE(jce_mp4_parser_get_video_sample(s_parser, i, &si));
        TEST_ASSERT_TRUE(jce_av1_decode_packet(serial,
            (const uint8_t *)s_data + si.offset, si.size_bytes,
            &y, &ys, &u, &uvs, &v, &w, &h));
        gold[i] = frame_hash(y, ys, w, h);
        pts[i] = si.timestamp;
    }
    jce_av1_close(serial);
    uint32_t sent = 0, received = 0, waits = 0, attempts = 0;
    /* Deliberately fill the decoder until it rejects input. Retrying the
     * same packet after receiving must neither lose nor duplicate a frame. */
    while (sent < count) {
        TEST_ASSERT_LESS_THAN_UINT32(count * 256u, ++attempts);
        JceMp4SampleInfo si;
        TEST_ASSERT_TRUE(jce_mp4_parser_get_video_sample(s_parser, sent, &si));
        int result = jce_av1_packet_send(parallel,
            (const uint8_t *)s_data + si.offset, si.size_bytes,
            (int64_t)si.timestamp);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, result);
        if (result == 1) { ++sent; continue; }
        ++waits;
        JceAv1PacketFrame f;
        int got = jce_av1_packet_receive(parallel, &f);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, got);
        if (!got) continue; /* a packet can also contain non-display OBUs */
        TEST_ASSERT_LESS_THAN_UINT32(count, received);
        TEST_ASSERT_EQUAL_UINT64(pts[received], (uint64_t)f.timestamp);
        TEST_ASSERT_EQUAL_UINT64(gold[received],
            frame_hash(f.y, f.y_stride, f.width, f.height));
        ++received;
    }
    JceAv1PacketFrame f;
    int result;
    bool drain_probe = false;
    for (;;) {
        result = jce_av1_packet_receive(parallel, &f);
        if (result != 1) {
            if (result == 0 && !drain_probe) { drain_probe = true; continue; }
            break;
        }
        TEST_ASSERT_LESS_THAN_UINT32(count, received);
        TEST_ASSERT_EQUAL_UINT64(pts[received], (uint64_t)f.timestamp);
        TEST_ASSERT_EQUAL_UINT64(gold[received],
            frame_hash(f.y, f.y_stride, f.width, f.height));
        ++received;
    }
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_UINT32(count, received);
    TEST_ASSERT_GREATER_THAN_UINT32(0, waits);
    TEST_ASSERT_EQUAL_INT(0, jce_av1_packet_receive(parallel, &f));
    jce_av1_close(parallel);
    check_ivf(gold, count);
    free(gold);
    free(pts);
}

int main(void)
{
    const char *path = getenv("JCE_TEST_AV1_MP4");
    if (path && path[0]) {
        s_data = jce_fs_host_read_all(path, &s_size);
        if (s_data) s_parser = jce_mp4_parser_open_memory(s_data, (size_t)s_size, NULL);
        if (s_parser) jce_mp4_parser_get_video_track_info(s_parser, &s_track);
        if (!s_parser || strcmp(s_track.codec, "av01") != 0) return 2;
    }
    UNITY_BEGIN();
    RUN_TEST(test_invalid_arguments);
    RUN_TEST(test_parallel_matches_serial_and_drains);
    int result = UNITY_END();
    if (s_parser) jce_mp4_parser_close(s_parser);
    jce_fs_buffer_free(s_data);
    return result;
}
