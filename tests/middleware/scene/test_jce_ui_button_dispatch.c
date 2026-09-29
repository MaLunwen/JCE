/* test_jce_ui_button_dispatch.c
 *
 * FEATURE 4.1 — making shipped in-game UIButtons clickable.
 *
 * Two halves, both exercising the REAL dynamic paths (no mocks / no
 * reimplementations):
 *
 *  1. The graphic-raycaster click state machine.  We author a real JceScene
 *     with a Canvas → UIImage(raycast_target) + UIButton at a KNOWN screen
 *     rect, then drive the public jce_ui_canvas_render() — which runs the
 *     actual uc_update_buttons() hit-test + press/release state machine — with
 *     a JceUIPointer that presses then releases.  We assert
 *     jce_ui_canvas_last_clicked() returns the button entity ONLY on the
 *     release-over-the-button frame, and 0 otherwise (press frame, idle frame,
 *     pointer outside, press-inside-release-outside).  The canvas is created
 *     HEADLESS (renderer == NULL) so the full layout + raycast + click state
 *     machine run with no live bgfx context.
 *
 *  2. The script-dispatch seam.  jce_runtime_dispatch_ui_click() ultimately
 *     calls jce_script_call_named(vm, handler, button_entity) — the same
 *     primitive a UIButton's on_click_handler is routed through.  We drive that
 *     primitive directly on a headless Lua VM (binding-less host with only a
 *     log callback that counts invocations) so we can assert the handler fires
 *     EXACTLY ONCE per dispatch, receives the clicked entity id, returns true,
 *     and that a missing / empty handler name is a clean no-op (returns false).
 *
 *  This covers the wiring DEAD-ENDs the feature closed: the canvas click
 *  recording (half 1) and the named-handler dispatch (half 2).  The remaining
 *  glue (jce_runtime_dispatch_ui_click + the default-main app_draw drain) is a
 *  thin pass-through over these two tested primitives; see followups.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_ui_canvas.h>
#include <jce/middleware/script/jce_script.h>

#include "unity.h"
#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Shared scene geometry ─────────────────────────────────────────────
 * A 200x200 screen.  The button child uses a fixed-size RectTransform
 * Authored the Unity UGUI way -- anchored to the parent's TOP-left corner
 * (anchor_min == anchor_max == {0,1}), pivot at the element's own top-left
 * ({0,1}), and anchoredPosition.y NEGATIVE because +Y is UP in UGUI.  That is
 * what puts it 20 px in from the top-left of the screen, so uc_resolve_rect
 * places
 * it deterministically at:
 *     x = anchored_position.x = 20, w = size_delta.x = 60
 *     y = anchored_position.y = 20, h = size_delta.y = 40
 * i.e. the button occupies screen rect (20,20)..(80,60). */
#define SCREEN_W   200.0f
#define SCREEN_H   200.0f
#define BTN_X      20.0f
#define BTN_Y      20.0f
#define BTN_W      60.0f
#define BTN_H      40.0f
/* A point comfortably inside the button rect, and one well outside it. */
#define IN_X       40.0f
#define IN_Y       40.0f
#define OUT_X      150.0f
#define OUT_Y      150.0f

static const char *HANDLER_NAME = "on_play_clicked";

/* Author canvas + button; return the button entity (out) and the scene. */
static JceScene *make_scene_with_button(JceEntity *out_button)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity canvas = jce_scene_create_entity(s, "Canvas");
    JceCanvasComponent cv;
    memset(&cv, 0, sizeof cv);
    cv.render_mode = JCE_CANVAS_OVERLAY;
    cv.sort_order  = 0;
    jce_scene_set_canvas(s, canvas, &cv);

    JceEntity button = jce_scene_create_entity(s, "PlayButton");
    jce_scene_set_parent(s, button, canvas);

    /* UIImage with raycast_target so the layout pass records a hit, and a
     * fixed-size RectTransform that resolves to the known rect above. */
    JceUIImageComponent im;
    memset(&im, 0, sizeof im);
    im.color[0] = im.color[1] = im.color[2] = im.color[3] = 1.0f;
    im.raycast_target = true;
    im.rect.anchor_min[0] = 0.0f; im.rect.anchor_min[1] = 1.0f;
    im.rect.anchor_max[0] = 0.0f; im.rect.anchor_max[1] = 1.0f;
    im.rect.pivot[0]      = 0.0f; im.rect.pivot[1]      = 1.0f;
    im.rect.anchored_position[0] = BTN_X; im.rect.anchored_position[1] = -BTN_Y;
    im.rect.size_delta[0]        = BTN_W; im.rect.size_delta[1]        = BTN_H;
    jce_scene_set_ui_image(s, button, &im);

    JceUIButtonComponent bt;
    memset(&bt, 0, sizeof bt);
    bt.interactable  = true;
    bt.fade_duration = 0.1f;
    bt.normal_color[3] = bt.highlighted_color[3] =
        bt.pressed_color[3] = bt.disabled_color[3] = 1.0f;
    strncpy(bt.on_click_handler, HANDLER_NAME, sizeof bt.on_click_handler - 1);
    jce_scene_set_ui_button(s, button, &bt);
    TEST_ASSERT_TRUE(jce_scene_has_ui_button(s, button));

    *out_button = button;
    return s;
}

