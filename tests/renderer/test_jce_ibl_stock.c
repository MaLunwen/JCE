/* Stock bytes compared with exact production integration, without a GPU. */
#include "renderer/jce_ibl_brdf.h"
#include "renderer/jce_ibl_stock.h"
#include "unity.h"
#include <stdlib.h>
void setUp(void) {}
void tearDown(void) {}
static void rejects_incomplete_or_wrong_size(void)
{
    uint16_t one = 0;
    TEST_ASSERT_FALSE(jce_ibl_stock_decode(128, &one, 2, jce_ibl_stock_256, jce_ibl_stock_256_size));
    TEST_ASSERT_FALSE(jce_ibl_stock_decode(256, NULL, 524288, jce_ibl_stock_256, jce_ibl_stock_256_size));
    TEST_ASSERT_FALSE(jce_ibl_stock_decode(256, &one, 524288, jce_ibl_stock_256, 524287));
}
static void full_fixed_stock_matches_exact_kernel_physical_values(void)
{
    uint16_t *data = malloc(524288);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_TRUE(jce_ibl_stock_decode(256, data, 524288,
        jce_ibl_stock_256, jce_ibl_stock_256_size));
    uint16_t row[256 * 4];
    for (uint32_t y = 0; y < 256; ++y) {
        jce_ibl_brdf_row(256, y, row);
        for (uint32_t x = 0; x < 256; ++x) {
            size_t i = ((size_t)y * 256 + x) * 4;
            TEST_ASSERT_FLOAT_WITHIN(0.002f, f16_to_f32(row[x * 4]), f16_to_f32(data[i]));
            TEST_ASSERT_FLOAT_WITHIN(0.002f, f16_to_f32(row[x * 4 + 1]), f16_to_f32(data[i + 1]));
            TEST_ASSERT_EQUAL_UINT16(0, data[i + 2]);
            TEST_ASSERT_EQUAL_UINT16(0x3c00, data[i + 3]);
        }
    }
    free(data);
}
static void custom_size_preserves_texel_center_kernel(void)
{
    uint16_t row[4 * 4];
    jce_ibl_brdf_row(4, 2, row);
    float scale, bias;
    integrate_brdf(0.125f, 0.625f, &scale, &bias);
    TEST_ASSERT_EQUAL_UINT16(f32_to_f16(scale), row[0]);
    TEST_ASSERT_EQUAL_UINT16(f32_to_f16(bias), row[1]);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(rejects_incomplete_or_wrong_size);
    RUN_TEST(full_fixed_stock_matches_exact_kernel_physical_values);
    RUN_TEST(custom_size_preserves_texel_center_kernel);
    return UNITY_END();
}
