/* test_jce_ui_widgets.c
 *
 * Interactive value widgets — UI Slider + UI Toggle.
 *
 * Drives the REAL headless UI canvas pipeline (jce_ui_canvas_render on a
 * canvas created with renderer == NULL) against authored JceScenes that carry
 * a Canvas + a UISlider / UIToggle at a KNOWN screen rect, then feeds a
 * synthetic JceUIPointer across frames and asserts the canvas MUTATES the
 * scene component in place (Unity source-of-truth model):
 *   - a SLIDER drag maps the pointer position along its main axis to a value
 *     in [min,max], honouring direction + whole_numbers + interactable;
 *   - a TOGGLE click (press-inside → release-over) flips is_on, honouring
 *     interactable;
 *   - releasing the pointer ends a slider drag (further !down moves are inert).
 *
 * The full layout + raycast + interaction state machines run with no live
 * bgfx context (renderer NULL) — only the GPU draw calls are skipped — so the
 * widget value/state logic is exercised exactly as it runs in a shipped game.
 * Mirrors the harness of test_jce_ui_button_dispatch.c.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_ui_canvas.h>

#include "unity.h"
#include <math.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Shared scene geometry ─────────────────────────────────────────────
 * A 200x40 screen.  The widget child uses a fixed-size RectTransform
 * (anchor_min == anchor_max == {0,1}, pivot {0,1} -- UGUI top-left) so uc_resolve_rect places
 * it deterministically at:
 *     x = anchored_position.x = 0,  w = size_delta.x = 200
 *     y = anchored_position.y = 0,  h = size_delta.y = 40
 * i.e. the widget occupies the whole screen rect (0,0)..(200,40). */
#define SCREEN_W   200.0f
#define SCREEN_H   40.0f
#define TOL        0.02f

/* Fixed-size RectTransform of (w,h) flush with the parent's top-left,
 * authored in UGUI terms (anchor/pivot {0,1}). */
