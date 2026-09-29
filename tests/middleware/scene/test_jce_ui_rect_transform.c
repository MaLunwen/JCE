/*
 * test_jce_ui_rect_transform.c  RectTransform resolution is Unity UGUI.
 *
 * WHY THIS EXISTS.  The ECS UI is documented as "Unity-UGUI-shaped", every one
 * of the 844 UI rects authored in this tree uses a UGUI idiom, and nothing
 * tested the resolution.  uc_resolve_rect fed anchor.y and anchoredPosition.y
 * straight into a top-left-origin, +Y-down pixel space, so:
 *
 *   anchorMin/Max.y = 1, pivot.y = 1, anchoredY = -N   (UGUI: N px below the
 *                                                       TOP edge)
 *   resolved to N px above the BOTTOM edge.
 *
 * Every authored HUD landed on the wrong edge -- in the editor and in the
 * shipped runtime alike -- and the six existing UI tests could not see it
 * because they all pinned anchor/pivot to {0,0} with a full-height rect, where
 * the two readings coincide.
 *
 * uc_resolve_rect is static, so this observes the resolved rect through the
 * one public channel that reports it: the graphic raycaster.  A HEADLESS
 * canvas (renderer == NULL) runs layout + raycast + the click state machine
 * with no bgfx context, and jce_ui_canvas_last_clicked() is non-zero only when
 * the press AND release both landed inside the resolved rect.  So "is the
 * button here?" is answerable exactly, at a point, without a GPU.
 */

#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_ui_canvas.h>

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define SCREEN_W  400.0f
#define SCREEN_H  300.0f

#define BOX_W      80.0f
#define BOX_H      40.0f
#define INSET      30.0f   /* distance from the anchored edge */

/* Build a scene with one overlay Canvas and one UIImage+UIButton child whose
 * RectTransform is exactly `rt`.  reference_resolution is left at 0 so the
 * CanvasScaler factor stays 1 and every number below is literal pixels. */
static JceScene *make_scene(const JceRectTransform *rt, JceEntity *out_btn)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity canvas = jce_scene_create_entity(s, "Canvas");
    JceCanvasComponent cv;
    memset(&cv, 0, sizeof cv);
    cv.render_mode = JCE_CANVAS_OVERLAY;
    cv.scale_factor = 1.0f;
    jce_scene_set_canvas(s, canvas, &cv);

    JceEntity btn = jce_scene_create_entity(s, "Btn");
    jce_scene_set_parent(s, btn, canvas);

    JceUIImageComponent im;
    memset(&im, 0, sizeof im);
    im.color[0] = im.color[1] = im.color[2] = im.color[3] = 1.0f;
    im.raycast_target = true;
    im.rect = *rt;
    jce_scene_set_ui_image(s, btn, &im);

    JceUIButtonComponent bt;
    memset(&bt, 0, sizeof bt);
    bt.interactable = true;
    bt.fade_duration = 0.0f;
    jce_scene_set_ui_button(s, btn, &bt);

    *out_btn = btn;
    return s;
}

/* Press then release at (x,y); returns the clicked entity (0 = missed). */
static uint64_t click_at(JceUICanvas *uc, JceScene *s, float x, float y)
{
    JceUIPointer p;
    p.x = x; p.y = y; p.valid = true;
    p.down = true;
    jce_ui_canvas_render(uc, s, 0, 0, SCREEN_W, SCREEN_H, &p, 0.016f);
    p.down = false;
    jce_ui_canvas_render(uc, s, 0, 0, SCREEN_W, SCREEN_H, &p, 0.016f);
    return jce_ui_canvas_last_clicked(uc);
}

/* Assert the button's resolved rect is exactly (x,y,BOX_W,BOX_H): its centre
 * hits, and a point just past each edge misses. */
static void assert_rect_at(const JceRectTransform *rt, float x, float y)
{
    JceEntity btn = 0;
    JceScene *s = make_scene(rt, &btn);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);   /* headless */
    TEST_ASSERT_NOT_NULL(uc);

    TEST_ASSERT_EQUAL_UINT64((uint64_t)btn,
        click_at(uc, s, x + BOX_W * 0.5f, y + BOX_H * 0.5f));

    TEST_ASSERT_EQUAL_UINT64(0u, click_at(uc, s, x - 2.0f, y + BOX_H * 0.5f));
    TEST_ASSERT_EQUAL_UINT64(0u, click_at(uc, s, x + BOX_W + 2.0f, y + BOX_H * 0.5f));
    TEST_ASSERT_EQUAL_UINT64(0u, click_at(uc, s, x + BOX_W * 0.5f, y - 2.0f));
    TEST_ASSERT_EQUAL_UINT64(0u, click_at(uc, s, x + BOX_W * 0.5f, y + BOX_H + 2.0f));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

