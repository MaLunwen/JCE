#include "jce_water_gradient.h"
#include "os/core/jce_half_internal.h"
#include <jce/os/core/jce_math.h>
#include "unity.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_gradient_mips_preserve_mean_and_cancel_high_frequency(void)
{
    float x[16], z[16], second_x[16], second_z[16], packed[64];
    for (int y = 0; y < 4; ++y) for (int col = 0; col < 4; ++col) {
        const int i = y*4 + col;
        x[i] = ((y+col)&1) ? -2.0f : 2.0f;
        z[i] = 3.0f; second_x[i] = (float)i; second_z[i] = -4.0f;
    }
    TEST_ASSERT_TRUE(jce_water_gradient_pack(packed, 64, 4, x, z, second_x, second_z));
    TEST_ASSERT_TRUE(jce_water_gradient_reduce(packed, 64, 4));
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_FLOAT(0.0f, packed[4*i]);
        TEST_ASSERT_EQUAL_FLOAT(3.0f, packed[4*i+1]);
        TEST_ASSERT_EQUAL_FLOAT(-4.0f, packed[4*i+3]);
    }
    TEST_ASSERT_TRUE(jce_water_gradient_reduce(packed, 64, 2));
    TEST_ASSERT_EQUAL_FLOAT(7.5f, packed[2]);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, packed[1]);
}

static void test_gradient_invalid_input_does_not_publish_partial_pack(void)
{
    float x[4] = {1,2,3,4}, z[4] = {5,6,7,8}, packed[16], before[16];
    for (int i = 0; i < 16; ++i) packed[i] = before[i] = 19.0f;
    TEST_ASSERT_FALSE(jce_water_gradient_pack(packed, 15, 2, x, z, NULL, NULL));
    TEST_ASSERT_FALSE(jce_water_gradient_pack(packed, 16, 2, x, z, x, NULL));
    x[3] = 70000.0f;
    TEST_ASSERT_FALSE(jce_water_gradient_pack(packed, 16, 2, x, z, NULL, NULL));
    TEST_ASSERT_EQUAL_MEMORY(before, packed, sizeof(before));
    x[3] = 4.0f;
    TEST_ASSERT_TRUE(jce_water_gradient_pack(packed, 16, 2, x, z, NULL, NULL));
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_FLOAT(0.0f, packed[4*i+2]);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, packed[4*i+3]);
    }
}

static void test_shared_half_encoder_rounds_to_even(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x3c00u, jce_float_to_half(1.0f));
    TEST_ASSERT_EQUAL_HEX16(0xbc00u, jce_float_to_half(-1.0f));
    TEST_ASSERT_EQUAL_HEX16(0x3c00u, jce_float_to_half(1.0f + 1.0f/2048.0f));
    TEST_ASSERT_EQUAL_HEX16(0x3c02u, jce_float_to_half(1.0f + 3.0f/2048.0f));
    TEST_ASSERT_EQUAL_HEX16(1u, jce_float_to_half(1.0f/16777216.0f));
    TEST_ASSERT_EQUAL_FLOAT(65504.0f, jce_half_to_float(jce_float_to_half(65504.0f)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_gradient_mips_preserve_mean_and_cancel_high_frequency);
    RUN_TEST(test_gradient_invalid_input_does_not_publish_partial_pack);
    RUN_TEST(test_shared_half_encoder_rounds_to_even);
    return UNITY_END();
}
