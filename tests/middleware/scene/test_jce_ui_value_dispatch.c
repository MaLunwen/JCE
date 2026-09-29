/* test_jce_ui_value_dispatch.c
 *
 * Making shipped in-game UI widgets DO something on value change — the
 * companions to test_jce_ui_button_dispatch.c for the non-button widgets
 * (UISlider / UIToggle / UIDropdown value changes + UIInputField edits/submit).
 *
 * Two halves, both exercising the REAL dynamic paths (no mocks):
 *
 *  1. The canvas value/text/submit RECORDING.  We author a real headless
 *     JceScene (renderer == NULL) with a Canvas + one widget at a KNOWN screen
 *     rect, drive jce_ui_canvas_render() / text_input() / key_edit() with a
 *     synthetic pointer + keys, and assert the canvas records the changed
 *     widget's entity via jce_ui_canvas_last_value_changed() (slider drag /
 *     toggle flip / dropdown select), jce_ui_canvas_last_text_changed()
 *     (input-field edit) and jce_ui_canvas_last_submitted() (RETURN) — and ONLY
 *     on the change frame, 0 otherwise.
 *
 *  2. The named-handler-with-arg script dispatch primitives.
 *     jce_runtime_dispatch_ui_value_changed() ultimately calls
 *     jce_script_call_named_num(vm, handler, entity, value); the input-field
 *     seams call jce_script_call_named_str(vm, handler, entity, text).  We drive
 *     those primitives directly on a headless Lua VM (a binding-less host with
 *     only a log callback that counts invocations + echoes the args) so we can
 *     assert each handler fires EXACTLY ONCE, receives the entity id AND the
 *     value/text, returns true, and that a missing handler is a clean no-op.
 *
 *  The remaining glue (jce_runtime_dispatch_ui_* + the default-main / editor
 *  drain) is a thin pass-through over these two tested primitives — exactly the
 *  scope split of test_jce_ui_button_dispatch.c.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_ui_canvas.h>
#include <jce/middleware/script/jce_script.h>
#include <jce/os/platform/jce_keys.h>

#include "unity.h"
#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define SCREEN_W 200.0f
#define SCREEN_H 200.0f

/* Fixed-size RectTransform of (w,h) flush with the parent TOP-left.
 *
 * Spelled out in UGUI terms.  The memset zeros meant anchor/pivot {0,0}, which
 * is UGUI's BOTTOM-left, so this helper only produced (0,0)..(w,h) while the
 * resolver was reading the Y axis upside-down; every widget it placed would
 * otherwise sit at the bottom of the screen and the fixed click coordinates
 * below would miss it. */
static void set_rect(JceRectTransform *rt, float w, float h)
{
    memset(rt, 0, sizeof *rt);
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
    jce_scene_set_canvas(s, canvas, &cv);
    *out_canvas = canvas;
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

/* ── Half 1a: slider drag records last_value_changed ──────────────────── */
static void test_slider_drag_records_value_changed(void)
{
    JceEntity canvas = 0;
    JceScene *s = make_canvas(&canvas);
    JceEntity sld = jce_scene_create_entity(s, "Slider");
    jce_scene_set_parent(s, sld, canvas);

    JceUISliderComponent sl;
    memset(&sl, 0, sizeof sl);
    sl.value = 0.0f; sl.min_value = 0.0f; sl.max_value = 1.0f;
    sl.direction = 0;            /* L→R */
    sl.interactable = true;
    sl.bg_color[3] = sl.fill_color[3] = sl.handle_color[3] = 1.0f;
    set_rect(&sl.rect, SCREEN_W, 40.0f);   /* (0,0)..(200,40) */
    jce_scene_set_ui_slider(s, sld, &sl);

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* Hover only → no change. */
    step(uc, s, 100.0f, 20.0f, false, true);
    TEST_ASSERT_EQUAL_UINT64(0u, jce_ui_canvas_last_value_changed(uc));

    /* Press over the mid-point → value 0 → 0.5 → records this slider. */
    step(uc, s, 100.0f, 20.0f, true, true);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)sld, jce_ui_canvas_last_value_changed(uc));
    JceUISliderComponent *got = jce_scene_get_ui_slider(s, sld);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_TRUE(got->value > 0.4f && got->value < 0.6f);

    /* Hold at the SAME spot → value unchanged → not re-recorded. */
    step(uc, s, 100.0f, 20.0f, true, true);
    TEST_ASSERT_EQUAL_UINT64(0u, jce_ui_canvas_last_value_changed(uc));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── Half 1b: toggle flip records last_value_changed (release frame) ───── */