static void set_rect(JceRectTransform *rt, float ax, float ay,
                     float px, float py, float apx, float apy)
{
    memset(rt, 0, sizeof *rt);
    rt->anchor_min[0] = ax; rt->anchor_min[1] = ay;
    rt->anchor_max[0] = ax; rt->anchor_max[1] = ay;
    rt->pivot[0]      = px; rt->pivot[1]      = py;
    rt->anchored_position[0] = apx; rt->anchored_position[1] = apy;
    rt->size_delta[0] = BOX_W; rt->size_delta[1] = BOX_H;
}

/* UGUI: anchor + pivot at the TOP-left, anchoredY negative => INSET px below
 * the screen's top edge.  The inverted reading put it INSET px above the
 * BOTTOM edge instead -- this is the exact shape 124 authored rects use. */
static void test_top_anchor_negative_y_lands_below_the_top_edge(void)
{
    JceRectTransform rt;
    set_rect(&rt, 0.0f, 1.0f, 0.0f, 1.0f, INSET, -INSET);
    assert_rect_at(&rt, INSET, INSET);
}

/* UGUI: anchor + pivot at the BOTTOM-left, anchoredY positive => INSET px
 * above the screen's bottom edge.  186 authored rects use this shape. */
static void test_bottom_anchor_positive_y_lands_above_the_bottom_edge(void)
{
    JceRectTransform rt;
    set_rect(&rt, 0.0f, 0.0f, 0.0f, 0.0f, INSET, INSET);
    assert_rect_at(&rt, INSET, SCREEN_H - INSET - BOX_H);
}

/* UGUI: centred anchor + pivot, anchoredY negative => BELOW the centre.  The
 * inverted reading mirrored it about the centre line, which is small enough to
 * look like a layout nudge rather than a defect. */
static void test_centre_anchor_negative_y_lands_below_the_centre(void)
{
    JceRectTransform rt;
    set_rect(&rt, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, -INSET);
    assert_rect_at(&rt,
                   SCREEN_W * 0.5f - BOX_W * 0.5f,
                   SCREEN_H * 0.5f + INSET - BOX_H * 0.5f);
}

/* Pivot is the element's OWN reference point: with the pivot at the bottom
 * edge, the same anchoredY hangs the box's bottom on the anchor, so the box
 * extends upward. */
static void test_pivot_moves_the_box_not_the_anchor(void)
{
    JceRectTransform rt;
    set_rect(&rt, 0.0f, 1.0f, 0.0f, 0.0f, INSET, -INSET);
    /* bottom edge INSET below the top => top edge is INSET - BOX_H */
    assert_rect_at(&rt, INSET, INSET - BOX_H);
}

/* Unity: size = anchorSpan*parentSize + sizeDelta, and rect.min =
 * anchorMin*parentSize + anchoredPosition - sizeDelta*pivot.  So a NEGATIVE
 * sizeDelta INSETS a stretched element.  The old code subtracted sizeDelta
 * from the span and offset by +sizeDelta*0.5 ignoring the pivot, which grew
 * the element where Unity shrinks it. */
static void test_stretch_negative_size_delta_insets(void)
{
    JceRectTransform rt;
    memset(&rt, 0, sizeof rt);
    rt.anchor_min[0] = 0.0f; rt.anchor_min[1] = 0.0f;
    rt.anchor_max[0] = 1.0f; rt.anchor_max[1] = 1.0f;
    rt.pivot[0]      = 0.5f; rt.pivot[1]      = 0.5f;
    rt.size_delta[0] = -2.0f * INSET;   /* inset INSET on each side */
    rt.size_delta[1] = -2.0f * INSET;

    JceEntity btn = 0;
    JceScene *s = make_scene(&rt, &btn);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* Inside the inset box. */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)btn,
        click_at(uc, s, SCREEN_W * 0.5f, SCREEN_H * 0.5f));
    TEST_ASSERT_EQUAL_UINT64((uint64_t)btn,
        click_at(uc, s, INSET + 2.0f, INSET + 2.0f));
    /* In the margin the inset carved out -- inside the screen, outside the box.
     * Under the old (grow) reading the box covered these. */
    TEST_ASSERT_EQUAL_UINT64(0u, click_at(uc, s, INSET - 2.0f, SCREEN_H * 0.5f));
    TEST_ASSERT_EQUAL_UINT64(0u, click_at(uc, s, SCREEN_W * 0.5f, INSET - 2.0f));
    TEST_ASSERT_EQUAL_UINT64(0u,
        click_at(uc, s, SCREEN_W - INSET + 2.0f, SCREEN_H * 0.5f));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* Canvas.scale_factor is Unity's Constant-Pixel-Size factor.  It parsed,
 * serialized and showed in the Inspector while NOTHING read it, so authoring
 * it did nothing at all. */
