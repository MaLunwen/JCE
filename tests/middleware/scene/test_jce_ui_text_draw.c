/* Exact production canvas text owner with counted measure/draw ports.
 * Verifies submitted scale/positions/markup, not rasterized glyph pixels. */
#include "jce_ui_canvas_text.h"
#include <jce/os/core/jce_str.h>
#include "unity.h"
#include <string.h>

struct JceFont { int px; };
static JceFont face;
static unsigned char canvas_token, renderer_token;
static unsigned lookups, draws, plain_measures, math_measures;
static bool split_shaping;
typedef struct {
    float x, y, scale;
    int mode;
    char text[96];
    const JceRectXform *transform;
} Draw;
static Draw submitted[16];
static JceUICanvas *canvas(void) { return (JceUICanvas *)&canvas_token; }
JceRenderer *uc_renderer(const JceUICanvas *owner)
{
    TEST_ASSERT_EQUAL_PTR(canvas(), owner);
    return (JceRenderer *)&renderer_token;
}
int uc_font_px_clamp(int px)
{
    /* The measured atlas is supplied by this port; atlas ceiling arithmetic
     * belongs to the existing production SDF fixture, not this test. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(4, px);
    TEST_ASSERT_LESS_OR_EQUAL_INT(256, px);
    return px;
}
JceFont *uc_get_font(JceUICanvas *owner, const char *path, int px, bool sdf)
{
    TEST_ASSERT_EQUAL_PTR(canvas(), owner);
    TEST_ASSERT_NOT_NULL(path);
    (void)sdf;
    ++lookups;
    face.px = px;
    return &face;
}
int jce_font_line_height(const JceFont *font) { return font->px; }
const char *jce_loc_t(const char *key) { return key; }
uint32_t uc_color(const float rgba[4], float alpha)
{
    (void)rgba; (void)alpha;
    return 0xffffffffu;
}
void jce_text_measure(const JceFont *font, const char *text, float *w, float *h)
{
    ++plain_measures;
    *w = (float)strlen(text) * (float)font->px;
    /* Joined plain shaping is narrower than two independently styled runs.
     * A fit measured on stripped text would choose 26px instead of 20px. */
    if (split_shaping && strcmp(text, "AB") == 0) *w -= 20.0f;
    *h = (float)font->px;
}
void jce_text_measure_math(const JceFont *font, const char *markup, float *w, float *h)
{
    TEST_ASSERT_EQUAL_STRING("\\frac{A}{B}", markup);
    ++math_measures;
    *w = 2.0f * font->px;
    *h = 2.0f * font->px;
}
static void record(const JceRenderer *renderer, JceFont *font,
                    float x, float y, float scale, const char *text,
                    const JceRectXform *transform, int mode)
{
    TEST_ASSERT_EQUAL_PTR(&renderer_token, renderer);
    TEST_ASSERT_EQUAL_PTR(&face, font);
    TEST_ASSERT_LESS_THAN_UINT(16, draws);
    Draw *draw = &submitted[draws++];
    draw->x = x; draw->y = y; draw->scale = scale;
    draw->mode = mode; draw->transform = transform;
    jce_strlcpy(draw->text, text, sizeof draw->text);
}
void jce_text_draw_scaled_view_xf(const JceRenderer *renderer, JceFont *font,
                                 uint16_t view, float x, float y, float scale,
                                 const char *text, uint32_t color,
                                 const JceRectXform *transform)
{
    (void)view; (void)color;
    record(renderer, font, x, y, scale, text, transform, 1);
}
void jce_text_draw_styled_view_xf(const JceRenderer *renderer, JceFont *font,
                                 uint16_t view, float x, float y, float scale,
                                 const char *text, uint32_t color,
                                 const JceRectXform *transform,
                                 const JceTextStyle *style)
{
    (void)view; (void)color; (void)style;
    record(renderer, font, x, y, scale, text, transform, 0);
}
void jce_text_draw_math_view(const JceRenderer *renderer, JceFont *font,
                            uint16_t view, float x, float y, float scale,
                            const char *markup, uint32_t color)
{
    (void)view; (void)color;
    record(renderer, font, x, y, scale, markup, NULL, 2);
}
void setUp(void)
{
    lookups = draws = plain_measures = math_measures = 0;
    split_shaping = false;
    memset(submitted, 0, sizeof submitted);
}
void tearDown(void) {}
static JceUITextComponent label(const char *text)
{
    JceUITextComponent tx = {0};
    jce_strlcpy(tx.text, text, sizeof tx.text);
    jce_strlcpy(tx.font_path, "fonts/test.ttf", sizeof tx.font_path);
    tx.font_size = 40;
    tx.best_fit = true;
    tx.min_size = 8;
    tx.max_size = 40;
    tx.line_spacing = 1;
    tx.vertical_alignment = JCE_UI_TEXT_VALIGN_TOP;
    tx.color[3] = 1;
    return tx;
}
static void rich_fit_uses_run_width_and_submits_scaled_advances(void)
{
    split_shaping = true;
    JceUITextComponent tx = label("A<color=red>B</color>");
    tx.rich_text = true;
    tx.alignment = JCE_UI_TEXT_ALIGN_RIGHT;
    UCRect rect = {10, 20, 40, 40};
    JceRectXform transform = {0};
    uc_draw_text(canvas(), 1, &rect, &tx, 1, 1, &transform);
    TEST_ASSERT_EQUAL_UINT(2, draws);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, submitted[0].scale);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, submitted[1].scale);
    TEST_ASSERT_EQUAL_FLOAT(10, submitted[0].x);
    TEST_ASSERT_EQUAL_FLOAT(30, submitted[1].x);
    TEST_ASSERT_EQUAL_PTR(&transform, submitted[1].transform);
    TEST_ASSERT_EQUAL_STRING("A", submitted[0].text);
    TEST_ASSERT_EQUAL_STRING("B", submitted[1].text);
    TEST_ASSERT_EQUAL_UINT(1, lookups);
}
static void math_fit_uses_formula_height_and_submits_matching_scale(void)
{
    JceUITextComponent tx = label("\\frac{A}{B}");
    tx.math_text = true;
    UCRect rect = {10, 20, 80, 40};
    uc_draw_text(canvas(), 1, &rect, &tx, 1, 1, NULL);
    TEST_ASSERT_EQUAL_UINT(1, draws);
    TEST_ASSERT_EQUAL_INT(2, submitted[0].mode);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, submitted[0].scale);
    TEST_ASSERT_EQUAL_FLOAT(20, submitted[0].y);
    TEST_ASSERT_EQUAL_UINT(0, plain_measures);
    TEST_ASSERT_GREATER_THAN_UINT(0, math_measures);
}
static void math_multiline_spacing_and_alignment_share_formula_line_boxes(void)
{
    JceUITextComponent tx = label("\\frac{A}{B}\n\\frac{A}{B}");
    tx.math_text = true;
    tx.line_spacing = 1.5f;
    tx.alignment = JCE_UI_TEXT_ALIGN_CENTER;
    UCRect rect = {10, 20, 100, 120};
    uc_draw_text(canvas(), 1, &rect, &tx, 1, 1, NULL);
    TEST_ASSERT_EQUAL_UINT(2, draws);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, submitted[0].scale);
    TEST_ASSERT_EQUAL_FLOAT(40, submitted[0].x);
    TEST_ASSERT_EQUAL_FLOAT(20, submitted[0].y);
    TEST_ASSERT_EQUAL_FLOAT(80, submitted[1].y);
}
static void plain_styled_fit_keeps_its_existing_scaled_submission(void)
{
    JceUITextComponent tx = label("AB");
    UCRect rect = {10, 20, 40, 40};
    uc_draw_text(canvas(), 1, &rect, &tx, 1, 1, NULL);
    TEST_ASSERT_EQUAL_UINT(1, draws);
    TEST_ASSERT_EQUAL_INT(0, submitted[0].mode);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, submitted[0].scale);
}
static void resizing_only_requests_the_upper_atlas_and_updates_draw_scale(void)
{
    JceUITextComponent tx = label("AB");
    UCRect rect = {0, 0, 40, 40};
    for (unsigned i = 0; i < 1000; ++i) {
        draws = 0;
        rect.w = i % 2 ? 80 : 40;
        uc_draw_text(canvas(), 1, &rect, &tx, 1, 1, NULL);
        TEST_ASSERT_EQUAL_INT(40, face.px);
        TEST_ASSERT_EQUAL_UINT(1, draws);
        TEST_ASSERT_EQUAL_FLOAT(i % 2 ? 1.0f : 0.5f, submitted[0].scale);
    }
    TEST_ASSERT_EQUAL_UINT(1000, lookups);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(rich_fit_uses_run_width_and_submits_scaled_advances);
    RUN_TEST(math_fit_uses_formula_height_and_submits_matching_scale);
    RUN_TEST(math_multiline_spacing_and_alignment_share_formula_line_boxes);
    RUN_TEST(plain_styled_fit_keeps_its_existing_scaled_submission);
    RUN_TEST(resizing_only_requests_the_upper_atlas_and_updates_draw_scale);
    return UNITY_END();
}