/* Drive one render frame through the REAL canvas pipeline with the given
 * pointer state, then return what the click state machine recorded. */
static uint64_t step(JceUICanvas *uc, JceScene *s, float x, float y,
                     bool down, bool valid)
{
    JceUIPointer ptr;
    ptr.x = x; ptr.y = y; ptr.down = down; ptr.valid = valid;
    jce_ui_canvas_render(uc, s, /*view*/0, /*fb*/0, SCREEN_W, SCREEN_H,
                         valid ? &ptr : NULL, 1.0f / 60.0f);
    return jce_ui_canvas_last_clicked(uc);
}

/* ── Half 1: the real hit-test + click state machine ──────────────────── */

/* Press inside then release inside ⇒ click fires ONLY on the release frame. */
static void test_press_release_inside_clicks_on_release(void)
{
    JceEntity button = 0;
    JceScene *s = make_scene_with_button(&button);
    JceUICanvas *uc = jce_ui_canvas_create(/*renderer*/NULL, /*pak*/NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* Frame 0: hover only (no button down) → no click. */
    TEST_ASSERT_EQUAL_UINT64(0, step(uc, s, IN_X, IN_Y, false, true));

    /* Frame 1: press begins inside → no click yet (press, not release). */
    TEST_ASSERT_EQUAL_UINT64(0, step(uc, s, IN_X, IN_Y, true, true));

    /* Frame 2: still held inside → still no click. */
    TEST_ASSERT_EQUAL_UINT64(0, step(uc, s, IN_X, IN_Y, true, true));

    /* Frame 3: release inside → CLICK fires, reporting the button entity. */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)button,
                             step(uc, s, IN_X, IN_Y, false, true));

    /* Frame 4: idle after release → click latch cleared (fires once). */
    TEST_ASSERT_EQUAL_UINT64(0, step(uc, s, IN_X, IN_Y, false, true));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* Pressing and releasing entirely OUTSIDE the button never clicks it. */
