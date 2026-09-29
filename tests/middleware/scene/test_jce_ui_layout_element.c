/* test_jce_ui_layout_element.c
 *
 * The two things a LayoutGroup could not express.
 *
 * 1. WEIGHTED SIZING.  control_child_size gave strictly EQUAL shares, so
 *    "this child takes twice the space" was inexpressible -- no Unity
 *    LayoutElement.flexibleWidth, no Godot size_flags_stretch_ratio, no Slate
 *    FSlateChildSize Fill weight.  With it off, a child's own rect was its
 *    size, so "at least this big" was inexpressible too.
 *
 * 2. GRID CONSTRAINT.  The column count came only from the parent's width, so
 *    a 3-column grid was not authorable: you resized the parent until three
 *    happened to fit, and it silently became four on a wider screen.  Nor was
 *    there a start corner or a start axis.
 *
 * Assertions read jce_ui_canvas_entity_rect -- where the child ACTUALLY DREW
 * after a real headless render (renderer == NULL: layout and raycast run, only
 * GPU draws are skipped).  Not the component, not an internal helper: a test
 * of the arithmetic would stay green if the packed rects were never consumed,
 * which is the exact shape this campaign keeps finding.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_ui_canvas.h>
#include <jce/os/core/jce_str.h>
#include <jce/renderer/jce_primitives.h>

#include "unity.h"
#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define SCREEN_W  400.0f
#define SCREEN_H  300.0f
#define EPS         0.6f

typedef struct { float x, y, w, h; } Rect;

static JceScene   *g_scene;
static JceUICanvas *g_uc;
static JceEntity   g_root;

static void begin(int kind)
{
    g_scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(g_scene);
    g_root = jce_scene_create_entity(g_scene, "Canvas");

    JceCanvasComponent cv;
    memset(&cv, 0, sizeof cv);
    cv.render_mode = JCE_CANVAS_OVERLAY;
    jce_scene_set_canvas(g_scene, g_root, &cv);

    JceLayoutGroupComponent lg;
    memset(&lg, 0, sizeof lg);
    lg.layout_kind = kind;
    jce_scene_set_layout_group(g_scene, g_root, &lg);

    g_uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(g_uc);
}

static JceLayoutGroupComponent *group(void)
{
    JceLayoutGroupComponent *lg = jce_scene_get_layout_group(g_scene, g_root);
    TEST_ASSERT_NOT_NULL(lg);
    return lg;
}

static void finish(void)
{
    jce_ui_canvas_destroy(g_uc);
    jce_scene_destroy(g_scene);
    g_uc = NULL; g_scene = NULL;
}

/* One child with its own size, optionally carrying a LayoutElement. */
static JceEntity child(const char *name, float w, float h)
{
    JceEntity e = jce_scene_create_entity(g_scene, name);
    jce_scene_set_parent(g_scene, e, g_root);

    JceUIImageComponent im;
    memset(&im, 0, sizeof im);
    im.color[3] = 1.0f;
    im.raycast_target = true;
    im.rect.anchor_min[0] = 0.0f; im.rect.anchor_min[1] = 1.0f;
    im.rect.anchor_max[0] = 0.0f; im.rect.anchor_max[1] = 1.0f;
    im.rect.pivot[0]      = 0.0f; im.rect.pivot[1]      = 1.0f;
    im.rect.size_delta[0] = w;
    im.rect.size_delta[1] = h;
    jce_scene_set_ui_image(g_scene, e, &im);
    return e;
}

static void elem(JceEntity e, float pref_w, float flex_w, float min_w)
{
    JceLayoutElementComponent le;
    memset(&le, 0, sizeof le);
    le.preferred_width  = pref_w;
    le.preferred_height = -1.0f;
    le.flexible_width   = flex_w;
    le.min_width        = min_w;
    jce_scene_set_layout_element(g_scene, e, &le);
}

static void render(void)
{
    jce_ui_canvas_render(g_uc, g_scene, 0, 0, SCREEN_W, SCREEN_H, NULL,
                         1.0f / 60.0f);
}

static Rect rect_of(JceEntity e)
{
    Rect r;
    TEST_ASSERT_TRUE_MESSAGE(
        jce_ui_canvas_entity_rect(g_uc, (uint64_t)e, &r.x, &r.y, &r.w, &r.h),
        "the child recorded no rect at all");
    return r;
}

