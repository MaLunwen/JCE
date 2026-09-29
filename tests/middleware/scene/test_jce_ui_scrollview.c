/* test_jce_ui_scrollview.c
 *
 * Clipped, scrollable content viewport — UI ScrollView.
 *
 * Drives the REAL headless UI canvas pipeline (jce_ui_canvas_render on a
 * canvas created with renderer == NULL) against an authored JceScene that
 * carries a Canvas + a UIScrollView at a KNOWN viewport rect, then:
 *   - drives a synthetic JceUIPointer hover over the scroll view (so the
 *     canvas resolves it as the hovered scroll view during the raycast pass),
 *   - feeds wheel deltas via jce_ui_canvas_scroll and asserts the canvas
 *     MUTATES the component's scroll_position in place (Unity source-of-truth
 *     model) with the documented sign convention + clamp behaviour.
 *
 * The whole scroll/clamp/offset logic runs with no live bgfx context
 * (renderer NULL) — only the GPU draws + scissor are skipped — so the math is
 * exercised exactly as it runs in a shipped game.  Mirrors the harness of
 * test_jce_ui_inputfield.c.
 *
 * Sign convention (documented in jce_ui_canvas.h / .c): wheel `dy` is +up;
 * content scrolls up when the wheel rolls up, so the offset DECREASES on +dy
 * and INCREASES on -dy.  Hence jce_ui_canvas_scroll(uc, 0, -1) moves
 * scroll_position.y UP by scroll_sensitivity (reveals content further down).
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_ui_canvas.h>
#include <jce/renderer/jce_primitives.h>

#include "unity.h"
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Shared scene geometry ─────────────────────────────────────────────
 * A 200x100 screen.  The scroll view child uses a fixed-size RectTransform
 * (anchor_min == anchor_max == {0,1}, pivot {0,1} -- UGUI top-left) so uc_resolve_rect places
 * it deterministically at (0,0)..(200,100) — the whole screen rect. */
#define SCREEN_W    200.0f
#define SCREEN_H    100.0f
#define SENS        30.0f   /* scroll_sensitivity (px per notch) */

