/*
 * test_jce_input_gamepad_actions.c
 *
 * The gamepad-to-action chain, driven the way the hardware drives it.
 *
 * Every other gamepad-to-action test in this tree injects state with
 * jce_input_apply() and a hand-built JceInputFrame.  That is a REPLAY door: it
 * writes pad slots directly and never asks the state machine to open a device,
 * so it stays green even when nothing can ever open a pad.  That is exactly how
 * a shipped build reached users with the SDL gamepad backend never installed
 * while the suite was fully green.
 *
 * So everything here arrives through jce_input_submit() -- SEAM B, the same
 * door the SDL translator pushes into -- and every pad here has to be OPENED by
 * a DEVICE_ADDED event first, through a backend whose open_device is recorded.
 * Break the device-open path, the translator's event shape, or the axis
 * evaluator and these assertions fail.
 *
 * The subject is the merge that gives a keyboard-only action map its gamepad
 * bindings (jce_actions_merge_gamepad_defaults), because a map loaded from disk
 * is the ONLY thing the runtime ever uses -- jce_actions_bind_fps_defaults runs
 * only when every load path fails.
 */

#include "unity.h"

#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_actions.h>
#include <jce/os/platform/jce_input_device.h>
#include <jce/os/platform/jce_input_event.h>
#include <jce/os/platform/jce_gamepad.h>
#include <jce/os/platform/jce_keys.h>

#include <string.h>

/* The exact shape of the map the editor writes and the engine loads: eight
 * actions, six bindings, every one of them type 0 (keyboard).  Copied from the
 * repo's own .jce/input_actions.json -- this is the file that made a plugged-in
 * pad dead. */
static const char k_keyboard_only_map[] =
    "{\"actions\":["
    "{\"name\":\"move_forward\",\"binds\":[{\"type\":0,\"code\":26,\"scale\":1,\"deadzone\":0.15}]},"
    "{\"name\":\"move_back\",\"binds\":[{\"type\":0,\"code\":22,\"scale\":1,\"deadzone\":0.15}]},"
    "{\"name\":\"move_left\",\"binds\":[{\"type\":0,\"code\":4,\"scale\":1,\"deadzone\":0.15}]},"
    "{\"name\":\"move_right\",\"binds\":[{\"type\":0,\"code\":7,\"scale\":1,\"deadzone\":0.15}]},"
    "{\"name\":\"jump\",\"binds\":[{\"type\":0,\"code\":44,\"scale\":1,\"deadzone\":0.15}]},"
    "{\"name\":\"sprint\",\"binds\":[{\"type\":0,\"code\":225,\"scale\":1,\"deadzone\":0.15}]},"
    "{\"name\":\"look_x\",\"binds\":[]},"
    "{\"name\":\"look_y\",\"binds\":[]}"
    "]}";

/* ── A recording backend, so "the device was opened" is an assertion ──── */

typedef struct {
    int      opens;
    int      closes;
    uint64_t last_open;
} FakeBackend;

static FakeBackend     g_fake;
static JceInputBackend g_backend;

static bool fake_open(void *user, uint64_t instance, JceInputDeviceInfo *out)
{
    FakeBackend *f = (FakeBackend *)user;
    f->opens++;
    f->last_open = instance;
    if (out) {
        out->cls    = JCE_DEVCLASS_GAMEPAD;
        out->layout = JCE_INPUT_LAYOUT_GAMEPAD;
    }
    return true;
}

static void fake_close(void *user, uint64_t instance)
{
    FakeBackend *f = (FakeBackend *)user;
    f->closes++;
    (void)instance;
}

static JceInput        *g_input;
static JceInputActions *g_actions;

/* The instance id the "Xbox pad" arrives as. */
#define PAD_INSTANCE 7u

static JceInputEvent mk(int kind)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size = (uint32_t)sizeof(JceInputEvent);
    ev.kind = kind;
    return ev;
}

