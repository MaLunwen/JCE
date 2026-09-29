/* test_jce_ui_canvas_safe_area.c
 *
 * A NOTCH IS A FACT; LAYING OUT INSIDE IT IS A DECISION.
 *
 * Before this, jce_ui_canvas.c built its root rect as {0, 0, screen_w,
 * screen_h} unconditionally and nothing in the tree knew a display could have
 * an unusable border -- a grep for safe_area / notch / display_cutout /
 * SDL_GetWindowSafeArea across engine, editor and scripting returned two
 * irrelevant lines.  On a phone with a cutout the UI drew under it, including
 * the on-screen joystick jce_touch_hud.h auto-creates on exactly those
 * devices.
 *
 * THE ASSERTION THAT MATTERS IS NOT "OPTING IN INSETS THE CANVAS".  It is
 * that opting OUT does not: a full-bleed background, a letterbox bar and a
 * vignette all want the cutout, and a feature that quietly insets every
 * canvas would put an unexplained margin around every existing scene.  So
 * each case here is paired with its opposite in the same fixture.
 *
 * This asks the ENGINE (jce_ui_canvas_root_rect) rather than recomputing the
 * rule -- a test that derives the answer a second way asserts against its own
 * arithmetic, which this tree has paid for.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * The line that stood here said the opposite, and acting on it is why this
 * file sat untracked on a worktree eleven branches share.  Settle it with
 * `git check-ignore -v <path>`, never from memory.
 */
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_ui_canvas.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

/* A 1170x2532 phone with a 47 px status inset and a 34 px home indicator --
 * the shape this feature exists for, not a round number. */
#define SCR_W 1170.0f
#define SCR_H 2532.0f
#define SAFE_X 0
#define SAFE_Y 47
#define SAFE_W 1170
#define SAFE_H (2532 - 47 - 34)

static JceCanvasComponent canvas(bool respect)
{
    JceCanvasComponent c;
    memset(&c, 0, sizeof c);
    c.render_mode = JCE_CANVAS_OVERLAY;
    c.reference_resolution[0] = 1080.0f;
    c.reference_resolution[1] = 1920.0f;
    c.scale_factor = 1.0f;
    c.match_width_or_height = 0.5f;
    c.respect_safe_area = respect;
    return c;
}

static void root_of(const JceCanvasComponent *cv, int sx, int sy, int sw,
                    int sh, float *x, float *y, float *w, float *h)
{
    jce_ui_canvas_root_rect(cv, SCR_W, SCR_H, sx, sy, sw, sh, x, y, w, h);
}

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------------- */

static void test_opting_out_is_the_whole_drawable(void)
{
    /* THE LOAD-BEARING ONE.  Every canvas authored before this field existed
     * has respect_safe_area == false, and must lay out exactly where it did.
     * A safe area IS supplied here, so this proves the flag gates it rather
     * than proving the host forgot to push one. */
    const JceCanvasComponent c = canvas(false);
    float x, y, w, h;
    root_of(&c, SAFE_X, SAFE_Y, SAFE_W, SAFE_H, &x, &y, &w, &h);

    printf("  opted out -> %.0f,%.0f %.0fx%.0f\n", (double)x, (double)y,
           (double)w, (double)h);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, x,
        "a canvas that did not opt in was inset anyway -- every existing "
        "scene would gain an unexplained margin");
    TEST_ASSERT_EQUAL_FLOAT(0.0f, y);
    TEST_ASSERT_EQUAL_FLOAT(SCR_W, w);
    TEST_ASSERT_EQUAL_FLOAT(SCR_H, h);
}

static void test_opting_in_uses_the_safe_rect_verbatim(void)
{
    const JceCanvasComponent c = canvas(true);
    float x, y, w, h;
    root_of(&c, SAFE_X, SAFE_Y, SAFE_W, SAFE_H, &x, &y, &w, &h);

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE((float)SAFE_Y, y,
        "the status-bar inset did not reach the root rect");
    TEST_ASSERT_EQUAL_FLOAT((float)SAFE_X, x);
    TEST_ASSERT_EQUAL_FLOAT((float)SAFE_W, w);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE((float)SAFE_H, h,
        "the home-indicator inset did not reach the root rect");

    /* And the fixture must actually distinguish the two: if the safe area
     * equalled the drawable, the test above would pass on a build that
     * ignored the flag entirely. */
    TEST_ASSERT_TRUE_MESSAGE(h < SCR_H - 1.0f,
        "the fixture's safe area is the whole screen, so nothing here is "
        "about the safe area");
}

static void test_an_empty_safe_rect_falls_back_to_the_drawable(void)
{
    /* A device that reports a degenerate rect must not vanish the UI.  Both
     * halves matter: zero width and zero height each on their own. */
    const JceCanvasComponent c = canvas(true);
    float x, y, w, h;

    root_of(&c, 0, 0, 0, SAFE_H, &x, &y, &w, &h);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(SCR_W, w,
        "a zero-WIDTH safe area collapsed the canvas instead of being ignored");
    TEST_ASSERT_EQUAL_FLOAT(SCR_H, h);

    root_of(&c, 0, 0, SAFE_W, 0, &x, &y, &w, &h);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(SCR_H, h,
        "a zero-HEIGHT safe area collapsed the canvas instead of being ignored");
    TEST_ASSERT_EQUAL_FLOAT(SCR_W, w);

    /* POSITIVE CONTROL: the same opted-in canvas DOES inset for a valid rect,
     * so the two assertions above are about the degenerate input and not
     * about a build where the flag never works. */
    root_of(&c, SAFE_X, SAFE_Y, SAFE_W, SAFE_H, &x, &y, &w, &h);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE((float)SAFE_H, h,
        "positive control failed: this canvas does not inset even for a good "
        "rect, so the fallbacks above prove nothing");
}

static void test_a_null_component_is_the_whole_drawable(void)
{
    /* The renderer reaches this with whatever the scene holds; a caller
     * should never have to test for NULL before asking. */
    float x, y, w, h;
    root_of(NULL, SAFE_X, SAFE_Y, SAFE_W, SAFE_H, &x, &y, &w, &h);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, y);
    TEST_ASSERT_EQUAL_FLOAT(SCR_W, w);
    TEST_ASSERT_EQUAL_FLOAT(SCR_H, h);
}

static void test_a_zeroed_component_does_not_opt_in(void)
{
    /* A struct built by memset -- which is how every deserialiser and every
     * test in this tree starts one -- must mean "full drawable", because that
     * is what a scene file without the key parses to. */
    JceCanvasComponent z;
    memset(&z, 0, sizeof z);
    float x, y, w, h;
    root_of(&z, SAFE_X, SAFE_Y, SAFE_W, SAFE_H, &x, &y, &w, &h);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(SCR_H, h,
        "a zeroed JceCanvasComponent opted into the safe area, so every scene "
        "file written before this key existed would lay out differently");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_opting_out_is_the_whole_drawable);
    RUN_TEST(test_opting_in_uses_the_safe_rect_verbatim);
    RUN_TEST(test_an_empty_safe_rect_falls_back_to_the_drawable);
    RUN_TEST(test_a_null_component_is_the_whole_drawable);
    RUN_TEST(test_a_zeroed_component_does_not_opt_in);
    return UNITY_END();
}
