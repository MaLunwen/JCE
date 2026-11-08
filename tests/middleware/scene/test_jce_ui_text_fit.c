#include "jce_ui_text_fit.h"
#include "unity.h"
#include <math.h>

void setUp(void) {}
void tearDown(void) {}
static void multiline_height_and_spacing_limit_the_same_draw_scale(void)
{
    /* The canvas supplies the actual full block measured with its spacing. */
    int px = uc_text_fit_size(8, 40, 40, 200.0f, 96.0f, 180.0f, 48.0f);
    TEST_ASSERT_EQUAL_INT(20, px);
    float scale = (float)px / 40.0f;
    TEST_ASSERT_EQUAL_FLOAT(100.0f, 200.0f * scale);
    TEST_ASSERT_EQUAL_FLOAT(48.0f, 96.0f * scale);
}
static void width_limit_minimum_and_magnification_are_preserved(void)
{
    TEST_ASSERT_EQUAL_INT(25, uc_text_fit_size(8, 40, 40, 200, 40, 125, 100));
    TEST_ASSERT_EQUAL_INT(8, uc_text_fit_size(8, 40, 40, 200, 40, 1, 1));
    TEST_ASSERT_EQUAL_INT(400, uc_text_fit_size(4, 400, 256, 256, 256, 400, 400));
    TEST_ASSERT_EQUAL_INT(25, uc_text_fit_size(25, 5, 40, 200, 40, 1000, 1000));
}
static void resizing_uses_fixed_measured_metrics_without_font_creation(void)
{
    for (unsigned repeat = 0; repeat < 1000; ++repeat) {
        TEST_ASSERT_EQUAL_INT(20, uc_text_fit_size(8, 40, 40, 200, 96, 180, 48));
        TEST_ASSERT_EQUAL_INT(40, uc_text_fit_size(8, 40, 40, 200, 96, 400, 192));
        TEST_ASSERT_EQUAL_INT(8, uc_text_fit_size(8, 40, 40, 200, 96, 40, 48));
    }
}
static void invalid_measurements_do_not_select_an_oversized_label(void)
{
    TEST_ASSERT_EQUAL_INT(8, uc_text_fit_size(8, 40, 40, NAN, 20, 100, 100));
    TEST_ASSERT_EQUAL_INT(8, uc_text_fit_size(8, 40, 40, 20, INFINITY, 100, 100));
    TEST_ASSERT_EQUAL_INT(8, uc_text_fit_size(8, 40, 40, 20, 20, NAN, 100));
    TEST_ASSERT_EQUAL_INT(8, uc_text_fit_size(8, 40, 0, 20, 20, 100, 100));
    TEST_ASSERT_EQUAL_INT(8, uc_text_fit_size(8, 40, 40, -1, 20, 100, 100));
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(multiline_height_and_spacing_limit_the_same_draw_scale);
    RUN_TEST(width_limit_minimum_and_magnification_are_preserved);
    RUN_TEST(resizing_uses_fixed_measured_metrics_without_font_creation);
    RUN_TEST(invalid_measurements_do_not_select_an_oversized_label);
    return UNITY_END();
}