static void test_toggle_flip_records_value_changed(void)
{
    JceEntity canvas = 0;
    JceScene *s = make_canvas(&canvas);
    JceEntity tog = jce_scene_create_entity(s, "Toggle");
    jce_scene_set_parent(s, tog, canvas);

    JceUIToggleComponent tg;
    memset(&tg, 0, sizeof tg);
    tg.is_on = false;
    tg.interactable = true;
    tg.bg_color[3] = tg.checkmark_color[3] = 1.0f;
    set_rect(&tg.rect, 40.0f, 40.0f);   /* (0,0)..(40,40) */
    jce_scene_set_ui_toggle(s, tog, &tg);

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* Press (no flip yet) → no record. */
    step(uc, s, 20.0f, 20.0f, true, true);
    TEST_ASSERT_EQUAL_UINT64(0u, jce_ui_canvas_last_value_changed(uc));

    /* Release over the toggle → is_on flips → records. */
    step(uc, s, 20.0f, 20.0f, false, true);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)tog, jce_ui_canvas_last_value_changed(uc));
    JceUIToggleComponent *got = jce_scene_get_ui_toggle(s, tog);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_TRUE(got->is_on);

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── Half 1c: dropdown select records last_value_changed ───────────────── */
static void test_dropdown_select_records_value_changed(void)
{
    JceEntity canvas = 0;
    JceScene *s = make_canvas(&canvas);
    JceEntity dd = jce_scene_create_entity(s, "Dropdown");
    jce_scene_set_parent(s, dd, canvas);

    JceUIDropdownComponent d;
    memset(&d, 0, sizeof d);
    snprintf(d.options[0], sizeof d.options[0], "Alpha");
    snprintf(d.options[1], sizeof d.options[1], "Beta");
    snprintf(d.options[2], sizeof d.options[2], "Gamma");
    d.option_count = 3;
    d.selected_index = 0;
    d.expanded = true;             /* popup already open */
    d.interactable = true;
    d.bg_color[3] = d.text_color[3] = d.popup_color[3] = d.highlight_color[3] = 1.0f;
    d.font_size = 16.0f;
    set_rect(&d.rect, SCREEN_W, 40.0f);   /* main (0,0)..(200,40); row1 (0,40)..(200,80) */
    jce_scene_set_ui_dropdown(s, dd, &d);

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* Option rows stack below the main rect: row 0 = y[40,80), row 1 = y[80,120).
     * Press over row 1 (y=100) then release over it → selection 0 → 1. */
    step(uc, s, 100.0f, 100.0f, true,  true);
    TEST_ASSERT_EQUAL_UINT64(0u, jce_ui_canvas_last_value_changed(uc)); /* press: no commit */
    step(uc, s, 100.0f, 100.0f, false, true);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)dd, jce_ui_canvas_last_value_changed(uc));
    JceUIDropdownComponent *got = jce_scene_get_ui_dropdown(s, dd);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_INT(1, got->selected_index);

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── Half 1d: input-field edit + submit record their channels ──────────── */
static void test_inputfield_edit_and_submit_record(void)
{
    JceEntity canvas = 0;
    JceScene *s = make_canvas(&canvas);
    JceEntity fld = jce_scene_create_entity(s, "InputField");
    jce_scene_set_parent(s, fld, canvas);

    JceUIInputFieldComponent f;
    memset(&f, 0, sizeof f);
    f.content_type = 0;            /* any */
    f.interactable = true;
    f.bg_color[3] = f.text_color[3] = f.caret_color[3] = 1.0f;
    f.font_size = 16.0f;
    set_rect(&f.rect, SCREEN_W, SCREEN_H);
    jce_scene_set_ui_input_field(s, fld, &f);

    JceUICanvas *uc = jce_ui_canvas_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(uc);

    /* Focus the field (press + release over it). */
    step(uc, s, 100.0f, 100.0f, true,  true);
    step(uc, s, 100.0f, 100.0f, false, true);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)fld, jce_ui_canvas_focused_input(uc));

    /* No edit yet → text-changed channel empty. */
    TEST_ASSERT_EQUAL_UINT64(0u, jce_ui_canvas_last_text_changed(uc));

    /* Type → records the field (on_value_changed channel). */
    jce_ui_canvas_text_input(uc, "hi");
    TEST_ASSERT_EQUAL_UINT64((uint64_t)fld, jce_ui_canvas_last_text_changed(uc));
    /* Cleared on read: a second read is 0. */
    TEST_ASSERT_EQUAL_UINT64(0u, jce_ui_canvas_last_text_changed(uc));

    /* Backspace edits too → records again. */
    jce_ui_canvas_key_edit(uc, JCE_KEY_BACKSPACE, 0);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)fld, jce_ui_canvas_last_text_changed(uc));

    /* No submit yet. */
    TEST_ASSERT_EQUAL_UINT64(0u, jce_ui_canvas_last_submitted(uc));
    /* RETURN → submit channel records the field. */
    jce_ui_canvas_key_edit(uc, JCE_KEY_RETURN, 0);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)fld, jce_ui_canvas_last_submitted(uc));
    /* Cleared on read. */
    TEST_ASSERT_EQUAL_UINT64(0u, jce_ui_canvas_last_submitted(uc));

    jce_ui_canvas_destroy(uc);
    jce_scene_destroy(s);
}

