/* test_jce_input_composite.c
 *
 * Unit tests for composite input bindings (Feature 6.6):
 *   - 2D vector composite (WASD): up/down/left/right keys -> raw (x,y)
 *       * W+D held -> (+1, +1)   (RAW sum, NOT normalized)
 *       * no keys  -> (0, 0)
 *       * A held   -> (-1, 0)
 *   - 1D axis composite (Q/E): positive/negative keys -> signed scalar
 *       * E (positive) -> +1 ; Q (negative) -> -1 ; both -> 0
 *   - scalar value() of a 2D composite == vector MAGNITUDE (0..1), so a
 *     left/down-only press reports +1.0 (active), never a negative scalar;
 *     1D axis keeps its sign. Classification is by the binding's DECLARED
 *     kind (b->code), not by whether vy is 0 this frame.
 *
 * Exercises the REAL jce_actions_update / evaluate path: keys are injected
 * through a JceInput frame (jce_input_capture/apply), then jce_actions_update
 * resolves the composite and the typed jce_action_value2 query is checked.
 */

#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_actions.h>
#include <jce/os/platform/jce_keys.h>
#include <jce/os/platform/jce_gamepad.h>

#include "unity.h"

#include <math.h>
#include <string.h>

static JceInput        *g_input   = NULL;
static JceInputActions *g_actions = NULL;

void setUp(void)
{
    g_input   = jce_input_create();
    g_actions = jce_actions_create();
}

void tearDown(void)
{
    jce_actions_destroy(g_actions);
    jce_input_destroy(g_input);
    g_actions = NULL;
    g_input   = NULL;
}

/* Inject a set of held scancodes into g_input via the capture/apply path,
 * then run the action update over the real evaluate code. `keys` is a
 * NULL-able array of `count` JceKey scancodes that should be held down. */
static void hold_keys_and_update(const int *keys, int count)
{
    JceInputFrame frame;
    jce_input_capture(g_input, &frame);     /* version-stamped */
    /* capture() snapshots currently-HELD keys (it is the record/replay path,
     * not a clean frame builder); clear them so this helper injects exactly
     * `keys` each call rather than accumulating the previous call's keys. */
    memset(frame.keys_bits, 0, sizeof(frame.keys_bits));

    for (int i = 0; i < count; ++i) {
        int sc = keys[i];
        frame.keys_bits[sc >> 6] |= (uint64_t)1 << (sc & 63);
    }

    TEST_ASSERT_TRUE(jce_input_apply(g_input, &frame));
    jce_actions_update(g_actions, g_input);
}

/* Register a WASD 2D-vector composite action; returns its id. */
static int register_wasd(void)
{
    int id = jce_action_register(g_actions, "move");
    TEST_ASSERT_TRUE(id >= 0);

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_COMPOSITE, JCE_COMPOSITE_VECTOR_2D);
    b.comp[JCE_COMP_POS].type  = (int16_t)JCE_SRC_KEY;
    b.comp[JCE_COMP_POS].code  = JCE_KEY_D;   /* +x right */
    b.comp[JCE_COMP_NEG].type  = (int16_t)JCE_SRC_KEY;
    b.comp[JCE_COMP_NEG].code  = JCE_KEY_A;   /* -x left  */
    b.comp[JCE_COMP_UP].type   = (int16_t)JCE_SRC_KEY;
    b.comp[JCE_COMP_UP].code   = JCE_KEY_W;   /* +y up    */
    b.comp[JCE_COMP_DOWN].type = (int16_t)JCE_SRC_KEY;
    b.comp[JCE_COMP_DOWN].code = JCE_KEY_S;   /* -y down  */
    TEST_ASSERT_TRUE(jce_action_bind(g_actions, id, &b));
    return id;
}

static void test_2d_no_keys_is_zero(void)
{
    int id = register_wasd();
    hold_keys_and_update(NULL, 0);

    JceActionVec2 v;
    jce_action_value2(g_actions, id, &v);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.y);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_action_value(g_actions, id));
    TEST_ASSERT_FALSE(jce_action_down(g_actions, id));
}