static void test_canvas_scale_factor_scales_px_metrics(void)
{
    JceRectTransform rt;
    set_rect(&rt, 0.0f, 1.0f, 0.0f, 1.0f, INSET, -INSET);

    JceEntity btn = 0;
    JceScene *s = make_scene(&rt, &btn);
    JceCanvasComponent *cv = NULL;
    /* The canvas is the button's parent. */
    cv = jce_scene_get_canvas(s, jce_scene_get_parent(s, btn));
    TEST_ASSERT_NOT_NULL(cv);
    cv->scale_factor = 2.0f;

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* Every px metric doubles: the box is 160x80 at (60,60). */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)btn,
        click_at(uc, s, 2.0f * INSET + BOX_W, 2.0f * INSET + BOX_H));
    /* The un-scaled box's far corner is now well inside; its OLD outside is
     * still outside, and a point inside the old box but left of the new
     * origin must miss. */
    TEST_ASSERT_EQUAL_UINT64(0u, click_at(uc, s, INSET + 2.0f, INSET + 2.0f));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ZERO MEANS LEGACY.
 *
 * UIText grew a second alignment axis and an overflow mode.  205 components in
 * this tree were authored before either existed, and this engine has no
 * scene-level migration machinery, so the zero value of each new field MUST be
 * the behaviour those components already have: vertically centred, no
 * wrapping.  Three paths produce that zero -- a memset in the parser, a memset
 * in the editor's defaults, and an absent JSON key -- and if the enum is ever
 * renumbered to match Unity's TextAnchor or Godot's VERTICAL_ALIGNMENT_TOP,
 * all three start meaning "top" and every one of those components moves.
 *
 * This is the assertion that says no to that. */
static void test_zero_valued_text_fields_mean_the_legacy_behaviour(void)
{
    TEST_ASSERT_EQUAL_INT(0, JCE_UI_TEXT_VALIGN_MIDDLE);
    TEST_ASSERT_EQUAL_INT(0, JCE_UI_TEXT_OVERFLOW_CLIP);
    /* And the horizontal axis keeps its shipped numbering: 184 components are
     * authored with alignment 0 meaning LEFT. */
    TEST_ASSERT_EQUAL_INT(0, JCE_UI_TEXT_ALIGN_LEFT);
    TEST_ASSERT_EQUAL_INT(1, JCE_UI_TEXT_ALIGN_CENTER);
    TEST_ASSERT_EQUAL_INT(2, JCE_UI_TEXT_ALIGN_RIGHT);

    JceUITextComponent t;
    memset(&t, 0, sizeof t);
    TEST_ASSERT_EQUAL_INT(JCE_UI_TEXT_VALIGN_MIDDLE, t.vertical_alignment);
    TEST_ASSERT_EQUAL_INT(JCE_UI_TEXT_OVERFLOW_CLIP, t.overflow);
    TEST_ASSERT_EQUAL_INT(JCE_UI_TEXT_ALIGN_LEFT,    t.alignment);
}

/* CONTAINERS MUST NOT DELETE THEIR SUBTREE.
 *
 * uc_layout_draw only walks INTO children that pass uc_is_ui_element, so an
 * entity that fails it is not merely undrawn -- everything under it is gone
 * too.  The predicate listed the eight graphic/widget components and nothing
 * else, which silently deleted both canonical container idioms: an empty node
 * with a CanvasGroup, and an empty node with a LayoutGroup holding a column of
 * widgets.  Add Component offers both on any entity and the Inspector draws
 * them in full, so they were authorable and inert.
 *
 * Observed the same way as the rest of this file: through the raycaster, which
 * only reports a hit for something that was actually laid out. */
