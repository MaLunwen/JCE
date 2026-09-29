/* test_jce_ui_dropdown.c
 *
 * Expandable option selector — UI Dropdown (+ a read-only ProgressBar smoke).
 *
 * Drives the REAL headless UI canvas pipeline (jce_ui_canvas_render on a canvas
 * created with renderer == NULL) against an authored JceScene that carries a
 * Canvas + a UIDropdown at a KNOWN screen rect, feeds a synthetic JceUIPointer
 * across frames, and asserts the canvas MUTATES the scene component in place
 * (Unity source-of-truth model, exactly like UISlider.value / UIToggle.is_on):
 *   - a click (press-inside → release-over) on the collapsed main rect toggles
 *     `expanded`;
 *   - while expanded, a click on an option row sets `selected_index` and
 *     collapses (`expanded` → false);
 *   - a click OUTSIDE the dropdown while expanded collapses it without changing
 *     the selection (mirrors the InputField defocus-on-outside-click pattern);
 *   - a non-interactable dropdown never expands.
 *
 * The full layout + raycast + expand/select state machine runs with no live
 * bgfx context (renderer NULL) — only the GPU draw calls are skipped — so the
 * interaction logic is exercised exactly as it runs in a shipped game.  Mirrors
 * the harness of test_jce_ui_widgets.c / test_jce_ui_inputfield.c.
 *
 * ProgressBar is render-only (NOT a raycast target — gameplay drives its
 * `value`), so it has no interaction to unit-test; test_progress_bar_smoke just
 * authors one, renders headless (must not crash), and round-trips its `value`
 * through the mutable accessor.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_ui_canvas.h>

#include "unity.h"
#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Shared scene geometry ─────────────────────────────────────────────
 * A 200x200 screen.  The dropdown's MAIN rect is the top 200x40 strip at
 * (0,0)..(200,40) (fixed-size, top-left-anchored RectTransform, so
 * uc_resolve_rect places it deterministically).  The expanded option rows are
 * stacked one main-height (40) tall directly BELOW the main rect (see
 * uc_dd_row_rect): row 0 = (0,40)..(200,80), row 1 = (0,80)..(200,120),
 * row 2 = (0,120)..(200,160).  An "outside" point at (190,190) is below all
 * rows, within the screen, and outside the main rect. */
#define SCREEN_W   200.0f
#define SCREEN_H   200.0f
#define MAIN_H     40.0f

/* Set a fixed-size, top-left-anchored RectTransform of (w,h) at (0,0). */
static void set_top_rect(JceRectTransform *rt, float w, float h)
{
    memset(rt, 0, sizeof *rt);
    /* UGUI: anchor + pivot at the TOP-left, anchoredPosition zero => the
     * element's top edge sits on the parent's top edge.  (anchor/pivot {0,0}
     * would be the BOTTOM-left corner and put this row off the bottom of the
     * screen, taking its popup with it.) */
    rt->anchor_min[0] = 0.0f; rt->anchor_min[1] = 1.0f;
    rt->anchor_max[0] = 0.0f; rt->anchor_max[1] = 1.0f;
    rt->pivot[0]      = 0.0f; rt->pivot[1]      = 1.0f;
    rt->anchored_position[0] = 0.0f; rt->anchored_position[1] = 0.0f;
    rt->size_delta[0] = w; rt->size_delta[1] = h;
}

static JceScene *make_canvas(JceEntity *out_canvas)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity canvas = jce_scene_create_entity(s, "Canvas");
    JceCanvasComponent cv;
    memset(&cv, 0, sizeof cv);
    cv.render_mode = JCE_CANVAS_OVERLAY;
    cv.sort_order  = 0;
    jce_scene_set_canvas(s, canvas, &cv);

    *out_canvas = canvas;
    return s;
}

/* Author a Canvas + a child Dropdown with `n` options, the given selection /
 * expanded / interactable state. */