/* ── 1. weight: the thing that could not be said ──────────────────────── */
static void test_flexible_weights_split_the_leftover(void)
{
    begin(JCE_LAYOUT_HORIZONTAL);
    JceEntity a = child("A", 50.0f, 40.0f);
    JceEntity b = child("B", 50.0f, 40.0f);
    /* Both ask for 50 of the 400 available; 300 is left over and the weights
     * are 1 and 2, so A gets 100 more and B gets 200 more. */
    elem(a, 50.0f, 1.0f, 0.0f);
    elem(b, 50.0f, 2.0f, 0.0f);
    render();

    Rect ra = rect_of(a), rb = rect_of(b);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 150.0f, ra.w,
        "weight 1 of 3 did not take one third of the leftover");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 250.0f, rb.w,
        "weight 2 of 3 did not take two thirds of the leftover");
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, ra.x);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 150.0f, rb.x);
    finish();
}

/* ── 2. the negative control for weight ───────────────────────────────── */
static void test_no_layout_element_lays_out_exactly_as_before(void)
{
    /* Every scene authored before LayoutElement existed must be untouched, so
     * this is the case that matters most: two plain children keep their own
     * widths and pack from the start, which is what the group always did. */
    begin(JCE_LAYOUT_HORIZONTAL);
    JceEntity a = child("A", 50.0f, 40.0f);
    JceEntity b = child("B", 90.0f, 40.0f);
    render();

    Rect ra = rect_of(a), rb = rect_of(b);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 50.0f, ra.w,
        "a child with no LayoutElement lost its own width");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 90.0f, rb.w,
        "a child with no LayoutElement lost its own width");
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, ra.x);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 50.0f, rb.x);
    finish();
}

/* ── 3. a zero weight does not grow ───────────────────────────────────── */
static void test_zero_weight_keeps_its_preferred_size(void)
{
    /* The header-and-body case, which is the whole reason the feature exists:
     * one child stays put and the other takes the rest. */
    begin(JCE_LAYOUT_HORIZONTAL);
    JceEntity head = child("Head", 50.0f, 40.0f);
    JceEntity body = child("Body", 50.0f, 40.0f);
    elem(head, 80.0f, 0.0f, 0.0f);
    elem(body, 50.0f, 1.0f, 0.0f);
    render();

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 80.0f, rect_of(head).w,
        "a zero-weight child grew");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 320.0f, rect_of(body).w,
        "the only flexible child did not take all the leftover");
    finish();
}

/* ── 4. preferred < 0 means no opinion ────────────────────────────────── */
static void test_preferred_minus_one_falls_back_to_the_own_size(void)
{
    /* -1 must behave exactly like having no component: adding a
     * LayoutElement and leaving it alone cannot change the picture, or the
     * component is a trap. */
    begin(JCE_LAYOUT_HORIZONTAL);
    JceEntity a = child("A", 70.0f, 40.0f);
    elem(a, -1.0f, 0.0f, 0.0f);
    render();
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 70.0f, rect_of(a).w,
        "preferred = -1 did not fall back to the element's own size");
    finish();
}

/* ── 5. min raises, and only raises ───────────────────────────────────── */
static void test_min_width_is_a_floor_not_a_size(void)
{
    begin(JCE_LAYOUT_HORIZONTAL);
    JceEntity small = child("S", 10.0f, 40.0f);
    JceEntity big   = child("B", 10.0f, 40.0f);
    elem(small, 10.0f, 0.0f, 120.0f);   /* asks 10, floor 120 -> 120 */
    elem(big,   200.0f, 0.0f, 20.0f);   /* asks 200, floor 20  -> 200 */
    render();

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 120.0f, rect_of(small).w,
        "min did not raise a child under it");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 200.0f, rect_of(big).w,
        "min shrank a child that was already bigger -- it is a floor");
    finish();
}

/* ── 6. the grid column count is now authorable ───────────────────────── */
static void test_fixed_column_count_holds_whatever_the_width(void)
{
    /* Cells are 60 wide in a 400-wide parent, so the flexible rule would give
     * six columns.  Asking for three must give three -- which before this
     * could only be arranged by resizing the parent until it happened. */
    begin(JCE_LAYOUT_GRID);
    JceLayoutGroupComponent *lg = group();
    lg->cell_size[0] = 60.0f;
    lg->cell_size[1] = 30.0f;
    lg->grid_constraint = (uint8_t)JCE_GRID_FIXED_COLUMNS;
    lg->grid_constraint_count = 3;

    JceEntity e[6];
    for (int i = 0; i < 6; i++) {
        char nm[16]; snprintf(nm, sizeof nm, "C%d", i);
        e[i] = child(nm, 10.0f, 10.0f);
    }
    render();

    /* Items 0,1,2 on one row; item 3 starts the next. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, rect_of(e[0]).y, rect_of(e[2]).y);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 120.0f, rect_of(e[2]).x,
        "the third cell is not in the third column");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, rect_of(e[0]).x, rect_of(e[3]).x,
        "the fourth cell did not wrap to the start of the next row");
    TEST_ASSERT_TRUE_MESSAGE(rect_of(e[3]).y > rect_of(e[0]).y + 1.0f,
        "the fourth cell did not move down a row");
    finish();
}

/* ── 7. the negative control for the grid ─────────────────────────────── */
static void test_flexible_grid_still_fills_the_width(void)
{
    /* Constraint 0 is what every grid authored before this holds.  It must
     * still derive the column count from the width: six 60-wide cells across
     * 400 px means item 3 is on the FIRST row, not the second. */
    begin(JCE_LAYOUT_GRID);
    JceLayoutGroupComponent *lg = group();
    lg->cell_size[0] = 60.0f;
    lg->cell_size[1] = 30.0f;

    JceEntity e[6];
    for (int i = 0; i < 6; i++) {
        char nm[16]; snprintf(nm, sizeof nm, "C%d", i);
        e[i] = child(nm, 10.0f, 10.0f);
    }
    render();
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, rect_of(e[0]).y, rect_of(e[3]).y,
        "a flexible grid stopped filling the parent's width");
    finish();
}

