/* Real font joining/order oracle. Font bytes are explicit local input,
 * never redistributed. Missing input is a visible Unity IGNORE, not proof. */
#include "unity.h"
#include "renderer/jce_text_shape.h"
#include <hb.h>
#include <hb-ot.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

static hb_font_t *font;
void setUp(void) {}
void tearDown(void)
{
    jce_text_shape_shutdown();
    if (font) hb_font_destroy(font);
    font = NULL;
}
static uint8_t slot(void *ctx, uint32_t cp)
{
    (void)ctx; (void)cp; return 0;
}
static hb_font_t *face_for_slot(void *ctx, uint8_t index)
{
    (void)index; return (hb_font_t *)ctx;
}
static void open_real_font(void)
{
    const char *path = getenv("JCE_TEST_REAL_FONT");
    if (!path || !*path) TEST_IGNORE_MESSAGE("Real font path unavailable; joining NOT VERIFIED");
    hb_blob_t *blob = hb_blob_create_from_file(path);
    TEST_ASSERT_GREATER_THAN_UINT32(0, hb_blob_get_length(blob));
    hb_face_t *face = hb_face_create(blob, 0);
    hb_blob_destroy(blob);
    TEST_ASSERT_GREATER_THAN_UINT32(0, hb_face_get_glyph_count(face));
    font = hb_font_create(face);
    hb_font_set_scale(font, (int)hb_face_get_upem(face), (int)hb_face_get_upem(face));
    hb_face_destroy(face);
    hb_ot_font_set_funcs(font);
}
static void compare_run(const char *text, bool rtl, const uint32_t *cps,
                        unsigned source_count, bool require_joining)
{
    hb_buffer_t *buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, text, -1, 0, -1);
    hb_buffer_guess_segment_properties(buffer);
    TEST_ASSERT_EQUAL_INT(rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR,
                          hb_buffer_get_direction(buffer));
    hb_shape(font, buffer, NULL, 0);
    unsigned count = 0;
    const hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buffer, &count);
    const hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(buffer, NULL);
    TEST_ASSERT_EQUAL_UINT32(source_count, count);
    JceTextFontChain chain = {font, slot, face_for_slot, 0};
    uint32_t actual_count = 0;
    const JceShapedGlyph *run = jce_text_shape(font, &chain, text, &actual_count);
    TEST_ASSERT_NOT_NULL(run);
    TEST_ASSERT_EQUAL_UINT32(count, actual_count);
    unsigned joined = 0;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t source_index = rtl ? source_count - 1u - i : i;
        hb_codepoint_t nominal = 0;
        TEST_ASSERT_TRUE(hb_font_get_nominal_glyph(font, cps[source_index], &nominal));
        TEST_ASSERT_NOT_EQUAL(0, nominal);
        TEST_ASSERT_NOT_EQUAL(0, info[i].codepoint);
        TEST_ASSERT_EQUAL_UINT32(cps[source_index], run[i].codepoint);
        TEST_ASSERT_EQUAL_UINT32(info[i].codepoint, run[i].glyph_id);
        TEST_ASSERT_EQUAL_INT32(positions[i].x_advance, run[i].x_advance);
        TEST_ASSERT_EQUAL_INT32(positions[i].x_offset, run[i].x_offset);
        TEST_ASSERT_EQUAL_INT32(positions[i].y_offset, run[i].y_offset);
        TEST_ASSERT_NOT_EQUAL(0, run[i].x_advance);
        TEST_ASSERT_EQUAL_UINT8(0, run[i].font_slot);
        if (info[i].codepoint != nominal) ++joined;
        if (i) {
            if (rtl) TEST_ASSERT_TRUE(info[i - 1].cluster > info[i].cluster);
            else TEST_ASSERT_TRUE(info[i - 1].cluster < info[i].cluster);
        }
    }
    /* Independent semantic constraint rejects a renderer that merely maps
     * Unicode to nominal glyphs, even if its cache exactly repeats that bug. */
    if (require_joining) TEST_ASSERT_GREATER_OR_EQUAL_UINT32(2, joined);
    hb_buffer_destroy(buffer);
}
static void test_real_arabic_joining_and_latin_order(void)
{
    open_real_font();
    const uint32_t arabic[] = {0x0628, 0x0628, 0x0628};
    const uint32_t latin[] = {'H', 'i', 'W'};
    const uint32_t distinct_arabic[] = {0x0628, 0x062a, 0x062c};
    compare_run("\xd8\xa8\xd8\xa8\xd8\xa8", true, arabic, 3, true);
    compare_run("HiW", false, latin, 3, false);
    compare_run("\xd8\xa8\xd8\xaa\xd8\xac", true, distinct_arabic, 3, true);
    compare_run("\xd8\xa8\xd8\xa8\xd8\xa8", true, arabic, 3, true);
    compare_run("\xd8\xa8\xd8\xaa\xd8\xac", true, distinct_arabic, 3, true);
    printf("REAL_FONT_ORACLE joining=observed rtl=observed latin=observed cache=observed\n");
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_real_arabic_joining_and_latin_order);
    return UNITY_END();
}