static JceScene *make_scene_with_dropdown(JceEntity *out_dd, int option_count,
                                          int selected_index, bool expanded,
                                          bool interactable)
{
    JceEntity canvas = 0;
    JceScene *s = make_canvas(&canvas);

    JceEntity dd = jce_scene_create_entity(s, "Dropdown");
    jce_scene_set_parent(s, dd, canvas);

    JceUIDropdownComponent d;
    memset(&d, 0, sizeof d);
    static const char *labels[] = { "Alpha", "Beta", "Gamma", "Delta" };
    if (option_count < 0) option_count = 0;
    if (option_count > JCE_UI_DROPDOWN_MAX_OPTIONS) option_count = JCE_UI_DROPDOWN_MAX_OPTIONS;
    for (int i = 0; i < option_count; i++)
        snprintf(d.options[i], sizeof d.options[0], "%s", labels[i % 4]);
    d.option_count   = option_count;
    d.selected_index = selected_index;
    d.expanded       = expanded;
    d.interactable   = interactable;
    d.bg_color[3] = d.text_color[3] = d.popup_color[3] = d.highlight_color[3] = 1.0f;
    d.font_size = 16.0f;
    set_top_rect(&d.rect, SCREEN_W, MAIN_H);
    jce_scene_set_ui_dropdown(s, dd, &d);
    TEST_ASSERT_TRUE(jce_scene_has_ui_dropdown(s, dd));

    *out_dd = dd;
    return s;
}

/* Drive one render frame through the REAL canvas pipeline. */
static void step(JceUICanvas *uc, JceScene *s, float x, float y,
                 bool down, bool valid)
{
    JceUIPointer ptr;
    ptr.x = x; ptr.y = y; ptr.down = down; ptr.valid = valid;
    jce_ui_canvas_render(uc, s, /*view*/0, /*fb*/0, SCREEN_W, SCREEN_H,
                         valid ? &ptr : NULL, 1.0f / 60.0f);
}

/* Press + release over (x,y) — a complete click (press-inside → release-over). */
static void click_at(JceUICanvas *uc, JceScene *s, float x, float y)
{
    step(uc, s, x, y, true,  true);   /* press   */
    step(uc, s, x, y, false, true);   /* release */
}

static JceUIDropdownComponent *dd_of(JceScene *s, JceEntity e)
{
    JceUIDropdownComponent *d = jce_scene_get_ui_dropdown(s, e);
    TEST_ASSERT_NOT_NULL(d);
    return d;
}

/* The centre of the MAIN (collapsed) rect. */
static const float MAIN_CX = SCREEN_W * 0.5f;
static const float MAIN_CY = MAIN_H   * 0.5f;          /* 20 */

/* The centre of expanded option ROW i: y = main->h*(i+1) + main->h*0.5. */
static float row_cy(int i) { return MAIN_H * (float)(i + 1) + MAIN_H * 0.5f; }

