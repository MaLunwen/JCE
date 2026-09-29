/* test_jce_ui_child_capacity.c
 *
 * A node's UI children past the 128th were never drawn, laid out, or
 * clickable -- and nothing said so.
 *
 * The walk read the TRUE child count and then gathered at most 128 into stack
 * arrays, and jce_scene_get_children truncates silently (it drains the
 * iterator but stops writing).  Every consumer downstream reads that gathered
 * array: the draw, the LayoutGroup packing, the scissor stack and the raycast
 * hit list.  So this was not a limit on ARRANGEMENT, it was a limit on the ECS
 * UI subtree -- and 128 is a count a real screen reaches (an inventory, a
 * server browser, a chat log), which is precisely where a UI toolkit is
 * expected to hold.
 *
 * The assertions drive the REAL headless canvas (renderer == NULL: layout,
 * raycast and the button state machine all run, only GPU draws are skipped)
 * and check the thing a user would notice -- can I click it -- rather than an
 * internal count, because a test of the count would have stayed green if the
 * gather were fixed and the raycast list were not.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_ui_canvas.h>
#include <jce/renderer/jce_primitives.h>

#include "unity.h"
#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define SCREEN_W  400.0f
#define SCREEN_H  400.0f
#define ROW_H      10.0f

/* One button per row, stacked down the screen by a VERTICAL LayoutGroup, so
 * each child's rect is decided by the packing rather than by its own anchors
 * -- the same path a real list uses. */
static JceEntity g_canvas;   /* the parent make_list built, for probe order */

static JceScene *make_list(int count, JceEntity *out_first, JceEntity *out_last)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity canvas = jce_scene_create_entity(s, "Canvas");
    JceCanvasComponent cv;
    memset(&cv, 0, sizeof cv);
    cv.render_mode = JCE_CANVAS_OVERLAY;
    jce_scene_set_canvas(s, canvas, &cv);

    JceLayoutGroupComponent lg;
    memset(&lg, 0, sizeof lg);
    lg.layout_kind = JCE_LAYOUT_VERTICAL;
    lg.control_child_size_w = true;   /* full width, so x is never the reason */
    jce_scene_set_layout_group(s, canvas, &lg);
    g_canvas = canvas;

    for (int i = 0; i < count; i++) {
        char nm[32];
        snprintf(nm, sizeof nm, "Row%d", i);
        JceEntity b = jce_scene_create_entity(s, nm);
        jce_scene_set_parent(s, b, canvas);

        JceUIButtonComponent btn;
        memset(&btn, 0, sizeof btn);
        btn.interactable = true;
        btn.normal_color[3] = btn.highlighted_color[3] = 1.0f;
        btn.pressed_color[3] = btn.disabled_color[3] = 1.0f;
        /* Top-left anchored, fixed height: the group decides y. */
        btn.rect.anchor_min[0] = 0.0f; btn.rect.anchor_min[1] = 1.0f;
        btn.rect.anchor_max[0] = 0.0f; btn.rect.anchor_max[1] = 1.0f;
        btn.rect.pivot[0]      = 0.0f; btn.rect.pivot[1]      = 1.0f;
        btn.rect.size_delta[0] = SCREEN_W;
        btn.rect.size_delta[1] = ROW_H;
        jce_scene_set_ui_button(s, b, &btn);

        if (i == 0 && out_first)         *out_first = b;
        if (i == count - 1 && out_last)  *out_last  = b;
    }
    return s;
}

/* Press and release at (x,y) through the real pipeline; returns what the
 * canvas reports as clicked. */
static uint64_t click_at(JceUICanvas *uc, JceScene *s, float x, float y)
{
    JceUIPointer p;
    p.x = x; p.y = y; p.valid = true;

    p.down = false;
    jce_ui_canvas_render(uc, s, 0, 0, SCREEN_W, SCREEN_H, &p, 1.0f / 60.0f);
    p.down = true;
    jce_ui_canvas_render(uc, s, 0, 0, SCREEN_W, SCREEN_H, &p, 1.0f / 60.0f);
    p.down = false;
    jce_ui_canvas_render(uc, s, 0, 0, SCREEN_W, SCREEN_H, &p, 1.0f / 60.0f);
    return jce_ui_canvas_last_clicked(uc);
}