static void test_press_release_outside_never_clicks(void)
{
    JceEntity button = 0;
    JceScene *s = make_scene_with_button(&button);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    TEST_ASSERT_EQUAL_UINT64(0, step(uc, s, OUT_X, OUT_Y, false, true));
    TEST_ASSERT_EQUAL_UINT64(0, step(uc, s, OUT_X, OUT_Y, true,  true));
    TEST_ASSERT_EQUAL_UINT64(0, step(uc, s, OUT_X, OUT_Y, false, true));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* Press inside but release OUTSIDE the button ⇒ Unity "drag off" — no click. */
static void test_press_inside_release_outside_no_click(void)
{
    JceEntity button = 0;
    JceScene *s = make_scene_with_button(&button);
    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    TEST_ASSERT_EQUAL_UINT64(0, step(uc, s, IN_X,  IN_Y,  true,  true)); /* press in  */
    TEST_ASSERT_EQUAL_UINT64(0, step(uc, s, OUT_X, OUT_Y, true,  true)); /* drag out  */
    TEST_ASSERT_EQUAL_UINT64(0, step(uc, s, OUT_X, OUT_Y, false, true)); /* release out */

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* A non-interactable button is driven to the disabled state and never clicks,
 * even on a clean press+release inside. */
static void test_non_interactable_button_never_clicks(void)
{
    JceEntity button = 0;
    JceScene *s = make_scene_with_button(&button);
    JceUIButtonComponent *bt = jce_scene_get_ui_button(s, button);
    TEST_ASSERT_NOT_NULL(bt);
    bt->interactable = false;

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    TEST_ASSERT_EQUAL_UINT64(0, step(uc, s, IN_X, IN_Y, true,  true));
    TEST_ASSERT_EQUAL_UINT64(0, step(uc, s, IN_X, IN_Y, false, true));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── Half 2: the named-handler script dispatch primitive ──────────────── */

/* Counter bumped by the Lua handler via the host log callback, plus the entity
 * id the handler echoed back so we can verify the argument round-trips. */
static int      g_handler_calls = 0;
static uint64_t g_handler_arg   = 0;

static void count_log(void *user, const char *msg)
{
    (void)user;
    g_handler_calls++;
    /* The handler logs the entity id it received; parse it back. */
    if (msg) {
        unsigned long long v = 0;
        if (sscanf(msg, "click:%llu", &v) == 1)
            g_handler_arg = (uint64_t)v;
    }
}

/* jce_script_call_named fires a registered global handler EXACTLY once, passing
 * the clicked entity id, and reports success. */
static void test_call_named_fires_handler_once(void)
{
    JceScriptHost host;
    memset(&host, 0, sizeof host);
    host.log = count_log;

    JceScript *vm = jce_script_create(&host);
    TEST_ASSERT_NOT_NULL(vm);

    /* A global handler that logs the entity id it was called with.  Declaring a
     * global function is the registration mechanism the feature relies on. */
    static const char *SRC =
        "function on_play_clicked(e)\n"
        "  jce.log('click:' .. tostring(e))\n"
        "end\n";
    JceScriptInstance inst =
        jce_script_instantiate_source(vm, "ui_handlers", SRC, /*owner*/0);
    /* The chunk has no return table, so instance creation may return 0; the
     * global function it defines is still installed.  Either way the global is
     * what we dispatch to. */
    (void)inst;

    g_handler_calls = 0;
    g_handler_arg   = 0;

    const uint64_t BTN = 4242;
    bool fired = jce_script_call_named(vm, HANDLER_NAME, (JceScriptEntity)BTN);

    TEST_ASSERT_TRUE(fired);                       /* a handler existed + ran  */
    TEST_ASSERT_EQUAL_INT(1, g_handler_calls);     /* EXACTLY once             */
    TEST_ASSERT_EQUAL_UINT64(BTN, g_handler_arg);  /* clicked entity passed in */

    /* A second dispatch fires it again exactly once more (idempotent seam). */
    bool fired2 = jce_script_call_named(vm, HANDLER_NAME, (JceScriptEntity)BTN);
    TEST_ASSERT_TRUE(fired2);
    TEST_ASSERT_EQUAL_INT(2, g_handler_calls);

    jce_script_destroy(vm);
}

/* Missing / empty handler names are a clean no-op (false, nothing fired) — the
 * "UIButton with no on_click_handler is just a visual button" contract. */
static void test_call_named_missing_handler_is_noop(void)
{
    JceScriptHost host;
    memset(&host, 0, sizeof host);
    host.log = count_log;

    JceScript *vm = jce_script_create(&host);
    TEST_ASSERT_NOT_NULL(vm);

    g_handler_calls = 0;

    TEST_ASSERT_FALSE(jce_script_call_named(vm, "no_such_handler", 1));
    TEST_ASSERT_FALSE(jce_script_call_named(vm, "", 1));
    TEST_ASSERT_FALSE(jce_script_call_named(vm, NULL, 1));
    TEST_ASSERT_FALSE(jce_script_call_named(NULL, HANDLER_NAME, 1));
    TEST_ASSERT_EQUAL_INT(0, g_handler_calls);     /* nothing fired */

    jce_script_destroy(vm);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_press_release_inside_clicks_on_release);
    RUN_TEST(test_press_release_outside_never_clicks);
    RUN_TEST(test_press_inside_release_outside_no_click);
    RUN_TEST(test_non_interactable_button_never_clicks);
    RUN_TEST(test_call_named_fires_handler_once);
    RUN_TEST(test_call_named_missing_handler_is_noop);
    return UNITY_END();
}