/* ── 1. click expands ─────────────────────────────────────────────────── */
static void test_click_expands(void)
{
    JceEntity dd = 0;
    JceScene *s = make_scene_with_dropdown(&dd, /*opts*/3, /*sel*/0,
                                           /*expanded*/false, /*inter*/true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    TEST_ASSERT_FALSE(dd_of(s, dd)->expanded);

    /* A press over the main rect is not enough — only release-over commits. */
    step(uc, s, MAIN_CX, MAIN_CY, true, true);
    TEST_ASSERT_FALSE(dd_of(s, dd)->expanded);

    /* Release over the main rect → toggles expanded to true. */
    step(uc, s, MAIN_CX, MAIN_CY, false, true);
    TEST_ASSERT_TRUE(dd_of(s, dd)->expanded);

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 2. click option selects (and collapses) ──────────────────────────── */
static void test_click_option_selects(void)
{
    JceEntity dd = 0;
    JceScene *s = make_scene_with_dropdown(&dd, /*opts*/3, /*sel*/0,
                                           /*expanded*/true, /*inter*/true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    TEST_ASSERT_TRUE(dd_of(s, dd)->expanded);
    TEST_ASSERT_EQUAL_INT(0, dd_of(s, dd)->selected_index);

    /* Click option row index 1 → selected_index == 1 AND collapses. */
    click_at(uc, s, MAIN_CX, row_cy(1));
    TEST_ASSERT_EQUAL_INT(1, dd_of(s, dd)->selected_index);
    TEST_ASSERT_FALSE(dd_of(s, dd)->expanded);

    /* The selected option text is reachable through the mutable accessor. */
    TEST_ASSERT_EQUAL_STRING("Beta",
        dd_of(s, dd)->options[dd_of(s, dd)->selected_index]);

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 3. click outside collapses (selection unchanged) ─────────────────── */
static void test_click_outside_collapses(void)
{
    JceEntity dd = 0;
    JceScene *s = make_scene_with_dropdown(&dd, /*opts*/3, /*sel*/2,
                                           /*expanded*/true, /*inter*/true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    TEST_ASSERT_TRUE(dd_of(s, dd)->expanded);

    /* Click well below all option rows (outside the main rect + every row). */
    click_at(uc, s, SCREEN_W - 10.0f, SCREEN_H - 10.0f);
    TEST_ASSERT_FALSE(dd_of(s, dd)->expanded);
    TEST_ASSERT_EQUAL_INT(2, dd_of(s, dd)->selected_index);   /* unchanged */

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 4. non-interactable never expands ────────────────────────────────── */
static void test_non_interactable(void)
{
    JceEntity dd = 0;
    JceScene *s = make_scene_with_dropdown(&dd, /*opts*/3, /*sel*/0,
                                           /*expanded*/false, /*inter*/false);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* Clicking the main rect does NOT expand a non-interactable dropdown. */
    click_at(uc, s, MAIN_CX, MAIN_CY);
    TEST_ASSERT_FALSE(dd_of(s, dd)->expanded);
    TEST_ASSERT_EQUAL_INT(0, dd_of(s, dd)->selected_index);

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 5. selection resolves to a valid in-range index ──────────────────── */
static void test_selected_index_in_range(void)
{
    JceEntity dd = 0;
    JceScene *s = make_scene_with_dropdown(&dd, /*opts*/3, /*sel*/0,
                                           /*expanded*/true, /*inter*/true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* Click the LAST valid option row (index 2). */
    click_at(uc, s, MAIN_CX, row_cy(2));
    int sel = dd_of(s, dd)->selected_index;
    TEST_ASSERT_TRUE(sel >= 0 && sel < dd_of(s, dd)->option_count); /* in-range */
    TEST_ASSERT_EQUAL_INT(2, sel);
    TEST_ASSERT_EQUAL_STRING("Gamma", dd_of(s, dd)->options[sel]);

    /* A click on a row BEYOND option_count (e.g. a 4th row when there are 3
     * options) is not a valid target → no selection change, stays collapsed
     * after the option-2 click above. */
    TEST_ASSERT_FALSE(dd_of(s, dd)->expanded);

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── ProgressBar smoke: author + headless render does not crash + round-trip ─
 * ProgressBar is render-only (not interactive / not a raycast target), so there
 * is no interaction to drive.  This just proves a ProgressBar component can be
 * authored, rendered headless without crashing, and round-trips its `value`
 * through the mutable accessor. */
static void test_progress_bar_smoke(void)
{
    JceEntity canvas = 0;
    JceScene *s = make_canvas(&canvas);

    JceEntity pbE = jce_scene_create_entity(s, "ProgressBar");
    jce_scene_set_parent(s, pbE, canvas);

    JceUIProgressBarComponent pb;
    memset(&pb, 0, sizeof pb);
    pb.value     = 0.5f;
    pb.min_value = 0.0f;
    pb.max_value = 1.0f;
    pb.direction = 0;  /* L→R */
    pb.bg_color[3] = pb.fill_color[3] = 1.0f;
    set_top_rect(&pb.rect, SCREEN_W, MAIN_H);
    jce_scene_set_ui_progress_bar(s, pbE, &pb);
    TEST_ASSERT_TRUE(jce_scene_has_ui_progress_bar(s, pbE));

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* A headless render frame (renderer NULL) must not crash. */
    step(uc, s, 0.0f, 0.0f, false, false);

    JceUIProgressBarComponent *got = jce_scene_get_ui_progress_bar(s, pbE);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, got->value);   /* value round-trips */

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 7. an UNREACHABLE expanded popup must not stay open ───────────────
 *
 * A dropdown whose inherited CanvasGroup stops blocking raycasts is never
 * visited by the interaction pass (that pass walks the hit list, and a
 * non-blocking node records no hit).  It could therefore never be told to
 * collapse -- while still drawing, and still publishing its popup as the
 * frame's MODAL rect.  The result was strictly worse than an unclickable
 * dropdown: a permanent input dead-zone over whatever sat underneath,
 * unreachable itself and blocking everything else.
 *
 * A sibling button is parked under option row 1 so the test asserts the
 * consequence (the pointer reaches what is underneath) and not just the flag. */
static void test_unreachable_expanded_popup_collapses(void)
{
    JceEntity dd = 0;
    JceScene *s = make_scene_with_dropdown(&dd, 4, 0, /*expanded*/true,
                                           /*interactable*/true);
    JceEntity canvas = jce_scene_get_parent(s, dd);

    /* The dropdown's OWN group: the pointer passes through it, but the rest of
     * the canvas -- the button below -- stays reachable. */
    JceCanvasGroupComponent cg;
    memset(&cg, 0, sizeof cg);
    cg.alpha           = 1.0f;
    cg.interactable    = true;
    cg.blocks_raycasts = false;
    jce_scene_set_canvas_group(s, dd, &cg);

    /* A button occupying exactly option row 1 (y = 80..120), i.e. inside what
     * the popup would claim as its modal rect. */
    JceEntity btn = jce_scene_create_entity(s, "Under");
    jce_scene_set_parent(s, btn, canvas);
    JceUIButtonComponent b;
    memset(&b, 0, sizeof b);
    b.interactable = true;
    b.normal_color[3] = b.highlighted_color[3] = b.pressed_color[3] = 1.0f;
    set_top_rect(&b.rect, SCREEN_W, MAIN_H);
    b.rect.anchored_position[1] = -(MAIN_H * 2.0f);   /* down two rows */
    jce_scene_set_ui_button(s, btn, &b);

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    step(uc, s, 0.0f, 0.0f, false, false);            /* one frame, no pointer */
    TEST_ASSERT_FALSE_MESSAGE(dd_of(s, dd)->expanded,
        "an expanded popup that the pointer cannot reach stayed open");

    /* And the button underneath is clickable, which is the point. */
    click_at(uc, s, MAIN_CX, row_cy(1));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)btn,
        jce_ui_canvas_last_clicked(uc),
        "the stuck-open popup was still blocking the pointer beneath it");

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 8. a popup that would overflow the bottom flips above the control ──
 *
 * The rows used to be stacked unconditionally below the main rect, so a
 * dropdown near the bottom of the screen put its whole list past the bottom
 * edge: drawn off-screen, unclickable, and STILL claimed as the popup's modal
 * rect, so rows nobody could see went on blocking whatever was underneath.
 * Unity's Dropdown constrains the list to the canvas for the same reason.
 *
 * Geometry, all derived and spelled out so the click point cannot silently
 * stop testing anything (a modal test in this series once passed with the
 * feature off because its click landed outside every rect):
 *   screen 200 tall, main rect 40 tall placed at y 140..180, 2 options.
 *   block = 2*40 = 80.  below = 180, and 180+80 = 260 > 200 => does not fit.
 *   above = 140-80 = 60 >= 0 => flips.  Rows: [60,100) and [100,140).
 *   Row 1's centre is y = 120.  Under the OLD geometry row 1 was [220,260),
 *   entirely off a 200px screen, so a click at 120 hit nothing at all. */
static void test_popup_flips_above_when_it_would_overflow(void)
{
    JceEntity dd = 0;
    JceScene *s = make_scene_with_dropdown(&dd, 2, 0, /*expanded*/false,
                                           /*interactable*/true);
    dd_of(s, dd)->rect.anchored_position[1] = -140.0f;   /* main at y 140..180 */

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    click_at(uc, s, MAIN_CX, 160.0f);                   /* open it */
    TEST_ASSERT_TRUE(dd_of(s, dd)->expanded);

    click_at(uc, s, MAIN_CX, 120.0f);                   /* row 1, flipped above */
    TEST_ASSERT_FALSE_MESSAGE(dd_of(s, dd)->expanded,
        "picking an option should collapse the popup");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, dd_of(s, dd)->selected_index,
        "the option list was placed off the bottom of the screen");

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_click_expands);
    RUN_TEST(test_click_option_selects);
    RUN_TEST(test_click_outside_collapses);
    RUN_TEST(test_non_interactable);
    RUN_TEST(test_selected_index_in_range);
    RUN_TEST(test_progress_bar_smoke);
    RUN_TEST(test_unreachable_expanded_popup_collapses);
    RUN_TEST(test_popup_flips_above_when_it_would_overflow);
    return UNITY_END();
}