static void test_2d_w_plus_d_raw_sum(void)
{
    int id = register_wasd();
    int keys[2] = { JCE_KEY_W, JCE_KEY_D };
    hold_keys_and_update(keys, 2);

    JceActionVec2 v;
    jce_action_value2(g_actions, id, &v);
    /* RAW (un-normalized): W (up=+y) + D (right=+x) -> (+1, +1). */
    TEST_ASSERT_EQUAL_FLOAT(1.0f, v.x);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, v.y);

    /* Scalar value == magnitude clamped to 0..1 -> active. */
    TEST_ASSERT_TRUE(jce_action_down(g_actions, id));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_action_value(g_actions, id)); /* clamp */
}

static void test_2d_single_axis(void)
{
    int id = register_wasd();

    int a_key[1] = { JCE_KEY_A };
    hold_keys_and_update(a_key, 1);
    JceActionVec2 v;
    jce_action_value2(g_actions, id, &v);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, v.x);   /* left = -x */
    TEST_ASSERT_EQUAL_FLOAT( 0.0f, v.y);
    /* The scalar value() of a 2D-vector composite is the 0..1 MAGNITUDE, so a
     * LEFT-only press must report +1.0 (the action is active), NOT the signed
     * vx (-1.0). Classification is by the binding's DECLARED 2D kind, not by
     * vy happening to be 0 this frame. */
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_action_value(g_actions, id));
    TEST_ASSERT_TRUE(jce_action_down(g_actions, id));

    int s_key[1] = { JCE_KEY_S };
    hold_keys_and_update(s_key, 1);
    jce_action_value2(g_actions, id, &v);
    TEST_ASSERT_EQUAL_FLOAT( 0.0f, v.x);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, v.y);   /* down = -y */
    /* Down-only is also magnitude 1.0, never -1.0. */
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_action_value(g_actions, id));
}

static void test_2d_opposite_keys_cancel(void)
{
    int id = register_wasd();
    int keys[2] = { JCE_KEY_A, JCE_KEY_D };  /* left + right cancel on X */
    hold_keys_and_update(keys, 2);

    JceActionVec2 v;
    jce_action_value2(g_actions, id, &v);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.y);
}

static void test_1d_axis(void)
{
    int id = jce_action_register(g_actions, "turn");
    TEST_ASSERT_TRUE(id >= 0);

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_COMPOSITE, JCE_COMPOSITE_AXIS_1D);
    b.comp[JCE_COMP_POS].type = (int16_t)JCE_SRC_KEY;
    b.comp[JCE_COMP_POS].code = JCE_KEY_E;   /* positive */
    b.comp[JCE_COMP_NEG].type = (int16_t)JCE_SRC_KEY;
    b.comp[JCE_COMP_NEG].code = JCE_KEY_Q;   /* negative */
    TEST_ASSERT_TRUE(jce_action_bind(g_actions, id, &b));

    /* positive */
    int e_key[1] = { JCE_KEY_E };
    hold_keys_and_update(e_key, 1);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_action_value(g_actions, id));
    JceActionVec2 v;
    jce_action_value2(g_actions, id, &v);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, v.x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.y);

    /* negative keeps its sign in the scalar value */
    int q_key[1] = { JCE_KEY_Q };
    hold_keys_and_update(q_key, 1);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, jce_action_value(g_actions, id));
    jce_action_value2(g_actions, id, &v);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, v.x);

    /* both -> 0 */
    int both[2] = { JCE_KEY_Q, JCE_KEY_E };
    hold_keys_and_update(both, 2);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_action_value(g_actions, id));
}

/* A composite authored with scale == 0 must be HONORED (silenced), not
 * coerced back to 1.0: evaluate_composite uses b->scale directly. */