static void test_a_container_only_node_still_carries_its_children(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity canvas = jce_scene_create_entity(s, "Canvas");
    JceCanvasComponent cv;
    memset(&cv, 0, sizeof cv);
    cv.render_mode = JCE_CANVAS_OVERLAY;
    cv.scale_factor = 1.0f;
    jce_scene_set_canvas(s, canvas, &cv);

    /* An EMPTY node carrying only a CanvasGroup -- no graphic of its own. */
    JceEntity group = jce_scene_create_entity(s, "Group");
    jce_scene_set_parent(s, group, canvas);
    JceCanvasGroupComponent cg;
    memset(&cg, 0, sizeof cg);
    cg.alpha = 1.0f;
    cg.interactable = true;
    cg.blocks_raycasts = true;
    jce_scene_set_canvas_group(s, group, &cg);

    /* A real button underneath it. */
    JceEntity btn = jce_scene_create_entity(s, "Btn");
    jce_scene_set_parent(s, btn, group);
    JceUIImageComponent im;
    memset(&im, 0, sizeof im);
    im.color[0] = im.color[1] = im.color[2] = im.color[3] = 1.0f;
    im.raycast_target = true;
    set_rect(&im.rect, 0.0f, 1.0f, 0.0f, 1.0f, INSET, -INSET);
    jce_scene_set_ui_image(s, btn, &im);
    JceUIButtonComponent bt;
    memset(&bt, 0, sizeof bt);
    bt.interactable = true;
    jce_scene_set_ui_button(s, btn, &bt);

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);
    /* The container full-stretches the canvas, so the button lands where it
     * would have without it.  Before the fix this was 0: the container was
     * dropped and took the button with it. */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)btn,
        click_at(uc, s, INSET + BOX_W * 0.5f, INSET + BOX_H * 0.5f));

    /* And the container's alpha now actually gates it. */
    JceCanvasGroupComponent *live = jce_scene_get_canvas_group(s, group);
    TEST_ASSERT_NOT_NULL(live);
    live->alpha = 0.0f;
    TEST_ASSERT_EQUAL_UINT64(0u,
        click_at(uc, s, INSET + BOX_W * 0.5f, INSET + BOX_H * 0.5f));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* A Canvas nested under another Canvas is not a second screen-space root: it
 * used to be collected as one, re-anchor to the whole framebuffer, and -- when
 * it also carried a graphic -- be drawn and raycast TWICE. */
static void test_a_nested_canvas_is_not_collected_as_a_root(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceCanvasComponent cv;
    memset(&cv, 0, sizeof cv);
    cv.render_mode = JCE_CANVAS_OVERLAY;
    cv.scale_factor = 1.0f;

    JceEntity root = jce_scene_create_entity(s, "Canvas");
    jce_scene_set_canvas(s, root, &cv);

    /* A child that is BOTH a graphic and a Canvas -- the shape the editor's
     * GameObject > UI > Canvas produced when something was selected. */
    JceEntity nested = jce_scene_create_entity(s, "Nested");
    jce_scene_set_parent(s, nested, root);
    jce_scene_set_canvas(s, nested, &cv);
    JceUIImageComponent im;
    memset(&im, 0, sizeof im);
    im.color[0] = im.color[1] = im.color[2] = im.color[3] = 1.0f;
    im.raycast_target = true;
    set_rect(&im.rect, 0.0f, 1.0f, 0.0f, 1.0f, INSET, -INSET);
    jce_scene_set_ui_image(s, nested, &im);
    JceUIButtonComponent bt;
    memset(&bt, 0, sizeof bt);
    bt.interactable = true;
    jce_scene_set_ui_button(s, nested, &bt);

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);
    /* It still works -- reached once, through its parent. */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)nested,
        click_at(uc, s, INSET + BOX_W * 0.5f, INSET + BOX_H * 0.5f));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* A widget scrolled out of a ScrollView's viewport must not still be
 * clickable.  The DRAW side has always clipped (a view-level bgfx scissor);
 * the hit record was the child's own rect with no intersection against any
 * ancestor viewport, so draw and raycast disagreed about where the widget was. */
