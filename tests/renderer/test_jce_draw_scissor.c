/*
 * test_jce_draw_scissor.c — the 2D scissor every rect draw obeys.
 *
 * WHY THIS EXISTS AT ALL.  The scissor state used to live in
 * jce_ui_canvas.c, armed by two macros wrapped around the two rect helpers.
 * That clipped quads and left TEXT unclipped, because
 * jce_text_draw_scaled_view submits ONE DRAW PER GLYPH from inside
 * jce_text.c, where a macro in the canvas cannot reach — so scrolled text ran
 * outside its viewport and no care at the canvas's call sites could have
 * fixed it.  Moving the state into jce_primitives.c, which owns every submit,
 * is what fixed it; this file gates the contract that move created.
 *
 * WHAT IT CANNOT CHECK, said here rather than implied: whether bgfx actually
 * clipped a pixel.  That needs a GPU and a capture.  What it does check is
 * the half that was wrong for a year — WHICH RECTANGLE the engine asks for,
 * and whether it asks at all.
 */

#include <jce/renderer/jce_primitives.h>

#include "unity.h"

void setUp(void)    { jce_draw_clear_scissor(); }
void tearDown(void) { jce_draw_clear_scissor(); }

/* ── 1. set / get / clear ────────────────────────────────────────────── */
static void test_a_scissor_is_absent_until_it_is_set(void)
{
    int box[4] = { -1, -1, -1, -1 };
    TEST_ASSERT_FALSE_MESSAGE(jce_draw_get_scissor(box),
        "a fresh renderer must report NO scissor, or every draw before the "
        "first widget is clipped to whatever the last frame left behind");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, box[0],
        "get must not write into out when it answers false: a caller that "
        "checks the return value would still be reading a stale rectangle if "
        "it did");

    jce_draw_set_scissor(10, 20, 30, 40);
    TEST_ASSERT_TRUE(jce_draw_get_scissor(box));
    TEST_ASSERT_EQUAL_INT(10, box[0]);
    TEST_ASSERT_EQUAL_INT(20, box[1]);
    TEST_ASSERT_EQUAL_INT(30, box[2]);
    TEST_ASSERT_EQUAL_INT(40, box[3]);

    jce_draw_clear_scissor();
    TEST_ASSERT_FALSE(jce_draw_get_scissor(box));
}

/* ── 2. an empty rect CLEARS, it does not blank the frame ────────────── */
static void test_an_empty_rect_clears_rather_than_clipping_everything(void)
{
    int box[4];
    jce_draw_set_scissor(10, 10, 100, 100);
    TEST_ASSERT_TRUE(jce_draw_get_scissor(box));

    jce_draw_set_scissor(10, 10, 0, 100);
    TEST_ASSERT_FALSE_MESSAGE(jce_draw_get_scissor(box),
        "a zero-width rect must CLEAR: a widget whose rect collapsed would "
        "otherwise scissor the whole rest of the frame away, and a blank "
        "screen is a much worse symptom than an unclipped widget");

    jce_draw_set_scissor(10, 10, 100, -5);
    TEST_ASSERT_FALSE(jce_draw_get_scissor(box));
}

/* ── 3. negative origins are clamped, keeping the far edge put ───────── */
static void test_a_negative_origin_is_clamped_without_moving_the_far_edge(void)
{
    int box[4];
    /* bgfx takes UNSIGNED coordinates, so a rect that starts off-screen must
     * be clamped before the cast — casting -20 to uint16_t gives 65516 and
     * the scissor lands somewhere else entirely.  The far edge must not move
     * while the near one is clamped: x=-20,w=100 covers 0..80, not 0..100. */
    jce_draw_set_scissor(-20, -5, 100, 50);
    TEST_ASSERT_TRUE(jce_draw_get_scissor(box));
    TEST_ASSERT_EQUAL_INT(0, box[0]);
    TEST_ASSERT_EQUAL_INT(0, box[1]);
    TEST_ASSERT_EQUAL_INT_MESSAGE(80, box[2],
        "the right edge moved: clamping x without shrinking w widens the clip");
    TEST_ASSERT_EQUAL_INT_MESSAGE(45, box[3],
        "the bottom edge moved");
}

/* ── 4. a rect entirely off-screen clears ────────────────────────────── */
static void test_a_rect_entirely_off_screen_clears(void)
{
    int box[4];
    jce_draw_set_scissor(10, 10, 100, 100);
    /* x=-100 w=50 ends at -50: nothing of it is on screen.  Clamping alone
     * would leave w=-50, and the guard that catches THAT is the same one an
     * empty rect hits — checked here because the two arrive by different
     * paths and a fix to one can miss the other. */
    jce_draw_set_scissor(-100, 10, 50, 100);
    TEST_ASSERT_FALSE(jce_draw_get_scissor(box));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_scissor_is_absent_until_it_is_set);
    RUN_TEST(test_an_empty_rect_clears_rather_than_clipping_everything);
    RUN_TEST(test_a_negative_origin_is_clamped_without_moving_the_far_edge);
    RUN_TEST(test_a_rect_entirely_off_screen_clears);
    return UNITY_END();
}