static void test_2d_zero_scale_is_silenced(void)
{
    int id = jce_action_register(g_actions, "move0");
    TEST_ASSERT_TRUE(id >= 0);

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_COMPOSITE, JCE_COMPOSITE_VECTOR_2D);
    b.scale = 0.0f;            /* explicitly silenced */
    b.comp[JCE_COMP_POS].type  = (int16_t)JCE_SRC_KEY;
    b.comp[JCE_COMP_POS].code  = JCE_KEY_D;
    b.comp[JCE_COMP_NEG].type  = (int16_t)JCE_SRC_KEY;
    b.comp[JCE_COMP_NEG].code  = JCE_KEY_A;
    b.comp[JCE_COMP_UP].type   = (int16_t)JCE_SRC_KEY;
    b.comp[JCE_COMP_UP].code   = JCE_KEY_W;
    b.comp[JCE_COMP_DOWN].type = (int16_t)JCE_SRC_KEY;
    b.comp[JCE_COMP_DOWN].code = JCE_KEY_S;
    TEST_ASSERT_TRUE(jce_action_bind(g_actions, id, &b));

    int keys[2] = { JCE_KEY_W, JCE_KEY_D };
    hold_keys_and_update(keys, 2);

    JceActionVec2 v;
    jce_action_value2(g_actions, id, &v);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.y);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_action_value(g_actions, id));
    TEST_ASSERT_FALSE(jce_action_down(g_actions, id));
}

/* THE discrimination JceInputSource was introduced for, and until this test
 * nothing in the suite exercised it: every other comp[] sub-source here and in
 * test_jce_input_serialize.c is JCE_SRC_KEY, so reverting evaluate_composite()
 * to the old `comp[i].code != 0` presence test left the whole suite green.
 *
 * The codes collide on purpose -- that collision IS the shipped bug:
 * JCE_GAMEPAD_BUTTON_DPAD_RIGHT is 14 and so is JCE_KEY_K; DPAD_LEFT is 13 and
 * so is JCE_KEY_J.  Holding the KEY must move nothing, because the sub-source
 * is typed as a pad button. */
static void test_pad_sub_source_does_not_resolve_as_key(void)
{
    /* If either enum is renumbered this test silently stops testing what it is
     * named for, so the collision is asserted rather than assumed. */
    TEST_ASSERT_EQUAL_INT(JCE_KEY_K, JCE_GAMEPAD_BUTTON_DPAD_RIGHT);
    TEST_ASSERT_EQUAL_INT(JCE_KEY_J, JCE_GAMEPAD_BUTTON_DPAD_LEFT);

    int id = jce_action_register(g_actions, "menu_nav");
    TEST_ASSERT_TRUE(id >= 0);

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_COMPOSITE, JCE_COMPOSITE_VECTOR_2D);
    b.comp[JCE_COMP_POS].type  = (int16_t)JCE_SRC_PAD_BUTTON;
    b.comp[JCE_COMP_POS].code  = JCE_GAMEPAD_BUTTON_DPAD_RIGHT;   /* == K */
    b.comp[JCE_COMP_NEG].type  = (int16_t)JCE_SRC_PAD_BUTTON;
    b.comp[JCE_COMP_NEG].code  = JCE_GAMEPAD_BUTTON_DPAD_LEFT;    /* == J */
    /* +y stays a real key so the composite is still LIVE.  Without it, a
     * change that stopped evaluating composites at all would pass this test
     * for the wrong reason -- the ninth time that shape has appeared here. */
    b.comp[JCE_COMP_UP].type   = (int16_t)JCE_SRC_KEY;
    b.comp[JCE_COMP_UP].code   = JCE_KEY_W;
    TEST_ASSERT_TRUE(jce_action_bind(g_actions, id, &b));

    /* Hold ONE of the two colliding keys at a time.  Holding both would let a
     * code-only evaluator score +1 and -1 and cancel to 0, which is the
     * correct answer for the wrong reason. */
    int k_only[2] = { JCE_KEY_K, JCE_KEY_W };
    hold_keys_and_update(k_only, 2);

    JceActionVec2 v;
    jce_action_value2(g_actions, id, &v);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, v.x,
        "key K (14) drove a JCE_SRC_PAD_BUTTON sub-source: evaluate_composite "
        "is testing `code != 0` again, not comp[i].type");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, v.y,
        "the JCE_SRC_KEY sub-source stopped resolving too -- this test is no "
        "longer proving discrimination, only silence");

    int j_only[2] = { JCE_KEY_J, JCE_KEY_W };
    hold_keys_and_update(j_only, 2);
    jce_action_value2(g_actions, id, &v);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, v.x,
        "key J (13) drove a JCE_SRC_PAD_BUTTON sub-source");
    TEST_ASSERT_EQUAL_FLOAT(1.0f, v.y);
}

/* A JCE_SRC_NONE slot that still carries a stale code must stay absent.  The
 * code-only presence test resurrected it as a live key. */