static void set_full_rect(JceRectTransform *rt, float w, float h)
{
    memset(rt, 0, sizeof *rt);
    /* UGUI: anchor + pivot at the TOP-left so the element's top edge sits on
     * the parent's, whatever its height.  {0,0} is UGUI's BOTTOM-left and only
     * coincided with this while the rect happened to be full-height. */
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

/* Author a Canvas + a child slider with the given range/direction/flags. */
static JceScene *make_scene_with_slider(JceEntity *out_slider, float minv,
                                        float maxv, int direction,
                                        bool whole_numbers, bool interactable)
{
    JceEntity canvas = 0;
    JceScene *s = make_canvas(&canvas);

    JceEntity slider = jce_scene_create_entity(s, "Slider");
    jce_scene_set_parent(s, slider, canvas);

    JceUISliderComponent sl;
    memset(&sl, 0, sizeof sl);
    sl.value         = (minv + maxv) * 0.5f; /* start in the middle */
    sl.min_value     = minv;
    sl.max_value     = maxv;
    sl.direction     = direction;
    sl.interactable  = interactable;
    sl.whole_numbers = whole_numbers;
    sl.bg_color[3] = sl.fill_color[3] = sl.handle_color[3] = 1.0f;
    sl.handle_size = 20.0f;
    set_full_rect(&sl.rect, SCREEN_W, SCREEN_H);
    jce_scene_set_ui_slider(s, slider, &sl);
    TEST_ASSERT_TRUE(jce_scene_has_ui_slider(s, slider));

    *out_slider = slider;
    return s;
}

static JceScene *make_scene_with_toggle(JceEntity *out_toggle, bool is_on,
                                        bool interactable)
{
    JceEntity canvas = 0;
    JceScene *s = make_canvas(&canvas);

    JceEntity toggle = jce_scene_create_entity(s, "Toggle");
    jce_scene_set_parent(s, toggle, canvas);

    JceUIToggleComponent tg;
    memset(&tg, 0, sizeof tg);
    tg.is_on        = is_on;
    tg.interactable = interactable;
    tg.bg_color[3] = tg.checkmark_color[3] = 1.0f;
    set_full_rect(&tg.rect, SCREEN_W, SCREEN_H);
    jce_scene_set_ui_toggle(s, toggle, &tg);
    TEST_ASSERT_TRUE(jce_scene_has_ui_toggle(s, toggle));

    *out_toggle = toggle;
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

static float slider_value(JceScene *s, JceEntity e)
{
    JceUISliderComponent *sl = jce_scene_get_ui_slider(s, e);
    TEST_ASSERT_NOT_NULL(sl);
    return sl->value;
}

static bool toggle_on(JceScene *s, JceEntity e)
{
    JceUIToggleComponent *tg = jce_scene_get_ui_toggle(s, e);
    TEST_ASSERT_NOT_NULL(tg);
    return tg->is_on;
}

/* ── Slider drag sets value (L→R, [0,1]) ──────────────────────────────── */
static void test_slider_drag_sets_value(void)
{
    JceEntity slider = 0;
    JceScene *s = make_scene_with_slider(&slider, 0.0f, 1.0f, 0,
                                         /*whole*/false, /*inter*/true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* Press-drag to the far LEFT end → value ≈ min (0). */
    step(uc, s, 0.0f, SCREEN_H * 0.5f, true, true);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 0.0f, slider_value(s, slider));

    /* Still held, move to the far RIGHT end → value ≈ max (1). */
    step(uc, s, SCREEN_W, SCREEN_H * 0.5f, true, true);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 1.0f, slider_value(s, slider));

    /* Still held, move to the MIDDLE → value ≈ 0.5. */
    step(uc, s, SCREEN_W * 0.5f, SCREEN_H * 0.5f, true, true);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 0.5f, slider_value(s, slider));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── Slider value MAPPING over a [0,10] range ─────────────────────────── */
static void test_slider_range_mapping(void)
{
    JceEntity slider = 0;
    JceScene *s = make_scene_with_slider(&slider, 0.0f, 10.0f, 0,
                                         /*whole*/false, /*inter*/true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    step(uc, s, 0.0f, SCREEN_H * 0.5f, true, true);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 0.0f, slider_value(s, slider));

    step(uc, s, SCREEN_W, SCREEN_H * 0.5f, true, true);
    TEST_ASSERT_FLOAT_WITHIN(0.2f, 10.0f, slider_value(s, slider));

    step(uc, s, SCREEN_W * 0.5f, SCREEN_H * 0.5f, true, true);
    TEST_ASSERT_FLOAT_WITHIN(0.2f, 5.0f, slider_value(s, slider));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── Slider direction (R→L) mirrors the value mapping ─────────────────── */
static void test_slider_direction_rl(void)
{
    JceEntity slider = 0;
    JceScene *s = make_scene_with_slider(&slider, 0.0f, 1.0f, /*R→L*/1,
                                         /*whole*/false, /*inter*/true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* Press near the RIGHT edge (inside the rect so the drag begins) → MIN
     * for an R→L slider. */
    step(uc, s, SCREEN_W - 1.0f, SCREEN_H * 0.5f, true, true);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 0.0f, slider_value(s, slider));

    /* Still held, move to the left end → MAX. */
    step(uc, s, 0.0f, SCREEN_H * 0.5f, true, true);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 1.0f, slider_value(s, slider));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── Slider whole_numbers rounds to an integer ────────────────────────── */
static void test_slider_whole_numbers(void)
{
    JceEntity slider = 0;
    JceScene *s = make_scene_with_slider(&slider, 0.0f, 10.0f, 0,
                                         /*whole*/true, /*inter*/true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* A mid drag (≈5.0) yields a whole number. */
    step(uc, s, SCREEN_W * 0.5f, SCREEN_H * 0.5f, true, true);
    float v = slider_value(s, slider);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, floorf(v + 0.5f), v); /* v is integral */
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 5.0f, v);

    /* A drag near 7.3 rounds to 7. */
    step(uc, s, SCREEN_W * 0.73f, SCREEN_H * 0.5f, true, true);
    v = slider_value(s, slider);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.0f, v);

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── A non-interactable slider never changes value ────────────────────── */
static void test_slider_non_interactable(void)
{
    JceEntity slider = 0;
    JceScene *s = make_scene_with_slider(&slider, 0.0f, 1.0f, 0,
                                         /*whole*/false, /*inter*/false);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    float before = slider_value(s, slider); /* 0.5 */
    step(uc, s, 0.0f,     SCREEN_H * 0.5f, true, true);
    step(uc, s, SCREEN_W, SCREEN_H * 0.5f, true, true);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, before, slider_value(s, slider));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── Releasing the pointer ends the drag; later !down moves are inert ──── */
static void test_slider_drag_release(void)
{
    JceEntity slider = 0;
    JceScene *s = make_scene_with_slider(&slider, 0.0f, 1.0f, 0,
                                         /*whole*/false, /*inter*/true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* Drag to the middle, then RELEASE there. */
    step(uc, s, SCREEN_W * 0.5f, SCREEN_H * 0.5f, true,  true);
    step(uc, s, SCREEN_W * 0.5f, SCREEN_H * 0.5f, false, true); /* release */
    float held = slider_value(s, slider);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 0.5f, held);

    /* Pointer moves to the far right WITHOUT being held → value unchanged. */
    step(uc, s, SCREEN_W, SCREEN_H * 0.5f, false, true);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, held, slider_value(s, slider));

    /* Hovering (still !down) over the left end → still unchanged. */
    step(uc, s, 0.0f, SCREEN_H * 0.5f, false, true);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, held, slider_value(s, slider));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── Toggle click flips is_on (press-inside → release-over) ───────────── */
static void test_toggle_click_flips(void)
{
    JceEntity toggle = 0;
    JceScene *s = make_scene_with_toggle(&toggle, /*is_on*/false, /*inter*/true);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    const float cx = SCREEN_W * 0.5f, cy = SCREEN_H * 0.5f;

    /* First click: press over → release over ⇒ flips false → true. */
    step(uc, s, cx, cy, true,  true);
    TEST_ASSERT_FALSE(toggle_on(s, toggle));   /* not yet (press, not release) */
    step(uc, s, cx, cy, false, true);
    TEST_ASSERT_TRUE(toggle_on(s, toggle));    /* flipped on release */

    /* Second click flips it back true → false. */
    step(uc, s, cx, cy, true,  true);
    step(uc, s, cx, cy, false, true);
    TEST_ASSERT_FALSE(toggle_on(s, toggle));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── A non-interactable toggle never flips ────────────────────────────── */
static void test_toggle_non_interactable(void)
{
    JceEntity toggle = 0;
    JceScene *s = make_scene_with_toggle(&toggle, /*is_on*/false, /*inter*/false);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    const float cx = SCREEN_W * 0.5f, cy = SCREEN_H * 0.5f;
    step(uc, s, cx, cy, true,  true);
    step(uc, s, cx, cy, false, true);
    TEST_ASSERT_FALSE(toggle_on(s, toggle)); /* unchanged */

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_slider_drag_sets_value);
    RUN_TEST(test_slider_range_mapping);
    RUN_TEST(test_slider_direction_rl);
    RUN_TEST(test_slider_whole_numbers);
    RUN_TEST(test_slider_non_interactable);
    RUN_TEST(test_slider_drag_release);
    RUN_TEST(test_toggle_click_flips);
    RUN_TEST(test_toggle_non_interactable);
    return UNITY_END();
}