/* Plug a pad in, the way the translator does. */
static void plug_in_pad(void)
{
    JceInputEvent ev = mk(JCE_INPUT_EVENT_DEVICE_ADDED);
    ev.device.instance = PAD_INSTANCE;
    ev.device.cls      = (uint8_t)JCE_DEVCLASS_GAMEPAD;
    ev.device.layout   = (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD;
    jce_input_submit(g_input, &ev, 1);
}

/* Move one stick axis and re-evaluate the map.  `value` is already normalised
 * to [-1,1] -- that is the producer's contract, per jce_input_event.h. */
static void move_axis(int axis, float value)
{
    JceInputEvent ev = mk(JCE_INPUT_EVENT_DEVICE_AXIS);
    ev.daxis.instance = PAD_INSTANCE;
    ev.daxis.axis     = axis;
    ev.daxis.value    = value;
    ev.daxis.semantic = 1;            /* code is a JceGamepadAxis */
    jce_input_submit(g_input, &ev, 1);
    jce_actions_update(g_actions, g_input);
}

static void press_button(int button, int down)
{
    JceInputEvent ev = mk(JCE_INPUT_EVENT_DEVICE_BUTTON);
    ev.dbutton.instance = PAD_INSTANCE;
    ev.dbutton.code     = button;
    ev.dbutton.down     = (uint8_t)down;
    ev.dbutton.semantic = 1;          /* code is a JceGamepadButton */
    jce_input_submit(g_input, &ev, 1);
    jce_actions_update(g_actions, g_input);
}

static float action_value(const char *name)
{
    int id = jce_action_find(g_actions, name);
    TEST_ASSERT_TRUE_MESSAGE(id >= 0, name);
    return jce_action_value(g_actions, id);
}

static float action_pad_value(const char *name)
{
    int id = jce_action_find(g_actions, name);
    TEST_ASSERT_TRUE_MESSAGE(id >= 0, name);
    return jce_action_value_device(g_actions, id, JCE_DEVICE_GAMEPAD);
}

void setUp(void)
{
    memset(&g_fake, 0, sizeof(g_fake));
    memset(&g_backend, 0, sizeof(g_backend));
    g_backend.user         = &g_fake;
    g_backend.open_device  = fake_open;
    g_backend.close_device = fake_close;

    g_input = jce_input_create();
    jce_input_set_backend(g_input, &g_backend);

    g_actions = jce_actions_load_memory(k_keyboard_only_map,
                                        sizeof(k_keyboard_only_map) - 1);
}

void tearDown(void)
{
    if (g_actions) jce_actions_destroy(g_actions);
    if (g_input)   jce_input_destroy(g_input);
    g_actions = NULL;
    g_input   = NULL;
}

/* ── The break, pinned ────────────────────────────────────────────────── */

/* The map on disk really does carry no gamepad binding.  If this ever fails,
 * the premise of the merge is gone and the rest of this file is theatre. */
static void test_the_loaded_map_has_no_gamepad_binding(void)
{
    TEST_ASSERT_NOT_NULL(g_actions);
    TEST_ASSERT_EQUAL_INT(8, jce_actions_count(g_actions));

    int pad_binds = 0;
    for (int i = 0; i < jce_actions_count(g_actions); ++i) {
        for (int b = 0; b < jce_action_bind_count(g_actions, i); ++b) {
            JceBinding bind;
            TEST_ASSERT_TRUE(jce_action_bind_at(g_actions, i, b, &bind));
            if (bind.type == JCE_SRC_PAD_BUTTON ||
                bind.type == JCE_SRC_PAD_AXIS)
                pad_binds++;
        }
    }
    TEST_ASSERT_EQUAL_INT(0, pad_binds);
}

static void test_merge_adds_the_canonical_gamepad_bindings(void)
{
    int added = jce_actions_merge_gamepad_defaults(g_actions);
    /* move_forward, move_right, jump, sprint, look_x, look_y -- move_back and
     * move_left get none, deliberately (the axis carries the sign). */
    TEST_ASSERT_EQUAL_INT(6, added);
}

static void test_merge_is_idempotent(void)
{
    TEST_ASSERT_EQUAL_INT(6, jce_actions_merge_gamepad_defaults(g_actions));
    TEST_ASSERT_EQUAL_INT(0, jce_actions_merge_gamepad_defaults(g_actions));
    TEST_ASSERT_EQUAL_INT(0, jce_actions_merge_gamepad_defaults(g_actions));

    /* move_forward ends with exactly its key + one axis, never a second copy. */
    int id = jce_action_find(g_actions, "move_forward");
    TEST_ASSERT_EQUAL_INT(2, jce_action_bind_count(g_actions, id));
}

static void test_merge_keeps_its_hands_off_a_map_that_already_has_a_pad_bind(void)
{
    /* One deliberate pad binding anywhere means the author knows pads exist,
     * so their omissions are choices.  Nothing may be added -- not even to the
     * five other canonical actions that have none. */
    int jump = jce_action_find(g_actions, "jump");
    JceBinding b;
    jce_binding_init(&b, JCE_SRC_PAD_BUTTON, JCE_GAMEPAD_BUTTON_NORTH);
    TEST_ASSERT_TRUE(jce_action_bind(g_actions, jump, &b));  /* Y, not South */

    TEST_ASSERT_EQUAL_INT(0, jce_actions_merge_gamepad_defaults(g_actions));

    int fwd = jce_action_find(g_actions, "move_forward");
    TEST_ASSERT_EQUAL_INT(1, jce_action_bind_count(g_actions, fwd)); /* key only */
}

static void test_merge_honours_the_explicit_opt_out(void)
{
    static const char k_opted_out[] =
        "{\"gamepad_defaults\":false,\"actions\":["
        "{\"name\":\"move_forward\",\"binds\":[{\"type\":0,\"code\":26}]},"
        "{\"name\":\"jump\",\"binds\":[{\"type\":0,\"code\":44}]}"
        "]}";
    JceInputActions *a =
        jce_actions_load_memory(k_opted_out, sizeof(k_opted_out) - 1);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_INT(0, jce_actions_merge_gamepad_defaults(a));
    TEST_ASSERT_EQUAL_INT(1, jce_action_bind_count(a, jce_action_find(a, "jump")));
    jce_actions_destroy(a);
}

static void test_merge_never_resurrects_a_deleted_action(void)
{
    /* A map that kept only movement gets no "jump"/"look_x" invented for it. */
    static const char k_trimmed[] =
        "{\"actions\":["
        "{\"name\":\"move_forward\",\"binds\":[{\"type\":0,\"code\":26}]},"
        "{\"name\":\"move_right\",\"binds\":[{\"type\":0,\"code\":7}]}"
        "]}";
    JceInputActions *a =
        jce_actions_load_memory(k_trimmed, sizeof(k_trimmed) - 1);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_INT(2, jce_actions_merge_gamepad_defaults(a));
    TEST_ASSERT_EQUAL_INT(2, jce_actions_count(a));          /* still two */
    TEST_ASSERT_EQUAL_INT(-1, jce_action_find(a, "jump"));
    TEST_ASSERT_EQUAL_INT(-1, jce_action_find(a, "look_x"));
    jce_actions_destroy(a);
}

/* ── The chain, end to end ────────────────────────────────────────────── */

static void test_a_submitted_axis_event_moves_a_merged_action(void)
{
    jce_actions_merge_gamepad_defaults(g_actions);

    /* Nothing is open yet: an axis for an unopened instance must be dropped,
     * which is what makes the DEVICE_ADDED below load-bearing rather than
     * decorative. */
    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];
    move_axis(JCE_GAMEPAD_AXIS_LEFTY, -1.0f);
    TEST_ASSERT_EQUAL_INT(0, g_fake.opens);
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_ids(g_input, ids,
                                                  JCE_INPUT_MAX_DEVICES));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, action_value("move_forward"));

    plug_in_pad();
    TEST_ASSERT_EQUAL_INT(1, g_fake.opens);
    TEST_ASSERT_EQUAL_UINT64(PAD_INSTANCE, g_fake.last_open);
    TEST_ASSERT_EQUAL_INT(1, jce_input_device_ids(g_input, ids,
                                                  JCE_INPUT_MAX_DEVICES));

    /* Stick FORWARD.  SDL reports up as negative; the canonical bind carries
     * scale -1, so the action reads positive. */
    move_axis(JCE_GAMEPAD_AXIS_LEFTY, -1.0f);
    TEST_ASSERT_TRUE(action_value("move_forward") > 0.9f);
}

