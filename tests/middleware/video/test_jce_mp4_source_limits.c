/* Reject malformed container metadata before upstream allocations. */
#include "unity.h"
#include "jce_mp4_source.h"
#include <jce/api_core.h>
#include <string.h>

static JceReadSource *source;
static uint8_t *moov;
void setUp(void) { source = NULL; moov = NULL; }
void tearDown(void) { jce_free(moov); jce_read_source_close(source); }

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24); p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8); p[3] = (uint8_t)value;
}

static size_t oversized_read(void *user, uint64_t offset, void *out, size_t size)
{
    (void)user;
    memset(out, 0, size);
    if (offset == 0 && size >= 8u) {
        put32(out, 33u * 1024u * 1024u);
        memcpy((uint8_t *)out + 4u, "moov", 4u);
    }
    return size;
}

static void test_oversized_moov_stops_at_read_ahead(void)
{
    JceReadSourceDesc desc = {40u * 1024u * 1024u, oversized_read, NULL, NULL};
    JceReadSourceStats stats;
    uint64_t offset = 0;
    size_t bytes = 0;
    source = jce_read_source_create(&desc);
    TEST_ASSERT_NOT_NULL(source);
    moov = jce_mp4_source_moov(source, &offset, &bytes);
    TEST_ASSERT_NULL(moov);
    TEST_ASSERT_TRUE(jce_read_source_get_stats(source, &stats));
    TEST_ASSERT_EQUAL_UINT64(65536u, stats.bytes_read);
    TEST_ASSERT_LESS_THAN_UINT64(80u * 1024u, stats.owned_bytes);
}

static void test_sample_runs_and_table_payloads_are_bounded(void)
{
    uint8_t data[32] = {0};
    uint64_t offset = 0;
    size_t bytes = 0;
    put32(data, sizeof(data)); memcpy(data + 4u, "moov", 4u);
    put32(data + 8u, 24u); memcpy(data + 12u, "stts", 4u);
    put32(data + 20u, 1u); put32(data + 24u, 1u); put32(data + 28u, 1u);
    source = jce_read_source_open_memory(data, sizeof(data), false);
    moov = jce_mp4_source_moov(source, &offset, &bytes);
    TEST_ASSERT_NOT_NULL(moov); TEST_ASSERT_EQUAL_UINT(sizeof(data), bytes);
    jce_free(moov); moov = NULL; jce_read_source_close(source); source = NULL;

    put32(data + 24u, UINT32_MAX); /* Tiny run table requests billions of samples. */
    source = jce_read_source_open_memory(data, sizeof(data), false);
    moov = jce_mp4_source_moov(source, &offset, &bytes);
    TEST_ASSERT_NULL(moov);
    jce_read_source_close(source); source = NULL;

    put32(data + 24u, 1u); put32(data + 20u, 2u); /* Count exceeds actual payload. */
    source = jce_read_source_open_memory(data, sizeof(data), false);
    moov = jce_mp4_source_moov(source, &offset, &bytes);
    TEST_ASSERT_NULL(moov);
}

static void test_extended_box_and_truncation(void)
{
    uint8_t data[16] = {0};
    uint64_t body = 0, end = 0;
    uint32_t type = 0;
    put32(data, 1u); memcpy(data + 4u, "free", 4u); put32(data + 12u, 16u);
    source = jce_read_source_open_memory(data, sizeof(data), false);
    TEST_ASSERT_TRUE(jce_mp4_source_box(source, 0u, &type, &body, &end));
    TEST_ASSERT_EQUAL_UINT64(16u, body); TEST_ASSERT_EQUAL_UINT64(16u, end);
    jce_read_source_close(source); source = NULL;
    source = jce_read_source_open_memory(data, sizeof(data) - 1u, false);
    TEST_ASSERT_FALSE(jce_mp4_source_box(source, 0u, &type, &body, &end));
    TEST_ASSERT_FALSE(jce_mp4_source_box(source, UINT64_MAX, &type, &body, &end));
}

static void test_composition_times_preserve_b_frame_order_and_signed_offsets(void)
{
    const uint64_t dts[] = {0u, 100u, 200u, 300u};
    const uint64_t expected[] = {0u, 300u, 100u, 200u};
    uint64_t pts[4] = {0};
    uint8_t ctts[32] = {0};
    put32(ctts + 4u, 3u);
    put32(ctts + 8u, 1u); put32(ctts + 12u, 200u);
    put32(ctts + 16u, 1u); put32(ctts + 20u, 400u);
    put32(ctts + 24u, 2u); put32(ctts + 28u, 100u);
    TEST_ASSERT_TRUE(jce_mp4_composition_index(dts, 4u, ctts, sizeof(ctts), pts));
    TEST_ASSERT_EQUAL_UINT64_ARRAY(expected, pts, 4u);
    ctts[0] = 1u;
    put32(ctts + 12u, 0u); put32(ctts + 20u, 200u);
    put32(ctts + 28u, (uint32_t)-100);
    TEST_ASSERT_TRUE(jce_mp4_composition_index(dts, 4u, ctts, sizeof(ctts), pts));
    TEST_ASSERT_EQUAL_UINT64_ARRAY(expected, pts, 4u);
    TEST_ASSERT_FALSE(jce_mp4_composition_index(dts, 4u, ctts, sizeof(ctts) - 1u, pts));
    put32(ctts + 24u, 3u);
    TEST_ASSERT_FALSE(jce_mp4_composition_index(dts, 4u, ctts, sizeof(ctts), pts));
    ctts[0] = 2u;
    TEST_ASSERT_FALSE(jce_mp4_composition_index(dts, 4u, ctts, sizeof(ctts), pts));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_oversized_moov_stops_at_read_ahead);
    RUN_TEST(test_sample_runs_and_table_payloads_are_bounded);
    RUN_TEST(test_extended_box_and_truncation);
    RUN_TEST(test_composition_times_preserve_b_frame_order_and_signed_offsets);
    return UNITY_END();
}