static void test_a_scrolled_out_widget_is_not_clickable(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity canvas = jce_scene_create_entity(s, "Canvas");
    JceCanvasComponent cv;
    memset(&cv, 0, sizeof cv);
    cv.render_mode = JCE_CANVAS_OVERLAY;
    cv.scale_factor = 1.0f;
    jce_scene_set_canvas(s, canvas, &cv);

    /* A 100-tall viewport pinned to the top-left. */
    JceEntity view = jce_scene_create_entity(s, "Scroll");
    jce_scene_set_parent(s, view, canvas);
    JceUIScrollViewComponent sv;
    memset(&sv, 0, sizeof sv);
    sv.vertical = true;
    sv.interactable = true;
    sv.content_size[1] = 400.0f;
    set_rect(&sv.rect, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    sv.rect.size_delta[0] = 200.0f;
    sv.rect.size_delta[1] = 100.0f;
    jce_scene_set_ui_scroll_view(s, view, &sv);

    /* A button 200px down its content -- well past the viewport's 100. */
    JceEntity btn = jce_scene_create_entity(s, "Row");
    jce_scene_set_parent(s, btn, view);
    JceUIImageComponent im;
    memset(&im, 0, sizeof im);
    im.color[0] = im.color[1] = im.color[2] = im.color[3] = 1.0f;
    im.raycast_target = true;
    set_rect(&im.rect, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, -200.0f);
    im.rect.size_delta[0] = 200.0f;
    im.rect.size_delta[1] = 40.0f;
    jce_scene_set_ui_image(s, btn, &im);
    JceUIButtonComponent bt;
    memset(&bt, 0, sizeof bt);
    bt.interactable = true;
    jce_scene_set_ui_button(s, btn, &bt);

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);
    /* Its own rect says y 200..240, which is outside the 0..100 viewport.
     * Before the fix a click there hit it. */
    TEST_ASSERT_EQUAL_UINT64(0u, click_at(uc, s, 100.0f, 220.0f));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* An OPEN dropdown popup must block the pointer from reaching what is under
 * it.  The rows are drawn on top and handled by uc_update_widgets, but they
 * were never in the hit list, and uc_update_buttons resolves from that list and
 * runs FIRST -- so choosing an option also pressed the button behind it. */
static void test_an_open_popup_blocks_the_button_underneath(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity canvas = jce_scene_create_entity(s, "Canvas");
    JceCanvasComponent cv;
    memset(&cv, 0, sizeof cv);
    cv.render_mode = JCE_CANVAS_OVERLAY;
    cv.scale_factor = 1.0f;
    jce_scene_set_canvas(s, canvas, &cv);

    /* A wide button first, so it is UNDER whatever is drawn later. */
    JceEntity under = jce_scene_create_entity(s, "Under");
    jce_scene_set_parent(s, under, canvas);
    JceUIImageComponent im;
    memset(&im, 0, sizeof im);
    im.color[0] = im.color[1] = im.color[2] = im.color[3] = 1.0f;
    im.raycast_target = true;
    set_rect(&im.rect, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, -60.0f);
    im.rect.size_delta[0] = 300.0f;
    im.rect.size_delta[1] = 120.0f;
    jce_scene_set_ui_image(s, under, &im);
    JceUIButtonComponent bt;
    memset(&bt, 0, sizeof bt);
    bt.interactable = true;
    jce_scene_set_ui_button(s, under, &bt);

    /* An expanded dropdown above it, drawn later so its popup is on top. */
    JceEntity dd = jce_scene_create_entity(s, "Dropdown");
    jce_scene_set_parent(s, dd, canvas);
    JceUIDropdownComponent d;
    memset(&d, 0, sizeof d);
    d.interactable = true;
    d.expanded = true;
    d.option_count = 3;
    snprintf(d.options[0], sizeof d.options[0], "%s", "A");
    snprintf(d.options[1], sizeof d.options[1], "%s", "B");
    snprintf(d.options[2], sizeof d.options[2], "%s", "C");
    set_rect(&d.rect, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    d.rect.size_delta[0] = 160.0f;
    d.rect.size_delta[1] = 30.0f;
    jce_scene_set_ui_dropdown(s, dd, &d);

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);
    /* Geometry, derived not guessed: the button resolves to y 60..180 x 0..300;
     * the dropdown's main row to y 0..30 x 0..160, and uc_dd_row_rect stacks
     * three more rows below it, so the popup spans y 0..120 x 0..160.  The two
     * overlap in y 60..120 x 0..160, and (80, 90) is inside both.
     * (An earlier version of this test used (80, 50), which is ABOVE the
     * button entirely -- it passed with the modal gate disabled, i.e. it was
     * testing nothing.) */
    TEST_ASSERT_EQUAL_UINT64(0u, click_at(uc, s, 80.0f, 90.0f));
    /* And a point clear of the popup (x past its 160) still reaches it. */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)under, click_at(uc, s, 250.0f, 100.0f));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* -- the Left/Right/Top/Bottom identity the Inspector edits through -----
 *
 * On a stretched axis the Inspector shows offsets from the two anchor edges
 * instead of anchoredPosition/sizeDelta, because those two say nothing useful
 * once an axis stretches.  It converts with
 *     lo = anchoredPosition - pivot*sizeDelta
 *     hi = -(anchoredPosition + (1-pivot)*sizeDelta)
 * and back.  That identity is a claim ABOUT THIS RESOLVER, so it is asserted
 * against the resolver rather than restated in the panel: the rect must land
 * at anchor_min*parent + lo, and end at anchor_max*parent - hi.
 *
 * Deliberately asymmetric numbers -- pivot 0.25, non-zero anchoredPosition,
 * anchors that are neither 0 nor 1 -- so a formula that drops the pivot term,
 * or one that is right only for a centred pivot, cannot pass. */