/* ── 8. start axis changes where item 1 goes ──────────────────────────── */
static void test_vertical_start_axis_fills_a_column_first(void)
{
    begin(JCE_LAYOUT_GRID);
    JceLayoutGroupComponent *lg = group();
    lg->cell_size[0] = 60.0f;
    lg->cell_size[1] = 30.0f;
    lg->grid_constraint = (uint8_t)JCE_GRID_FIXED_COLUMNS;
    lg->grid_constraint_count = 3;
    lg->grid_start_axis = (uint8_t)JCE_GRID_AXIS_VERTICAL;

    JceEntity e[6];
    for (int i = 0; i < 6; i++) {
        char nm[16]; snprintf(nm, sizeof nm, "C%d", i);
        e[i] = child(nm, 10.0f, 10.0f);
    }
    render();

    /* Column-major: item 1 sits BELOW item 0, not beside it.  This is not the
     * same picture with the cells renamed -- it is where item 1 is. */
    Rect r0 = rect_of(e[0]), r1 = rect_of(e[1]);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, r0.x, r1.x,
        "vertical start axis put item 1 in the next column");
    TEST_ASSERT_TRUE_MESSAGE(r1.y > r0.y + 1.0f,
        "vertical start axis did not put item 1 below item 0");
    finish();
}

/* ── 9. start corner mirrors the order, not the lattice ───────────────── */
static void test_upper_right_corner_mirrors_the_columns(void)
{
    begin(JCE_LAYOUT_GRID);
    JceLayoutGroupComponent *lg = group();
    lg->cell_size[0] = 60.0f;
    lg->cell_size[1] = 30.0f;
    lg->grid_constraint = (uint8_t)JCE_GRID_FIXED_COLUMNS;
    lg->grid_constraint_count = 3;
    lg->grid_start_corner = (uint8_t)JCE_GRID_CORNER_UPPER_RIGHT;

    JceEntity e[3];
    for (int i = 0; i < 3; i++) {
        char nm[16]; snprintf(nm, sizeof nm, "C%d", i);
        e[i] = child(nm, 10.0f, 10.0f);
    }
    render();

    /* Item 0 is now in the RIGHTMOST of the three columns, and the run walks
     * left.  The lattice is unchanged: the three x positions are still the
     * same three, only their occupants swapped. */
    Rect r0 = rect_of(e[0]), r2 = rect_of(e[2]);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 120.0f, r0.x,
        "upper-right did not put the first cell in the last column");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 0.0f, r2.x,
        "upper-right did not walk the run leftwards");
    TEST_ASSERT_FLOAT_WITHIN(EPS, r0.y, r2.y);
    finish();
}

/* ── 10. ignore_layout: out of the run entirely ──────────────────── */
static void test_ignored_child_keeps_its_own_rect_and_leaves_no_gap(void)
{
    /* A tooltip or a drag ghost parented into a list.  Two things must hold
     * and only one of them is obvious: the ignored child keeps its own rect,
     * AND the others pack as if it were not there -- including its spacing
     * gap, which is the half a skip-the-placement-only fix would miss. */
    begin(JCE_LAYOUT_HORIZONTAL);
    group()->spacing[0] = 10.0f;

    JceEntity a     = child("A", 60.0f, 40.0f);
    JceEntity ghost = child("Ghost", 33.0f, 40.0f);
    JceEntity b     = child("B", 60.0f, 40.0f);

    JceLayoutElementComponent le;
    memset(&le, 0, sizeof le);
    le.preferred_width = -1.0f; le.preferred_height = -1.0f;
    le.ignore_layout = true;
    jce_scene_set_layout_element(g_scene, ghost, &le);
    render();

    Rect ra = rect_of(a), rb = rect_of(b), rg = rect_of(ghost);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, ra.x);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 70.0f, rb.x,
        "B did not pack as if the ignored child were absent (60 + one gap)");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 33.0f, rg.w,
        "the ignored child was resized by the layout");
    finish();
}