/* The defect this slice exists to kill: a stick pulled fully BACK used to make
 * the character walk FORWARD, because jce_action_down() is `value != 0` and
 * -1.0 is not 0.  The value must be NEGATIVE, and the boolean must be the
 * thing that is wrong. */
static void test_pulling_the_stick_back_is_negative_not_merely_down(void)
{
    jce_actions_merge_gamepad_defaults(g_actions);
    plug_in_pad();

    move_axis(JCE_GAMEPAD_AXIS_LEFTY, 1.0f);   /* pulled fully back */

    int fwd = jce_action_find(g_actions, "move_forward");
    TEST_ASSERT_TRUE_MESSAGE(jce_action_value(g_actions, fwd) < -0.9f,
                             "stick back must read NEGATIVE forward");
    /* And the trap, pinned in place so nobody 'fixes' the map by reading it: */
    TEST_ASSERT_TRUE_MESSAGE(jce_action_down(g_actions, fwd),
                             "jce_action_down is TRUE here -- that is the bug, "
                             "movement must never be read through it");

    /* move_back stays unbound on the pad: the sign above IS backward. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, action_pad_value("move_back"));
}

static void test_half_deflection_is_half_magnitude(void)
{
    jce_actions_merge_gamepad_defaults(g_actions);
    plug_in_pad();

    move_axis(JCE_GAMEPAD_AXIS_LEFTY, -1.0f);
    float full = action_value("move_forward");

    move_axis(JCE_GAMEPAD_AXIS_LEFTY, -0.5f);
    float half = action_value("move_forward");

    TEST_ASSERT_TRUE(full > 0.9f);
    TEST_ASSERT_TRUE_MESSAGE(half > 0.3f && half < 0.7f,
                             "a half-deflected stick must be analog, not 0 or 1");
    TEST_ASSERT_TRUE(half < full);

    /* Inside the deadzone it is silent -- otherwise a resting stick drifts. */
    move_axis(JCE_GAMEPAD_AXIS_LEFTY, -0.05f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, action_value("move_forward"));
}

