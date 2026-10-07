/* CPU-only byte oracle for the exact shipped RGBA8 mip preparation helper. */
#include "renderer/jce_texture_mips.h"
#include "unity.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_rgba_and_alpha_box_average_are_exact(void)
{
    uint8_t chain[24] = {0,10,20,30, 4,14,24,34, 8,18,28,38, 12,22,32,42};
    memset(chain + 16, 0xcc, 8);
    TEST_ASSERT_TRUE(jce_texture_mip_fill(chain, 20, 2, 2));
    const uint8_t expected[4] = {6,16,26,36};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, chain + 16, 4);
    TEST_ASSERT_EQUAL_UINT8(0xcc, chain[20]);
}

static void test_npot_single_axis_preserves_floor_and_clamped_taps(void)
{
    uint8_t chain[32];
    for (int row = 0; row < 5; ++row)
        memset(chain + row * 4, row * 20, 4);
    TEST_ASSERT_TRUE(jce_texture_mip_fill(chain, sizeof chain, 1, 5));
    for (int channel = 0; channel < 4; ++channel) {
        TEST_ASSERT_EQUAL_UINT8(10, chain[20 + channel]);
        TEST_ASSERT_EQUAL_UINT8(50, chain[24 + channel]);
        TEST_ASSERT_EQUAL_UINT8(30, chain[28 + channel]);
    }
    uint8_t source[12] = {0,0,0,0, 20,20,20,20, 250,250,250,250};
    uint8_t output[4];
    jce_texture_mip_downsample(source, 3, 1, output);
    TEST_ASSERT_EQUAL_UINT8(10, output[0]);
}

static void test_mip_suffix_keeps_original_chain_and_exact_selected_bytes(void)
{
    size_t total, suffix;
    TEST_ASSERT_TRUE(jce_texture_mip_bytes(8, 4, &total));
    TEST_ASSERT_EQUAL_UINT32(172, total);
    uint32_t w = 8, h = 4;
    size_t offset = jce_texture_mip_select(&w, &h, 2);
    TEST_ASSERT_EQUAL_UINT32(160, offset);
    TEST_ASSERT_EQUAL_UINT32(2, w);
    TEST_ASSERT_EQUAL_UINT32(1, h);
    TEST_ASSERT_TRUE(jce_texture_mip_bytes(w, h, &suffix));
    TEST_ASSERT_EQUAL_UINT32(total, offset + suffix);
    w = 8; h = 4;
    TEST_ASSERT_EQUAL_UINT32(0, jce_texture_mip_select(&w, &h, 0));
    TEST_ASSERT_EQUAL_UINT32(8, w);
}

static void test_invalid_or_truncated_chain_writes_nothing(void)
{
    size_t bytes = 123;
    TEST_ASSERT_FALSE(jce_texture_mip_bytes(0, 4, &bytes));
    TEST_ASSERT_FALSE(jce_texture_mip_bytes(UINT32_MAX, UINT32_MAX, &bytes));
    TEST_ASSERT_EQUAL_UINT32(123, bytes);
    uint8_t chain[20];
    memset(chain, 0x77, sizeof chain);
    TEST_ASSERT_FALSE(jce_texture_mip_fill(chain, 19, 2, 2));
    for (unsigned i = 0; i < sizeof chain; ++i) TEST_ASSERT_EQUAL_UINT8(0x77, chain[i]);
    TEST_ASSERT_FALSE(jce_texture_mip_fill(NULL, 20, 2, 2));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rgba_and_alpha_box_average_are_exact);
    RUN_TEST(test_npot_single_axis_preserves_floor_and_clamped_taps);
    RUN_TEST(test_mip_suffix_keeps_original_chain_and_exact_selected_bytes);
    RUN_TEST(test_invalid_or_truncated_chain_writes_nothing);
    return UNITY_END();
}
