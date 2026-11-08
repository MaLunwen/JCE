/* Real font joining/order oracle. Font bytes are explicit local input,
 * never redistributed. Missing input is a visible Unity IGNORE, not proof. */
#include "unity.h"
#include "renderer/jce_text_shape.h"
#include <hb.h>
#include <hb-ot.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>
/* Consume this exact production owner once, exposing its private FT/blob
 * state to this native fixture. The linked archive does not compile a second
 * copy here; every public text symbol is supplied by this translation unit. */
#include "renderer/jce_text.c"

static hb_font_t *font;
static JceFont *native_fonts[3];
static JceRenderer *native_renderer;
static JceAtomicI32 *native_fatals;
static void observe_native_fatal(const JceLogRecord *record, void *user)
{
    if (record->level == JCE_LOG_LEVEL_ERROR &&
        strcmp(record->tag, "jce_renderer") == 0 &&
        strncmp(record->message, "bgfx fatal:", 11) == 0)
        jce_atomic_i32_add((JceAtomicI32 *)user, 1);
}
static uint16_t native_texture_count(void)
{
    for (unsigned i=0; i<6; ++i) bgfx_frame(false);
    return bgfx_get_stats()->numTextures;
}
void setUp(void) {}
void tearDown(void)
{
    jce_text_shape_shutdown();
    if (font) hb_font_destroy(font);
    font = NULL;
    for (unsigned i=0; i<3; ++i) {
        if (native_fonts[i]) jce_font_close(native_fonts[i]);
        native_fonts[i] = NULL;
    }
    jce_text_shutdown();
    if (native_renderer) jce_renderer_destroy(native_renderer);
    native_renderer = NULL;
    if (native_fatals) {
        int32_t observed = jce_atomic_i32_load(native_fatals);
        jce_log_set_sink(NULL, NULL);
        jce_atomic_i32_destroy(native_fatals); native_fatals = NULL;
        TEST_ASSERT_EQUAL_INT32_MESSAGE(0, observed, "actual Noop fatal observed");
    }
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
/* Licensed stock bytes are mandatory input, independent of the optional
 * host Arabic font above. Real FT/HB and Noop command delivery establish
 * independent face behavior and byte/texture lifecycle, never GPU pixels. */
static void test_actual_ft_noop_shared_blob_independent_faces_and_reopen(void)
{
    native_fatals = jce_atomic_i32_create(0);
    TEST_ASSERT_NOT_NULL(native_fatals);
    jce_log_set_sink(observe_native_fatal, native_fatals);
    native_renderer = jce_renderer_create_headless();
    TEST_ASSERT_NOT_NULL(native_renderer);
    const uint16_t baseline = native_texture_count();
    native_fonts[0] = jce_font_open_file_ex(JCE_TEST_FONT_PATH, 14, NULL, 0);
    native_fonts[1] = jce_font_open_file_ex(JCE_TEST_FONT_PATH, 28, NULL, 0);
    JceFont *a = native_fonts[0], *b = native_fonts[1];
    TEST_ASSERT_NOT_NULL(a); TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_EQUAL(a, b);
    TEST_ASSERT_NOT_EQUAL(a->ft_face, b->ft_face);
    TEST_ASSERT_NOT_EQUAL(a->hb_font, b->hb_font);
    TEST_ASSERT_EQUAL_PTR(a->font_data, b->font_data);
    TEST_ASSERT_EQUAL_PTR(a->font_data->data, a->ft_face->stream->base);
    TEST_ASSERT_EQUAL_PTR(b->font_data->data, b->ft_face->stream->base);
    TEST_ASSERT_EQUAL_UINT64(2, s_text.references);
    TEST_ASSERT_EQUAL_UINT64(a->font_data->size, s_text.unique_bytes);
    const uint64_t first_identity = a->font_data->serial;
    FT_UInt glyph = FT_Get_Char_Index(a->ft_face, 'A');
    TEST_ASSERT_NOT_EQUAL(0, glyph);
    TEST_ASSERT_EQUAL_INT(0, FT_Load_Glyph(a->ft_face, glyph, FT_LOAD_DEFAULT));
    const FT_Pos first_advance = a->ft_face->glyph->advance.x;
    TEST_ASSERT_EQUAL_INT(0, FT_Load_Glyph(b->ft_face, glyph, FT_LOAD_DEFAULT));
    TEST_ASSERT_GREATER_THAN_INT64(first_advance, b->ft_face->glyph->advance.x);
    TEST_ASSERT_EQUAL_INT(0, FT_Load_Glyph(a->ft_face, glyph, FT_LOAD_DEFAULT));
    TEST_ASSERT_EQUAL_INT64(first_advance, a->ft_face->glyph->advance.x);
    TEST_ASSERT_EQUAL_UINT16(baseline+2, native_texture_count());
    jce_font_close(native_fonts[0]); native_fonts[0] = NULL;
    TEST_ASSERT_EQUAL_UINT64(1, b->font_data->refs);
    TEST_ASSERT_EQUAL_INT(0, FT_Load_Glyph(b->ft_face, glyph, FT_LOAD_DEFAULT));
    TEST_ASSERT_GREATER_THAN_INT64(first_advance, b->ft_face->glyph->advance.x);
    TEST_ASSERT_EQUAL_UINT16(baseline+1, native_texture_count());
    jce_font_close(native_fonts[1]); native_fonts[1] = NULL;
    TEST_ASSERT_NULL(s_text.blobs);
    TEST_ASSERT_EQUAL_UINT64(0, s_text.references);
    TEST_ASSERT_EQUAL_UINT64(0, s_text.unique_bytes);
    TEST_ASSERT_EQUAL_UINT16(baseline, native_texture_count());
    jce_text_shutdown();
    native_fonts[2] = jce_font_open_file_ex(JCE_TEST_FONT_PATH, 14, NULL, 0);
    TEST_ASSERT_NOT_NULL(native_fonts[2]);
    TEST_ASSERT_GREATER_THAN_UINT64(first_identity, native_fonts[2]->font_data->serial);
    TEST_ASSERT_NOT_EQUAL(0, FT_Get_Char_Index(native_fonts[2]->ft_face, 'A'));
    jce_font_close(native_fonts[2]); native_fonts[2] = NULL;
    TEST_ASSERT_NULL(s_text.blobs);
    TEST_ASSERT_EQUAL_UINT16(baseline, native_texture_count());
    puts("ACTUAL_FT_NOOP_BLOB independent_faces=observed shared_raw_bytes=observed glyph_metrics=observed close_reopen=observed texture_baseline=restored");
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_real_arabic_joining_and_latin_order);
    RUN_TEST(test_actual_ft_noop_shared_blob_independent_faces_and_reopen);
    return UNITY_END();
}