/* ── 11. bottom-up: a row sized by what its own children need ───── */
static void test_a_group_sizes_a_nested_group_to_its_content(void)
{
    /* The gap this closes.  The arrange used to size a child by its AUTHORED
     * rect, so a row that is itself a container came out at whatever height
     * someone happened to type rather than at the height its contents need --
     * and that rect is exactly what an author has least reason to have set
     * correctly, because the point of nesting a group is not having to.
     *
     * TEXT is the other half of this feature and is NOT asserted here: a
     * headless canvas has no pak and no asset root, so uc_get_font returns
     * NULL and there is nothing to shape.  Worse, jce_font_open_* refuses to
     * return a font whose atlas texture is invalid, and a unit test has no
     * bgfx device -- so text measurement is unassertable in this process, not
     * merely inconvenient.  A nested group needs no font, which is why it is
     * the case that can carry the claim.
     *
     * The outer column holds two rows.  Both are authored 30 px tall.  The
     * SECOND is a vertical group holding three 50 px children, so it NEEDS
     * 150 px plus its spacing.  If the authored rect still decided, the two
     * rows would come out equal. */
    begin(JCE_LAYOUT_VERTICAL);
    JceEntity plain = child("Plain", 200.0f, 30.0f);
    JceEntity nested = child("Nested", 200.0f, 30.0f);

    JceLayoutGroupComponent inner;
    memset(&inner, 0, sizeof inner);
    inner.layout_kind = JCE_LAYOUT_VERTICAL;
    jce_scene_set_layout_group(g_scene, nested, &inner);
    for (int i = 0; i < 3; i++) {
        char nm[16]; snprintf(nm, sizeof nm, "Leaf%d", i);
        JceEntity leaf = jce_scene_create_entity(g_scene, nm);
        jce_scene_set_parent(g_scene, leaf, nested);
        JceUIImageComponent im;
        memset(&im, 0, sizeof im);
        im.color[3] = 1.0f;
        im.raycast_target = true;
        im.rect.anchor_min[0] = 0.0f; im.rect.anchor_min[1] = 1.0f;
        im.rect.anchor_max[0] = 0.0f; im.rect.anchor_max[1] = 1.0f;
        im.rect.pivot[0]      = 0.0f; im.rect.pivot[1]      = 1.0f;
        im.rect.size_delta[0] = 180.0f;
        im.rect.size_delta[1] = 50.0f;
        jce_scene_set_ui_image(g_scene, leaf, &im);
    }
    render();

    Rect rp = rect_of(plain), rn = rect_of(nested);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 30.0f, rp.h,
        "the plain row lost its authored height");
    TEST_ASSERT_TRUE_MESSAGE(rn.h > 100.0f,
        "the nested group was laid out at its authored 30 px instead of the "
        "150 px its own children need -- the arrange is not asking");
    TEST_ASSERT_TRUE_MESSAGE(rn.y > rp.y,
        "the second row did not follow the first down the column");
    finish();
}

/* ── 12. the control: a child that cannot say keeps its rect ────── */
static void test_a_child_with_no_content_keeps_its_authored_size(void)
{
    /* A plain UIImage has no content to measure, so it must still be laid out
     * at the size it was authored with.  A measure pass that returned zero for
     * anything it could not measure would collapse every image in every
     * group -- which is a far worse failure than the one it fixes. */
    begin(JCE_LAYOUT_VERTICAL);
    JceEntity a = child("A", 100.0f, 44.0f);
    JceEntity b = child("B", 100.0f, 77.0f);
    render();
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 44.0f, rect_of(a).h,
        "an unmeasurable child lost its authored height");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(EPS, 77.0f, rect_of(b).h,
        "an unmeasurable child lost its authored height");
    finish();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_flexible_weights_split_the_leftover);
    RUN_TEST(test_no_layout_element_lays_out_exactly_as_before);
    RUN_TEST(test_zero_weight_keeps_its_preferred_size);
    RUN_TEST(test_preferred_minus_one_falls_back_to_the_own_size);
    RUN_TEST(test_min_width_is_a_floor_not_a_size);
    RUN_TEST(test_fixed_column_count_holds_whatever_the_width);
    RUN_TEST(test_flexible_grid_still_fills_the_width);
    RUN_TEST(test_vertical_start_axis_fills_a_column_first);
    RUN_TEST(test_upper_right_corner_mirrors_the_columns);
    RUN_TEST(test_ignored_child_keeps_its_own_rect_and_leaves_no_gap);
    RUN_TEST(test_a_group_sizes_a_nested_group_to_its_content);
    RUN_TEST(test_a_child_with_no_content_keeps_its_authored_size);
    return UNITY_END();
}