static void test_none_sub_source_with_stale_code_stays_absent(void)
{
    int id = jce_action_register(g_actions, "stale");
    TEST_ASSERT_TRUE(id >= 0);

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_COMPOSITE, JCE_COMPOSITE_AXIS_1D);
    b.comp[JCE_COMP_POS].type = (int16_t)JCE_SRC_KEY;
    b.comp[JCE_COMP_POS].code = JCE_KEY_E;
    b.comp[JCE_COMP_NEG].type = (int16_t)JCE_SRC_NONE;   /* absent ... */
    b.comp[JCE_COMP_NEG].code = JCE_KEY_Q;               /* ... but not blank */
    TEST_ASSERT_TRUE(jce_action_bind(g_actions, id, &b));

    int q_only[1] = { JCE_KEY_Q };
    hold_keys_and_update(q_only, 1);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, jce_action_value(g_actions, id),
        "a JCE_SRC_NONE sub-source resolved because its code was non-zero");

    int e_only[1] = { JCE_KEY_E };
    hold_keys_and_update(e_only, 1);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_action_value(g_actions, id));
}

static void test_value2_null_safe_and_oob(void)
{
    /* NULL out is a no-op (must not crash). */
    jce_action_value2(g_actions, 0, NULL);

    /* Out-of-range id resolves to (0,0). */
    JceActionVec2 v;
    v.x = 7.0f; v.y = 7.0f;
    jce_action_value2(g_actions, 999, &v);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.y);
}

static void test_short_key_tap_survives_one_game_frame(void)
{
    JceInputEvent events[2] = {0};
    for (int i = 0; i < 2; ++i) {
        events[i].size = (uint32_t)sizeof events[i];
        events[i].kind = JCE_INPUT_EVENT_KEY;
        events[i].key.scancode = JCE_KEY_RETURN;
    }
    events[0].key.down = 1u;
    jce_input_submit(g_input, events, 2);
    TEST_ASSERT_TRUE(jce_input_key_down(g_input, JCE_KEY_RETURN));
    TEST_ASSERT_TRUE(jce_input_key_pressed(g_input, JCE_KEY_RETURN));

    JceInputFrame captured;
    jce_input_capture(g_input, &captured);
    JceInput *replay = jce_input_create();
    TEST_ASSERT_TRUE(jce_input_apply(replay, &captured));
    TEST_ASSERT_TRUE(jce_input_key_down(replay, JCE_KEY_RETURN));
    jce_input_destroy(replay);

    jce_input_update(g_input);
    TEST_ASSERT_FALSE(jce_input_key_down(g_input, JCE_KEY_RETURN));
    TEST_ASSERT_FALSE(jce_input_key_pressed(g_input, JCE_KEY_RETURN));
}

static void test_ordinary_held_key_releases_without_extra_frame(void)
{
    JceInputEvent event = {0};
    event.size = (uint32_t)sizeof event;
    event.kind = JCE_INPUT_EVENT_KEY;
    event.key.scancode = JCE_KEY_RETURN;
    event.key.down = 1u;
    jce_input_submit(g_input, &event, 1);
    jce_input_update(g_input);
    event.key.down = 0u;
    jce_input_submit(g_input, &event, 1);
    TEST_ASSERT_FALSE(jce_input_key_down(g_input, JCE_KEY_RETURN));
    TEST_ASSERT_TRUE(jce_input_key_released(g_input, JCE_KEY_RETURN));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_short_key_tap_survives_one_game_frame);
    RUN_TEST(test_ordinary_held_key_releases_without_extra_frame);
    RUN_TEST(test_2d_no_keys_is_zero);
    RUN_TEST(test_2d_w_plus_d_raw_sum);
    RUN_TEST(test_2d_single_axis);
    RUN_TEST(test_2d_opposite_keys_cancel);
    RUN_TEST(test_2d_zero_scale_is_silenced);
    RUN_TEST(test_1d_axis);
    RUN_TEST(test_pad_sub_source_does_not_resolve_as_key);
    RUN_TEST(test_none_sub_source_with_stale_code_stays_absent);
    RUN_TEST(test_value2_null_safe_and_oob);
    return UNITY_END();
}