static void test_stretch_offsets_match_the_inspector_identity(void)
{
    const float A_MIN = 0.25f, A_MAX = 0.75f;
    const float PIVOT = 0.25f, AP = 12.0f, SD = -20.0f;

    JceRectTransform rt;
    memset(&rt, 0, sizeof rt);
    rt.anchor_min[0] = A_MIN; rt.anchor_max[0] = A_MAX;
    rt.anchor_min[1] = 0.0f;  rt.anchor_max[1] = 1.0f;   /* stretch Y too */
    rt.pivot[0] = PIVOT;      rt.pivot[1] = 0.5f;
    rt.anchored_position[0] = AP;
    rt.size_delta[0] = SD;
    rt.size_delta[1] = -2.0f * INSET;                    /* inset top+bottom */

    const float lo =  AP - PIVOT * SD;                   /* Left  */
    const float hi = -(AP + (1.0f - PIVOT) * SD);        /* Right */

    const float want_x = A_MIN * SCREEN_W + lo;
    const float want_r = A_MAX * SCREEN_W - hi;

    JceEntity btn = 0;
    JceScene *s = make_scene(&rt, &btn);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    const float cy = SCREEN_H * 0.5f;
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)btn,
        click_at(uc, s, want_x + 2.0f, cy),
        "the stretched rect does not start at anchorMin*parent + Left");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)btn,
        click_at(uc, s, want_r - 2.0f, cy),
        "the stretched rect does not end at anchorMax*parent - Right");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0u, click_at(uc, s, want_x - 2.0f, cy),
        "the rect extends past the Left offset");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0u, click_at(uc, s, want_r + 2.0f, cy),
        "the rect extends past the Right offset");

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* -- the editor pick queries --------------------------------------------
 *
 * The Scene View could not select a UI element at all: jce_scene_pick.c is a
 * GPU object-ID render over MESHES and a UIImage has no mesh.  Rather than add
 * UI quads to an ID buffer that knows nothing about raycast_target,
 * blocksRaycasts, canvas-group alpha or ScrollView clipping, the editor asks
 * the canvas -- which already answers by exactly those rules.
 *
 * These assert that the query gives the SAME answer as a click, including the
 * cases where the honest answer is "nothing", and that entity_rect reports
 * where an element DREW (a UI rect comes from anchors against the parent; the
 * Transform is never consulted, so a Transform-positioned outline would sit
 * somewhere unrelated). */