/* ── the authored zero, through the hardware door ─────────────────────── */

/* Bind one pad axis to a fresh action and return its id.  `dz` goes straight
 * into deadzone_inner, so a caller can hand over the two values the old
 * evaluator could not tell apart: 0.0 ("the author said none") and -1.0 ("the
 * author did not say"). */
static int bind_pad_axis(const char *name, int axis, float dz)
{
    int id = jce_action_register(g_actions, name);
    TEST_ASSERT_TRUE(id >= 0);
    JceBinding b;
    jce_binding_init(&b, JCE_SRC_PAD_AXIS, axis);
    b.deadzone_inner = dz;
    b.deadzone_outer = 1.0f;
    b.side           = (uint8_t)JCE_AXIS_SIDE_FULL;
    TEST_ASSERT_TRUE(jce_action_bind(g_actions, id, &b));
    return id;
}

/* THE fix, measured through jce_input_submit rather than asserted about.
 *
 * The evaluator used to compute `deadzone > 0 ? deadzone : 0.15f`, so an
 * authored 0 was silently promoted to 0.15 and the first 15 % of an analog
 * trigger's travel could not be made to register by any file anyone could
 * write.  With NEGATIVE meaning "defer", 0 means 0. */
static void test_an_authored_zero_deadzone_registers_the_first_millimetre(void)
{
    int id = bind_pad_axis("throttle_raw", JCE_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.0f);
    plug_in_pad();

    move_axis(JCE_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.05f);
    float v = jce_action_value(g_actions, id);
    TEST_ASSERT_TRUE_MESSAGE(v > 0.001f,
        "deadzone_inner 0 means ZERO: 5 % travel must register.  If this is "
        "0.0 the evaluator is substituting a default over an authored value.");
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.05f, v);   /* no remap: dz == 0 */
}

/* And the other half of the same split: a binding that DEFERS (-1) still gets
 * the 0.15 the engine has always applied, so a resting stick stays silent. */
static void test_a_deferred_deadzone_still_swallows_a_resting_stick(void)
{
    int id = bind_pad_axis("throttle_default", JCE_GAMEPAD_AXIS_RIGHT_TRIGGER,
                           -1.0f);
    plug_in_pad();

    move_axis(JCE_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.05f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_action_value(g_actions, id));

    move_axis(JCE_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.60f);
    TEST_ASSERT_TRUE(jce_action_value(g_actions, id) > 0.4f);
}

static void test_south_button_reaches_jump_and_l3_reaches_sprint(void)
{
    jce_actions_merge_gamepad_defaults(g_actions);
    plug_in_pad();

    int jump = jce_action_find(g_actions, "jump");
    press_button(JCE_GAMEPAD_BUTTON_SOUTH, 1);
    TEST_ASSERT_TRUE(jce_action_down(g_actions, jump));
    TEST_ASSERT_TRUE(jce_action_pressed(g_actions, jump));  /* rising edge */

    press_button(JCE_GAMEPAD_BUTTON_SOUTH, 0);
    TEST_ASSERT_FALSE(jce_action_down(g_actions, jump));

    int sprint = jce_action_find(g_actions, "sprint");
    press_button(JCE_GAMEPAD_BUTTON_LEFT_STICK, 1);
    TEST_ASSERT_TRUE(jce_action_down(g_actions, sprint));
}