/* ── Half 2: the named-handler-with-arg script dispatch primitives ─────── */

static int      g_calls   = 0;
static uint64_t g_arg_ent = 0;
static double   g_arg_num  = 0.0;
static char     g_arg_str[64];

static void count_log(void *user, const char *msg)
{
    (void)user;
    g_calls++;
    if (!msg) return;
    unsigned long long e = 0;
    double v = 0.0;
    char sbuf[64];
    if (sscanf(msg, "num:%llu:%lf", &e, &v) == 2) {
        g_arg_ent = (uint64_t)e; g_arg_num = v;
    } else if (sscanf(msg, "str:%llu:%63s", &e, sbuf) == 2) {
        g_arg_ent = (uint64_t)e;
        snprintf(g_arg_str, sizeof g_arg_str, "%s", sbuf);
    }
}

static JceScript *make_vm_with_handlers(void)
{
    JceScriptHost host;
    memset(&host, 0, sizeof host);
    host.log = count_log;
    JceScript *vm = jce_script_create(&host);
    TEST_ASSERT_NOT_NULL(vm);

    static const char *SRC =
        "function on_value(e, v)\n"
        "  jce.log('num:' .. tostring(e) .. ':' .. string.format('%.4f', v))\n"
        "end\n"
        "function on_text(e, t)\n"
        "  jce.log('str:' .. tostring(e) .. ':' .. tostring(t))\n"
        "end\n";
    (void)jce_script_instantiate_source(vm, "ui_handlers", SRC, /*owner*/0);
    return vm;
}

/* call_named_num fires a global handler once with (entity, value). */
static void test_call_named_num_passes_value(void)
{
    JceScript *vm = make_vm_with_handlers();

    g_calls = 0; g_arg_ent = 0; g_arg_num = 0.0;
    const uint64_t ENT = 7777;
    bool fired = jce_script_call_named_num(vm, "on_value",
                                           (JceScriptEntity)ENT, 0.5);
    TEST_ASSERT_TRUE(fired);
    TEST_ASSERT_EQUAL_INT(1, g_calls);
    TEST_ASSERT_EQUAL_UINT64(ENT, g_arg_ent);
    TEST_ASSERT_TRUE(g_arg_num > 0.49 && g_arg_num < 0.51);

    /* Missing handler → clean no-op (false, nothing fired). */
    TEST_ASSERT_FALSE(jce_script_call_named_num(vm, "no_such", ENT, 1.0));
    TEST_ASSERT_FALSE(jce_script_call_named_num(vm, "", ENT, 1.0));
    TEST_ASSERT_FALSE(jce_script_call_named_num(NULL, "on_value", ENT, 1.0));
    TEST_ASSERT_EQUAL_INT(1, g_calls);   /* unchanged */

    jce_script_destroy(vm);
}

/* call_named_str fires a global handler once with (entity, text). */
static void test_call_named_str_passes_text(void)
{
    JceScript *vm = make_vm_with_handlers();

    g_calls = 0; g_arg_ent = 0; g_arg_str[0] = '\0';
    const uint64_t ENT = 3131;
    bool fired = jce_script_call_named_str(vm, "on_text",
                                           (JceScriptEntity)ENT, "Bob");
    TEST_ASSERT_TRUE(fired);
    TEST_ASSERT_EQUAL_INT(1, g_calls);
    TEST_ASSERT_EQUAL_UINT64(ENT, g_arg_ent);
    TEST_ASSERT_EQUAL_STRING("Bob", g_arg_str);

    /* NULL text is tolerated (handler sees nil → "nil"). */
    g_calls = 0;
    TEST_ASSERT_TRUE(jce_script_call_named_str(vm, "on_text", ENT, NULL));
    TEST_ASSERT_EQUAL_INT(1, g_calls);

    /* Missing handler → clean no-op. */
    TEST_ASSERT_FALSE(jce_script_call_named_str(vm, "no_such", ENT, "x"));
    TEST_ASSERT_FALSE(jce_script_call_named_str(NULL, "on_text", ENT, "x"));

    jce_script_destroy(vm);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_slider_drag_records_value_changed);
    RUN_TEST(test_toggle_flip_records_value_changed);
    RUN_TEST(test_dropdown_select_records_value_changed);
    RUN_TEST(test_inputfield_edit_and_submit_record);
    RUN_TEST(test_call_named_num_passes_value);
    RUN_TEST(test_call_named_str_passes_text);
    return UNITY_END();
}