static void test_pick_agrees_with_the_click_and_reports_the_drawn_rect(void)
{
    JceRectTransform rt;
    set_rect(&rt, 0.0f, 1.0f, 0.0f, 1.0f, INSET, -INSET);   /* top-left inset */
    JceEntity btn = 0;
    JceScene *s = make_scene(&rt, &btn);

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    const float cx = INSET + BOX_W * 0.5f, cy = INSET + BOX_H * 0.5f;
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)btn, click_at(uc, s, cx, cy),
        "the click harness itself must hit, or the pick assert proves nothing");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)btn, jce_ui_canvas_pick(uc, cx, cy),
        "pick disagreed with the click at the same point");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0u, jce_ui_canvas_pick(uc, INSET - 2.0f, cy),
        "pick hit outside the rect");

    float x = -1.0f, y = -1.0f, w = -1.0f, h = -1.0f;
    TEST_ASSERT_TRUE(jce_ui_canvas_entity_rect(uc, (uint64_t)btn, &x, &y, &w, &h));
    TEST_ASSERT_EQUAL_FLOAT(INSET, x);
    TEST_ASSERT_EQUAL_FLOAT(INSET, y);
    TEST_ASSERT_EQUAL_FLOAT(BOX_W, w);
    TEST_ASSERT_EQUAL_FLOAT(BOX_H, h);

    float sw = 0.0f, sh = 0.0f;
    jce_ui_canvas_last_size(uc, &sw, &sh);
    TEST_ASSERT_EQUAL_FLOAT(SCREEN_W, sw);
    TEST_ASSERT_EQUAL_FLOAT(SCREEN_H, sh);

    TEST_ASSERT_FALSE_MESSAGE(
        jce_ui_canvas_entity_rect(uc, (uint64_t)btn + 12345u, &x, &y, &w, &h),
        "entity_rect answered for an entity that recorded no rect");

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* The query must inherit the clipping, not just the geometry: a row scrolled
 * out of its viewport is not pickable, so the editor cannot select something
 * the user cannot see. */
static void test_pick_does_not_reach_a_scrolled_out_row(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity canvas = jce_scene_create_entity(s, "Canvas");
    JceCanvasComponent cv;
    memset(&cv, 0, sizeof cv);
    cv.render_mode = JCE_CANVAS_OVERLAY;
    cv.scale_factor = 1.0f;
    jce_scene_set_canvas(s, canvas, &cv);

    JceEntity view = jce_scene_create_entity(s, "Scroll");
    jce_scene_set_parent(s, view, canvas);
    JceUIScrollViewComponent sv;
    memset(&sv, 0, sizeof sv);
    sv.vertical = true;
    sv.interactable = true;
    sv.content_size[1] = 400.0f;
    set_rect(&sv.rect, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    sv.rect.size_delta[0] = 200.0f;
    sv.rect.size_delta[1] = 100.0f;
    jce_scene_set_ui_scroll_view(s, view, &sv);

    JceEntity row = jce_scene_create_entity(s, "Row");
    jce_scene_set_parent(s, row, view);
    JceUIImageComponent im;
    memset(&im, 0, sizeof im);
    im.color[0] = im.color[1] = im.color[2] = im.color[3] = 1.0f;
    im.raycast_target = true;
    set_rect(&im.rect, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, -200.0f);
    im.rect.size_delta[0] = 200.0f;
    im.rect.size_delta[1] = 40.0f;
    jce_scene_set_ui_image(s, row, &im);

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);
    JceUIPointer p; p.x = 0.0f; p.y = 0.0f; p.down = false; p.valid = false;
    jce_ui_canvas_render(uc, s, 0, 0, SCREEN_W, SCREEN_H, &p, 0.016f);

    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0u, jce_ui_canvas_pick(uc, 100.0f, 220.0f),
        "pick reached a row scrolled out of its viewport");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)view,
        jce_ui_canvas_pick(uc, 100.0f, 50.0f),
        "pick did not reach the scroll view itself inside its viewport");

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_top_anchor_negative_y_lands_below_the_top_edge);
    RUN_TEST(test_bottom_anchor_positive_y_lands_above_the_bottom_edge);
    RUN_TEST(test_centre_anchor_negative_y_lands_below_the_centre);
    RUN_TEST(test_pivot_moves_the_box_not_the_anchor);
    RUN_TEST(test_stretch_negative_size_delta_insets);
    RUN_TEST(test_stretch_offsets_match_the_inspector_identity);
    RUN_TEST(test_canvas_scale_factor_scales_px_metrics);
    RUN_TEST(test_zero_valued_text_fields_mean_the_legacy_behaviour);
    RUN_TEST(test_a_container_only_node_still_carries_its_children);
    RUN_TEST(test_a_nested_canvas_is_not_collected_as_a_root);
    RUN_TEST(test_a_scrolled_out_widget_is_not_clickable);
    RUN_TEST(test_an_open_popup_blocks_the_button_underneath);
    RUN_TEST(test_pick_agrees_with_the_click_and_reports_the_drawn_rect);
    RUN_TEST(test_pick_does_not_reach_a_scrolled_out_row);
    return UNITY_END();
}