static void test_the_right_stick_reaches_look(void)
{
    jce_actions_merge_gamepad_defaults(g_actions);
    plug_in_pad();

    move_axis(JCE_GAMEPAD_AXIS_RIGHTX, 1.0f);
    TEST_ASSERT_TRUE(action_value("look_x") > 0.9f);

    move_axis(JCE_GAMEPAD_AXIS_RIGHTX, -1.0f);
    TEST_ASSERT_TRUE(action_value("look_x") < -0.9f);

    /* Stick UP is negative in SDL and the bind inverts it, so up looks up. */
    move_axis(JCE_GAMEPAD_AXIS_RIGHTY, -1.0f);
    TEST_ASSERT_TRUE(action_value("look_y") > 0.9f);
}

/* ── The per-device split the editor depends on ───────────────────────── */

/* The Game View reads the keyboard through ImGui (so Play only moves while the
 * viewport is captured) and the pad through the engine map.  If the pad query
 * returned the WHOLE action value, a held W would drive the character from an
 * unfocused viewport.  So the split has to be real. */
static void test_the_pad_value_excludes_the_keyboard_half(void)
{
    jce_actions_merge_gamepad_defaults(g_actions);
    plug_in_pad();

    /* Hold W -- the keyboard half of move_forward. */
    JceInputEvent ev = mk(JCE_INPUT_EVENT_KEY);
    ev.key.scancode = JCE_KEY_W;
    ev.key.down     = 1;
    jce_input_submit(g_input, &ev, 1);
    jce_actions_update(g_actions, g_input);

    TEST_ASSERT_TRUE(action_value("move_forward") > 0.9f);     /* combined */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, action_pad_value("move_forward"));
    TEST_ASSERT_TRUE(jce_action_value_device(
        g_actions, jce_action_find(g_actions, "move_forward"),
        JCE_DEVICE_KBM) > 0.9f);

    /* Now add the stick, pulled back: the pad half is negative while the
     * keyboard half stays positive.  Reading the pad alone is what lets the
     * editor add the two without counting the keyboard twice. */
    move_axis(JCE_GAMEPAD_AXIS_LEFTY, 1.0f);
    TEST_ASSERT_TRUE(action_pad_value("move_forward") < -0.9f);
    TEST_ASSERT_TRUE(jce_action_value_device(
        g_actions, jce_action_find(g_actions, "move_forward"),
        JCE_DEVICE_KBM) > 0.9f);
}

static void test_unplugging_the_pad_closes_it_and_silences_the_action(void)
{
    jce_actions_merge_gamepad_defaults(g_actions);
    plug_in_pad();
    move_axis(JCE_GAMEPAD_AXIS_LEFTY, -1.0f);
    TEST_ASSERT_TRUE(action_value("move_forward") > 0.9f);

    JceInputEvent ev = mk(JCE_INPUT_EVENT_DEVICE_REMOVED);
    ev.device.instance = PAD_INSTANCE;
    jce_input_submit(g_input, &ev, 1);
    jce_actions_update(g_actions, g_input);

    TEST_ASSERT_EQUAL_INT(1, g_fake.closes);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, action_value("move_forward"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_loaded_map_has_no_gamepad_binding);
    RUN_TEST(test_merge_adds_the_canonical_gamepad_bindings);
    RUN_TEST(test_merge_is_idempotent);
    RUN_TEST(test_merge_keeps_its_hands_off_a_map_that_already_has_a_pad_bind);
    RUN_TEST(test_merge_honours_the_explicit_opt_out);
    RUN_TEST(test_merge_never_resurrects_a_deleted_action);
    RUN_TEST(test_a_submitted_axis_event_moves_a_merged_action);
    RUN_TEST(test_pulling_the_stick_back_is_negative_not_merely_down);
    RUN_TEST(test_half_deflection_is_half_magnitude);
    RUN_TEST(test_an_authored_zero_deadzone_registers_the_first_millimetre);
    RUN_TEST(test_a_deferred_deadzone_still_swallows_a_resting_stick);
    RUN_TEST(test_south_button_reaches_jump_and_l3_reaches_sprint);
    RUN_TEST(test_the_right_stick_reaches_look);
    RUN_TEST(test_the_pad_value_excludes_the_keyboard_half);
    RUN_TEST(test_unplugging_the_pad_closes_it_and_silences_the_action);
    return UNITY_END();
}