static void set_full_rect(JceRectTransform *rt, float w, float h)
{
    memset(rt, 0, sizeof *rt);
    /* UGUI: anchor + pivot at the parent TOP-left so the element hangs from
     * the top edge whatever its height.  Spelled out rather than left at the
     * memset zeros -- {0,0} is UGUI BOTTOM-left and only coincided with this
     * while the rect happened to be exactly full height. */
    rt->anchor_min[0] = 0.0f; rt->anchor_min[1] = 1.0f;
    rt->anchor_max[0] = 0.0f; rt->anchor_max[1] = 1.0f;
    rt->pivot[0]      = 0.0f; rt->pivot[1]      = 1.0f;
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

/* Author a Canvas + a child ScrollView with the given config. */
static JceScene *make_scene_with_scroll(JceEntity *out_sv,
                                        float content_w, float content_h,
                                        bool horizontal, bool vertical,
                                        bool interactable)
{
    JceEntity canvas = 0;
    JceScene *s = make_canvas(&canvas);

    JceEntity sv = jce_scene_create_entity(s, "ScrollView");
    jce_scene_set_parent(s, sv, canvas);

    JceUIScrollViewComponent c;
    memset(&c, 0, sizeof c);
    c.content_size[0]    = content_w;
    c.content_size[1]    = content_h;
    c.scroll_position[0] = 0.0f;
    c.scroll_position[1] = 0.0f;
    c.horizontal         = horizontal;
    c.vertical           = vertical;
    c.scroll_sensitivity = SENS;
    c.show_scrollbar     = true;
    c.scrollbar_thickness = 8.0f;
    c.bg_color[3] = c.scrollbar_color[3] = c.scrollbar_bg_color[3] = 1.0f;
    c.interactable       = interactable;
    set_full_rect(&c.rect, SCREEN_W, SCREEN_H);
    jce_scene_set_ui_scroll_view(s, sv, &c);
    TEST_ASSERT_TRUE(jce_scene_has_ui_scroll_view(s, sv));

    *out_sv = sv;
    return s;
}

/* Drive one render frame through the REAL canvas pipeline at pointer (x,y). */
static void step(JceUICanvas *uc, JceScene *s, float x, float y, bool valid)
{
    JceUIPointer ptr;
    ptr.x = x; ptr.y = y; ptr.down = false; ptr.valid = valid;
    jce_ui_canvas_render(uc, s, /*view*/0, /*fb*/0, SCREEN_W, SCREEN_H,
                         valid ? &ptr : NULL, 1.0f / 60.0f);
}

/* Render with the pointer hovering the scroll view centre (so the canvas
 * resolves it as the hovered scroll view this frame). */
static void hover_center(JceUICanvas *uc, JceScene *s)
{
    step(uc, s, SCREEN_W * 0.5f, SCREEN_H * 0.5f, true);
}

static JceUIScrollViewComponent *sv_get(JceScene *s, JceEntity e)
{
    JceUIScrollViewComponent *c = jce_scene_get_ui_scroll_view(s, e);
    TEST_ASSERT_NOT_NULL(c);
    return c;
}

/* ── 1. wheel scrolls within range ────────────────────────────────────── */
static void test_wheel_scrolls_within_range(void)
{
    JceEntity sv = 0;
    /* viewport 200x100, content 200x400 → vertical range 0..300. */
    JceScene *s = make_scene_with_scroll(&sv, 200.0f, 400.0f, false, true, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    hover_center(uc, s);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, sv_get(s, sv)->scroll_position[1]);

    /* dy = -1 (wheel down) → offset increases by one notch (SENS). */
    jce_ui_canvas_scroll(uc, 0.0f, -1.0f);
    TEST_ASSERT_EQUAL_FLOAT(SENS, sv_get(s, sv)->scroll_position[1]);

    /* Another notch down → 2*SENS. */
    jce_ui_canvas_scroll(uc, 0.0f, -1.0f);
    TEST_ASSERT_EQUAL_FLOAT(SENS * 2.0f, sv_get(s, sv)->scroll_position[1]);

    /* dy = +1 (wheel up) → offset decreases back toward 0. */
    jce_ui_canvas_scroll(uc, 0.0f, 1.0f);
    TEST_ASSERT_EQUAL_FLOAT(SENS, sv_get(s, sv)->scroll_position[1]);

    /* Horizontal axis is disabled → x stays 0 even though dy moved y. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, sv_get(s, sv)->scroll_position[0]);

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 2. clamp at top + bottom ─────────────────────────────────────────── */
static void test_clamp_top_and_bottom(void)
{
    JceEntity sv = 0;
    JceScene *s = make_scene_with_scroll(&sv, 200.0f, 400.0f, false, true, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    hover_center(uc, s);

    /* Scroll WAY past the content end (range max = 400-100 = 300). */
    for (int i = 0; i < 100; ++i)
        jce_ui_canvas_scroll(uc, 0.0f, -1.0f);   /* each = +SENS, far past 300 */
    TEST_ASSERT_EQUAL_FLOAT(300.0f, sv_get(s, sv)->scroll_position[1]);

    /* Scroll WAY back up past the start → clamps to 0. */
    for (int i = 0; i < 100; ++i)
        jce_ui_canvas_scroll(uc, 0.0f, 1.0f);    /* each = -SENS, far below 0 */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, sv_get(s, sv)->scroll_position[1]);

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 3. no scroll when content <= viewport ────────────────────────────── */
static void test_no_scroll_when_content_fits(void)
{
    JceEntity sv = 0;
    /* content_h (80) <= viewport (100) → no vertical range. */
    JceScene *s = make_scene_with_scroll(&sv, 200.0f, 80.0f, false, true, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    hover_center(uc, s);
    jce_ui_canvas_scroll(uc, 0.0f, -1.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, sv_get(s, sv)->scroll_position[1]);
    jce_ui_canvas_scroll(uc, 0.0f, 1.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, sv_get(s, sv)->scroll_position[1]);

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 4. horizontal disabled → dx ignored ──────────────────────────────── */
static void test_horizontal_disabled_ignores_dx(void)
{
    JceEntity sv = 0;
    /* vertical-only; content wide (400) AND tall (400) so a horizontal range
     * WOULD exist if the axis were enabled — proving the gate, not the range. */
    JceScene *s = make_scene_with_scroll(&sv, 400.0f, 400.0f, false, true, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    hover_center(uc, s);
    jce_ui_canvas_scroll(uc, 1.0f, 0.0f);   /* dx with horizontal disabled */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, sv_get(s, sv)->scroll_position[0]);

    /* Sanity: with horizontal ENABLED, dx WOULD move x by SENS. */
    JceEntity sh = 0;
    JceScene *s2 = make_scene_with_scroll(&sh, 400.0f, 400.0f, true, true, true);
    JceUICanvas *uc2 = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc2);
    hover_center(uc2, s2);
    jce_ui_canvas_scroll(uc2, 1.0f, 0.0f);
    TEST_ASSERT_EQUAL_FLOAT(SENS, sv_get(s2, sh)->scroll_position[0]);
    jce_ui_canvas_destroy(uc2);
    jce_scene_destroy(s2);

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 5. non-interactable → wheel ignored ──────────────────────────────── */
static void test_non_interactable_ignores_wheel(void)
{
    JceEntity sv = 0;
    JceScene *s = make_scene_with_scroll(&sv, 200.0f, 400.0f, false, true, /*inter*/false);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    hover_center(uc, s);
    jce_ui_canvas_scroll(uc, 0.0f, -1.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, sv_get(s, sv)->scroll_position[1]);

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 6. not hovered → no change ────────────────────────────────────────── */
static void test_not_hovered_no_change(void)
{
    JceEntity sv = 0;
    JceScene *s = make_scene_with_scroll(&sv, 200.0f, 400.0f, false, true, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* Render with NO valid pointer (pointer absent) → no scroll view hovered. */
    step(uc, s, 0.0f, 0.0f, false);
    jce_ui_canvas_scroll(uc, 0.0f, -1.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, sv_get(s, sv)->scroll_position[1]);

    /* Render with a valid pointer OUTSIDE the scroll view rect.  The rect is
     * (0,0)..(200,100); place the pointer far below/right at (199,99) is still
     * inside, so use an explicitly-outside coordinate via a smaller-rect view.
     * Here the view fills the screen, so emulate "outside" by a pointer beyond
     * the screen extent (the hit test is half-open [x, x+w)). */
    step(uc, s, SCREEN_W, SCREEN_H, true);  /* (200,100) is just OUTSIDE */
    jce_ui_canvas_scroll(uc, 0.0f, -1.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, sv_get(s, sv)->scroll_position[1]);

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 7. authored over-range offset is clamped on render ────────────────── */
static void test_authored_offset_clamped_on_render(void)
{
    JceEntity sv = 0;
    JceScene *s = make_scene_with_scroll(&sv, 200.0f, 400.0f, false, true, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* Author an offset beyond the valid range (max = 300). */
    sv_get(s, sv)->scroll_position[1] = 9999.0f;
    /* A render runs uc_scroll_clamp on the live component (headless-safe). */
    hover_center(uc, s);
    TEST_ASSERT_EQUAL_FLOAT(300.0f, sv_get(s, sv)->scroll_position[1]);

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 8. an ANCESTOR CanvasGroup's interactable=false stops the wheel ────
 *
 * Distinct from test_non_interactable_ignores_wheel above, which sets the
 * ScrollView's OWN interactable field.  The wheel channel resolves its target
 * from the hit list directly, and it was the one channel reading that list
 * without consulting the inherited CanvasGroup chain -- so a scroll view whose
 * buttons were correctly greyed out and click-dead still scrolled.
 *
 * The ScrollView's own interactable stays TRUE here on purpose: if it were
 * false this test would pass for the other reason and prove nothing. */
static void test_ancestor_canvas_group_non_interactable_ignores_wheel(void)
{
    JceEntity sv = 0;
    JceScene *s = make_scene_with_scroll(&sv, 0.0f, 400.0f, false, true,
                                         /*interactable*/true);
    JceEntity parent = jce_scene_get_parent(s, sv);
    TEST_ASSERT_NOT_EQUAL(0, parent);

    JceCanvasGroupComponent cg;
    memset(&cg, 0, sizeof cg);
    cg.alpha           = 1.0f;
    cg.interactable    = false;   /* greyed out ... */
    cg.blocks_raycasts = true;    /* ... but still receiving the pointer */
    jce_scene_set_canvas_group(s, parent, &cg);

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    hover_center(uc, s);
    jce_ui_canvas_scroll(uc, 0.0f, -1.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, sv_get(s, sv)->scroll_position[1]);

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── 9. the scroll range is in AUTHOR units, not device px ─────────────
 *
 * uc_resolve_rect multiplies the viewport by the CanvasScaler factor, while
 * content_size / scroll_position / scroll_sensitivity are authored numbers in
 * reference-resolution units -- like scrollbar_thickness, which the draw has
 * always scaled.  Subtracting one from the other gave a scroll range wrong by
 * the scale factor on any canvas that actually scales.
 *
 * scale_factor 2, viewport 200x100 authored => 200 device px tall, i.e. 100
 * reference units.  Content 400 => 300 units of range.  Reading the viewport
 * as 200 (device) instead of 100 (reference) yields 200, so the two answers
 * are far apart and the assert cannot pass for the wrong reason. */
static void test_scroll_range_is_in_reference_units(void)
{
    JceEntity sv = 0;
    JceScene *s = make_scene_with_scroll(&sv, 0.0f, 400.0f, false, true, true);

    JceCanvasComponent *cv = jce_scene_get_canvas(s, jce_scene_get_parent(s, sv));
    TEST_ASSERT_NOT_NULL(cv);
    cv->scale_factor = 2.0f;      /* constant-pixel-size term => ui_scale 2 */

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    hover_center(uc, s);
    for (int i = 0; i < 40; i++)  /* far past either candidate maximum */
        jce_ui_canvas_scroll(uc, 0.0f, -1.0f);

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(300.0f, sv_get(s, sv)->scroll_position[1],
        "scroll range compared authored content against a device-px viewport");

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* -- drag gestures ---------------------------------------------------
 *
 * Geometry, derived once and shared by the four tests below.  Viewport
 * (0,0,200,100), content 400 tall, vertical only, ui_scale 1:
 *   my    = 400 - 100                  = 300   (reference units of range)
 *   track = x 192..200, y 0..100       (thickness 8, right edge, no corner
 *                                       because the horizontal bar is off)
 *   thumb = track.h * (100/400) = 25   => y 0..25 at rest
 *   len   = 100 - 25                   = 75    (pointer travel <-> full range)
 * so one px of thumb travel is 300/75 = 4 units of scroll. */
#define TRACK_X   196.0f      /* inside the 192..200 track */
#define THUMB_CY   12.0f      /* inside the resting thumb, 0..25 */

/* One frame with an explicit button state. */
static void step_btn(JceUICanvas *uc, JceScene *s, float x, float y, bool down)
{
    JceUIPointer ptr;
    ptr.x = x; ptr.y = y; ptr.down = down; ptr.valid = true;
    jce_ui_canvas_render(uc, s, 0, 0, SCREEN_W, SCREEN_H, &ptr, 1.0f / 60.0f);
}

/* -- 10. dragging the scrollbar thumb scrolls -------------------------
 *
 * The bars were drawn and never read: a control that looks grabbable and is
 * not.  30 px of thumb travel is 30/75 of the range = 120 units. */
static void test_thumb_drag_scrolls(void)
{
    JceEntity sv = 0;
    JceScene *s = make_scene_with_scroll(&sv, 0.0f, 400.0f, false, true, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    step_btn(uc, s, TRACK_X, THUMB_CY, true);            /* grab the thumb */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, sv_get(s, sv)->scroll_position[1]);
    step_btn(uc, s, TRACK_X, THUMB_CY + 30.0f, true);    /* drag down 30 px */
    TEST_ASSERT_EQUAL_FLOAT(120.0f, sv_get(s, sv)->scroll_position[1]);
    step_btn(uc, s, TRACK_X, THUMB_CY + 30.0f, false);   /* release */
    TEST_ASSERT_EQUAL_FLOAT(120.0f, sv_get(s, sv)->scroll_position[1]);

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* -- 11. clicking the track jumps the thumb to the pointer ------------
 * y = 50 => (50 - 12.5)/75 = 0.5 of the range = 150. */
static void test_track_click_jumps(void)
{
    JceEntity sv = 0;
    JceScene *s = make_scene_with_scroll(&sv, 0.0f, 400.0f, false, true, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    step_btn(uc, s, TRACK_X, 50.0f, true);
    TEST_ASSERT_EQUAL_FLOAT(150.0f, sv_get(s, sv)->scroll_position[1]);

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* -- 12. content drag needs the threshold ----------------------------
 *
 * Below it the gesture is still a click and nothing moves; past it the content
 * follows the finger, so dragging UP by 30 px reveals what is below and the
 * offset INCREASES by 30. */
static void test_content_drag_threshold(void)
{
    JceEntity sv = 0;
    JceScene *s = make_scene_with_scroll(&sv, 0.0f, 400.0f, false, true, true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    step_btn(uc, s, 100.0f, 50.0f, true);
    step_btn(uc, s, 100.0f, 47.0f, true);      /* 3 px: under the threshold */
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, sv_get(s, sv)->scroll_position[1],
        "a 3px wobble scrolled, so no click inside a list can survive");
    step_btn(uc, s, 100.0f, 20.0f, true);      /* 30 px: a drag */
    TEST_ASSERT_EQUAL_FLOAT(30.0f, sv_get(s, sv)->scroll_position[1]);
    step_btn(uc, s, 100.0f, 20.0f, false);
    TEST_ASSERT_EQUAL_FLOAT(30.0f, sv_get(s, sv)->scroll_position[1]);

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* -- 13. a drag that starts on a list item must not click it ----------
 *
 * The half of the feature that silently does the wrong thing.  Both halves are
 * asserted in one test so neither can pass alone: the same button, clicked
 * without moving, still clicks. */
static void test_content_drag_cancels_the_click(void)
{
    JceEntity sv = 0;
    JceScene *s = make_scene_with_scroll(&sv, 0.0f, 400.0f, false, true, true);

    JceEntity btn = jce_scene_create_entity(s, "Item");
    jce_scene_set_parent(s, btn, sv);
    JceUIButtonComponent b;
    memset(&b, 0, sizeof b);
    b.interactable = true;
    b.normal_color[3] = b.highlighted_color[3] = b.pressed_color[3] = 1.0f;
    set_full_rect(&b.rect, 150.0f, 40.0f);       /* (0,0)..(150,40) */
    jce_scene_set_ui_button(s, btn, &b);

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* A plain click on the item still clicks. */
    step_btn(uc, s, 60.0f, 20.0f, true);
    step_btn(uc, s, 60.0f, 20.0f, false);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)btn, jce_ui_canvas_last_clicked(uc),
        "a press and release with no movement must still be a click");

    /* A drag that begins on it does not. */
    step_btn(uc, s, 60.0f, 30.0f, true);
    step_btn(uc, s, 60.0f, 5.0f,  true);          /* 25 px: a drag */
    step_btn(uc, s, 60.0f, 5.0f,  false);         /* release */
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0u, jce_ui_canvas_last_clicked(uc),
        "scrolling a list by dragging an item also clicked the item");
    TEST_ASSERT_TRUE_MESSAGE(sv_get(s, sv)->scroll_position[1] > 0.0f,
        "the drag did not scroll, so the click assert above proves nothing");

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── Nested ScrollViews: the clip is the INTERSECTION ────────────────
 *
 * WHAT THIS PINS.  jce_ui_canvas.c computes ONE rectangle per level --
 * `child_clip_p`, the node's rect intersected with whatever clip was already
 * active -- and both consumers now read it: the raycast (which records it in
 * last_hits[].rect, what jce_ui_canvas_entity_rect answers) and the GPU
 * scissor.  Before, the scissor was set to the node's OWN rect instead, so an
 * inner ScrollView drew outside the outer viewport while the raycast agreed
 * it was clipped -- input and pixels disagreeing about the same widget.
 *
 * WHAT IT DOES NOT PROVE, stated rather than implied: that bgfx clipped a
 * pixel.  This is a headless render (renderer == NULL) and no draw is
 * submitted.  It proves the ENGINE ASKS for the right rectangle, which is the
 * half that was wrong; the other half is one bgfx_set_scissor call, gated by
 * tests/renderer/test_jce_draw_scissor.c. */
static void test_a_nested_scroll_view_clips_to_the_intersection(void)
{
    JceEntity outer = 0;
    JceScene *s = make_scene_with_scroll(&outer, 0.0f, 0.0f, false, true, true);

    /* The outer viewport is the full screen; narrow it so an intersection is
     * distinguishable from either rect on its own. */
    JceUIScrollViewComponent oc = *jce_scene_get_ui_scroll_view(s, outer);
    oc.rect.size_delta[0] = 200.0f;
    oc.rect.size_delta[1] = 200.0f;
    jce_scene_set_ui_scroll_view(s, outer, &oc);

    /* An inner ScrollView, offset so it hangs OFF the outer's right edge:
     * outer covers x 0..200, inner x 100..300.  Their intersection is
     * 100..200 -- neither rect, which is the point. */
    JceEntity inner = jce_scene_create_entity(s, "Inner");
    jce_scene_set_parent(s, inner, outer);
    JceUIScrollViewComponent ic;
    memset(&ic, 0, sizeof ic);
    ic.vertical = true;
    ic.scroll_sensitivity = SENS;
    ic.interactable = true;
    ic.bg_color[3] = 1.0f;
    set_full_rect(&ic.rect, 200.0f, 100.0f);
    ic.rect.anchored_position[0] = 100.0f;
    jce_scene_set_ui_scroll_view(s, inner, &ic);

    /* A leaf inside the inner one, wider than both. */
    JceEntity leaf = jce_scene_create_entity(s, "Leaf");
    jce_scene_set_parent(s, leaf, inner);
    JceUIImageComponent img;
    memset(&img, 0, sizeof img);
    img.color[3] = 1.0f;
    img.raycast_target = true;
    set_full_rect(&img.rect, 400.0f, 50.0f);
    jce_scene_set_ui_image(s, leaf, &img);

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);
    step(uc, s, 150.0f, 20.0f, true);

    /* THE RAYCAST SIDE, which was already right. */
    float x = 0, y = 0, w = 0, h = 0;
    TEST_ASSERT_TRUE_MESSAGE(
        jce_ui_canvas_entity_rect(uc, (uint64_t)leaf, &x, &y, &w, &h),
        "the leaf was not recorded at all: it is inside two ScrollViews and "
        "should be clipped, not dropped");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(100.0f, x,
        "the raycast clip ignored the inner ScrollView");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(100.0f, w,
        "the raycast clip ignored the OUTER ScrollView");

    /* THE DRAW SIDE, which was not.  Two ScrollViews means two clip-set
     * entries followed by two restores; the INNER one (the second set) is the
     * one that used to be its own rect instead of the intersection, so a
     * nested viewport drew outside its parent while the raycast above agreed
     * it was clipped. */
    /* FOUR entries, in order, and every one of them is load-bearing:
     *
     *   0  outer set     x=0   w=200   the outer viewport
     *   1  inner set     x=100 w=100   the INTERSECTION, not the inner's own
     *                                  rect (x=100 w=200) -- that was the bug
     *   2  inner restore x=0   w=200   RESTORED to the outer clip, not
     *                                  cleared -- clearing here left every
     *                                  sibling after the inner ScrollView
     *                                  unclipped, and at top level a clear
     *                                  and a restore are indistinguishable,
     *                                  which is why this case is nested
     *   3  outer restore inactive      nothing survives the render
     */
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, jce_ui_canvas_clip_log_count(uc),
        "two nested ScrollViews must log two clips and two restores");

    float box[4]; bool active = false;

    TEST_ASSERT_TRUE(jce_ui_canvas_clip_log_at(uc, 0, box, &active));
    TEST_ASSERT_TRUE(active);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, box[0]);
    TEST_ASSERT_EQUAL_FLOAT(200.0f, box[2]);

    TEST_ASSERT_TRUE(jce_ui_canvas_clip_log_at(uc, 1, box, &active));
    TEST_ASSERT_TRUE(active);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(100.0f, box[0],
        "the inner subtree was not scissored at its own left edge");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(100.0f, box[2],
        "the SCISSOR was set to the inner ScrollView's own rect, not its "
        "intersection with the outer one: pixels and input clip the same "
        "widget differently");

    TEST_ASSERT_TRUE(jce_ui_canvas_clip_log_at(uc, 2, box, &active));
    TEST_ASSERT_TRUE_MESSAGE(active,
        "the inner ScrollView CLEARED the clip on its way out instead of "
        "restoring the outer one: every sibling drawn after it escapes the "
        "outer viewport");
    TEST_ASSERT_EQUAL_FLOAT(0.0f, box[0]);
    TEST_ASSERT_EQUAL_FLOAT(200.0f, box[2]);

    TEST_ASSERT_TRUE(jce_ui_canvas_clip_log_at(uc, 3, box, &active));
    TEST_ASSERT_FALSE_MESSAGE(active,
        "the outermost ScrollView left a clip behind");

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── A render must not leave a scissor armed ─────────────────────────
 *
 * The clip is one global in jce_primitives.c, restored by each level of
 * jce_ui_canvas.c's recursion.  If a level cleared instead of restoring (it
 * used to), the outer clip was lost for every sibling that followed; if the
 * OUTERMOST level failed to restore, the scissor would survive the render and
 * clip the next unrelated thing the frame draws -- a 3D pass, an ImGui panel
 * -- with a rectangle from a widget it has nothing to do with. */
static void test_a_render_leaves_no_scissor_armed(void)
{
    JceEntity sv = 0;
    JceScene *s = make_scene_with_scroll(&sv, 0.0f, 2000.0f, false, true, true);

    /* A child, and it is load-bearing: uc_layout_draw returns before the clip
     * block when a node has no children, so a childless ScrollView clips
     * nothing -- correctly, since there is nothing under it to clip. A test
     * without this child would assert on a code path that never ran. */
    JceEntity leaf = jce_scene_create_entity(s, "Leaf");
    jce_scene_set_parent(s, leaf, sv);
    JceUIImageComponent img;
    memset(&img, 0, sizeof img);
    img.color[3] = 1.0f;
    img.raycast_target = true;
    set_full_rect(&img.rect, 100.0f, 50.0f);
    jce_scene_set_ui_image(s, leaf, &img);

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* A REAL rectangle, not (0,0,0,0) which merely clears.  It has to be one
     * the render must overwrite: with a cleared start, a canvas that recorded
     * clips into its log and never touched the actual scissor would leave the
     * state cleared too and every assertion below would still pass -- the log
     * describing a render that did not happen.  Starting armed, only a render
     * that really drives jce_draw_set_scissor can end cleared. */
    jce_draw_set_scissor(5, 5, 7, 7);
    int pre[4];
    TEST_ASSERT_TRUE(jce_draw_get_scissor(pre));
    step(uc, s, SCREEN_W * 0.5f, SCREEN_H * 0.5f, true);

    int box[4];
    TEST_ASSERT_FALSE_MESSAGE(jce_draw_get_scissor(box),
        "the canvas left a scissor armed after its render: everything drawn "
        "after it this frame is clipped to a ScrollView's viewport");

    /* And the log says the restore HAPPENED rather than the clip merely never
     * being set: a ScrollView must produce a set AND a restore, and the last
     * entry must be the restore. */
    int n = jce_ui_canvas_clip_log_count(uc);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, n,
        "one ScrollView must log exactly one clip and one restore");
    float lb[4]; bool active = true;
    TEST_ASSERT_TRUE(jce_ui_canvas_clip_log_at(uc, 0, lb, &active));
    TEST_ASSERT_TRUE_MESSAGE(active, "the first entry is not a clip");
    TEST_ASSERT_TRUE(jce_ui_canvas_clip_log_at(uc, 1, lb, &active));
    TEST_ASSERT_FALSE_MESSAGE(active,
        "the ScrollView did not restore the clip it entered with");

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_wheel_scrolls_within_range);
    RUN_TEST(test_clamp_top_and_bottom);
    RUN_TEST(test_no_scroll_when_content_fits);
    RUN_TEST(test_horizontal_disabled_ignores_dx);
    RUN_TEST(test_non_interactable_ignores_wheel);
    RUN_TEST(test_not_hovered_no_change);
    RUN_TEST(test_authored_offset_clamped_on_render);
    RUN_TEST(test_ancestor_canvas_group_non_interactable_ignores_wheel);
    RUN_TEST(test_scroll_range_is_in_reference_units);
    RUN_TEST(test_thumb_drag_scrolls);
    RUN_TEST(test_track_click_jumps);
    RUN_TEST(test_content_drag_threshold);
    RUN_TEST(test_content_drag_cancels_the_click);
    RUN_TEST(test_a_nested_scroll_view_clips_to_the_intersection);
    RUN_TEST(test_a_render_leaves_no_scissor_armed);
    return UNITY_END();
}