/* The row index'th button's centre, given the vertical group's packing. */
static float row_center_y(int index)
{
    return (float)index * ROW_H + ROW_H * 0.5f;
}

/* ── 1. the case that used to vanish ──────────────────────────────────── */
static void test_the_child_past_the_old_cap_is_clickable(void)
{
    /* 40 rows past the old 128 wall, and the last one is what is asserted:
     * a fix that raised the constant instead of removing it would fail here
     * for a large enough list, and a fix that gathered more but left the
     * raycast list short would fail here too. */
    enum { N = 168 };
    JceEntity first = 0, last = 0;
    JceScene *s = make_list(N, &first, &last);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    const uint64_t got = click_at(uc, s, SCREEN_W * 0.5f, row_center_y(N - 1));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)last, got,
        "the last row of a 168-row list was not clickable -- the children past "
        "the 128th were never gathered, so they were never hit-tested");

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 2. the negative control ──────────────────────────────────────────── */
static void test_a_list_that_fits_still_works(void)
{
    /* Under the old cap: this passed before the change and must still pass.
     * Growing on the heap only when a node has more than 128 children means
     * the common case takes a different branch, and a change that only ever
     * exercises the new branch has not shown the old one survived. */
    enum { N = 12 };
    JceEntity first = 0, last = 0;
    JceScene *s = make_list(N, &first, &last);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        (uint64_t)first, click_at(uc, s, SCREEN_W * 0.5f, row_center_y(0)),
        "the first row of a short list must still be clickable");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        (uint64_t)last, click_at(uc, s, SCREEN_W * 0.5f, row_center_y(N - 1)),
        "the last row of a short list must still be clickable");

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 3. exactly at the boundary ───────────────────────────────────────── */
static void test_the_128th_and_129th_both_work(void)
{
    /* The off-by-one that a "> vs >=" slip would leave: 128 fits the stack
     * arrays exactly, 129 is the first that must grow. */
    for (int n = 128; n <= 129; n++) {
        JceEntity last = 0;
        JceScene *s = make_list(n, NULL, &last);
        JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
        TEST_ASSERT_NOT_NULL(uc);
        char msg[96];
        snprintf(msg, sizeof msg,
                 "the last row of a %d-row list was not clickable", n);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(
            (uint64_t)last,
            click_at(uc, s, SCREEN_W * 0.5f, row_center_y(n - 1)), msg);
        jce_ui_canvas_destroy(uc);
        jce_scene_destroy(s);
    }
}

/* ── 4. the rows in between are not disturbed ─────────────────────────── */
static void test_a_middle_row_of_a_long_list_is_still_itself(void)
{
    /* Guards the arithmetic of the heap path: three arrays carved out of one
     * block, so a wrong stride would still let the LAST row work by accident
     * while scrambling the middle. */
    enum { N = 200, PROBE = 150 };
    JceScene *s = make_list(N, NULL, NULL);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* Ask for the children in the SAME order the canvas walk does, rather
     * than assuming creation order equals iteration order. */
    JceEntity kids[N];
    const int n = jce_scene_get_children(s, g_canvas, kids, N);
    TEST_ASSERT_EQUAL_INT_MESSAGE(N, n,
        "jce_scene_get_children must report every child when the buffer fits");

    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        (uint64_t)kids[PROBE],
        click_at(uc, s, SCREEN_W * 0.5f, row_center_y(PROBE)),
        "a middle row of a 200-row list resolved to the wrong entity");

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_child_past_the_old_cap_is_clickable);
    RUN_TEST(test_a_list_that_fits_still_works);
    RUN_TEST(test_the_128th_and_129th_both_work);
    RUN_TEST(test_a_middle_row_of_a_long_list_is_still_itself);
    return UNITY_END();
}
