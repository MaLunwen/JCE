/* test_jce_binding_init.c
 *
 * jce_binding_init is the ONLY place a binding default is invented.
 *
 * The evaluator used to write `b->deadzone > 0 ? b->deadzone : 0.15f`.  That
 * single expression conflated "the author did not say" with "the author said
 * zero", which is why `deadzone: 0` -- the one value an analog trigger wants,
 * so travel registers from the start -- was literally unauthorable.  Splitting
 * the two apart means the DEFAULT lives in an initialiser and the EVALUATOR
 * substitutes nothing, ever.  These tests pin both halves: the per-source
 * defaults this function invents, and the fact that a zero written after it
 * survives to the other side of jce_action_bind.
 *
 * They also pin the receiver rule.  A JceBinding is size-prefixed; a caller
 * that memsets the struct and forgets `size` is handing over a record the
 * callee cannot interpret, and the callee must say so instead of copying
 * whatever fields happen to line up.
 */

#include <jce/os/platform/jce_input_actions.h>
#include <jce/os/platform/jce_input_device.h>
#include <jce/os/platform/jce_gamepad.h>
#include <jce/os/platform/jce_keys.h>

#include "unity.h"

#include <string.h>

static JceInputActions *g_actions = NULL;

void setUp(void)    { g_actions = jce_actions_create(); }
void tearDown(void) { jce_actions_destroy(g_actions); g_actions = NULL; }

/* The prefix constant is the whole record, not a guessed number.  The design
 * shipped 56, which is offsetof(JceBinding, comp) + 12 -- the middle of
 * comp[1].  Every v2 field is required by a v2 receiver, so the prefix IS the
 * record.  jce_input_actions.c carries the matching JCE_SASSERT, which is what
 * makes a future append break the build rather than this assertion. */
static void test_size_prefix_is_sizeof(void)
{
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(JceBinding), JCE_BINDING_SIZE_V2);
    TEST_ASSERT_EQUAL_UINT32(76u, JCE_BINDING_SIZE_V2);
    TEST_ASSERT_EQUAL_UINT32(8u, (uint32_t)sizeof(JceInputSource));
}

/* A key binding: no dead region at all, scale 1, curve 1, unpaired. */
static void test_key_defaults(void)
{
    JceBinding b;
    memset(&b, 0xAB, sizeof b);          /* poison: init must clear it */
    jce_binding_init(&b, JCE_SRC_KEY, JCE_KEY_W);

    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(JceBinding), b.size);
    TEST_ASSERT_EQUAL_INT(JCE_SRC_KEY, b.type);
    TEST_ASSERT_EQUAL_INT(JCE_KEY_W,   b.code);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, b.scale);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, b.curve);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b.deadzone_inner);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, b.deadzone_outer);
    TEST_ASSERT_EQUAL_INT(JCE_AXIS_SIDE_FULL, b.side);
    TEST_ASSERT_EQUAL_INT(JCE_CHAN_SCALAR,    b.channel);
    TEST_ASSERT_EQUAL_UINT8(JCE_BIND_PAIR_NONE, b.pair_axis);
    TEST_ASSERT_EQUAL_UINT8(JCE_HAT_CENTERED,   b.hat_dir);
    TEST_ASSERT_EQUAL_UINT8(0u, b.device_slot);
    TEST_ASSERT_EQUAL_UINT32(0u, b.flags);
    TEST_ASSERT_EQUAL_INT(JCE_DEVICE_NONE, b.device_group);
    for (int i = 0; i < 4; ++i)
        TEST_ASSERT_EQUAL_INT(JCE_SRC_NONE, b.comp[i].type);
}

/* JCE_INPUT_PLAYER_NONE is (-1), not 0.  A memset-then-fill would put every
 * binding on player 0 -- which passes every test in this suite today and
 * surfaces later as "both pads drive the same character".  Pinned separately
 * from the rest of the defaults so the failure names itself. */
static void test_player_default_is_none_not_zero(void)
{
    JceBinding b;
    memset(&b, 0x00, sizeof b);          /* the memset-and-fill caller */
    TEST_ASSERT_EQUAL_INT(0, b.player);  /* what a bare memset leaves */

    jce_binding_init(&b, JCE_SRC_KEY, JCE_KEY_W);
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_PLAYER_NONE, b.player);
    TEST_ASSERT_NOT_EQUAL_INT(0, b.player);
}

/* A trigger is UNIPOLAR by default and defers its dead region to the device
 * profile (-1).  A stick defers too, and pairs with its companion axis so the
 * shaping is radial rather than square. */
static void test_pad_axis_defaults_split_trigger_from_stick(void)
{
    JceBinding t;
    jce_binding_init(&t, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_RIGHT_TRIGGER);
    TEST_ASSERT_EQUAL_INT(JCE_AXIS_SIDE_UNIPOLAR, t.side);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, t.deadzone_inner);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, t.deadzone_outer);
    TEST_ASSERT_EQUAL_UINT8(JCE_BIND_PAIR_NONE, t.pair_axis);

    JceBinding s;
    jce_binding_init(&s, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_LEFTY);
    TEST_ASSERT_EQUAL_INT(JCE_AXIS_SIDE_FULL, s.side);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, s.deadzone_inner);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)JCE_GAMEPAD_AXIS_LEFTX, s.pair_axis);
}

/* A raw joystick axis does NOT pair: axis 0 of a wheel is steering and axis 1
 * is usually a pedal, and pairing them radially makes the pedal attenuate the
 * steering.  Authoring pair_axis is opt-in for raw devices. */
static void test_joy_axis_does_not_pair(void)
{
    JceBinding b;
    jce_binding_init(&b, JCE_SRC_JOY_AXIS, 0);
    TEST_ASSERT_EQUAL_UINT8(JCE_BIND_PAIR_NONE, b.pair_axis);
    TEST_ASSERT_EQUAL_INT(JCE_AXIS_SIDE_FULL, b.side);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, b.deadzone_inner);
}

/* A mouse axis is px/frame and already relative, so it is born RAW|DELTA. */
static void test_mouse_axis_is_raw_delta(void)
{
    JceBinding b;
    jce_binding_init(&b, JCE_SRC_MOUSE_AXIS, 0);
    TEST_ASSERT_EQUAL_UINT32(JCE_BINDF_DELTA | JCE_BINDF_RAW, b.flags);
}

/* THE point of the whole split: a zero written after init is a value, and it
 * survives jce_action_bind -> jce_action_bind_at unchanged. */
static void test_authored_zero_deadzone_survives_binding(void)
{
    int id = jce_action_register(g_actions, "accelerate");
    TEST_ASSERT_TRUE(id >= 0);

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_RIGHT_TRIGGER);
    b.deadzone_inner = 0.0f;             /* explicitly: no dead region */
    b.deadzone_outer = 0.95f;
    TEST_ASSERT_TRUE(jce_action_bind(g_actions, id, &b));

    JceBinding out;
    TEST_ASSERT_TRUE(jce_action_bind_at(g_actions, id, 0, &out));
    TEST_ASSERT_EQUAL_FLOAT(0.0f,  out.deadzone_inner);
    TEST_ASSERT_EQUAL_FLOAT(0.95f, out.deadzone_outer);
    TEST_ASSERT_EQUAL_INT(JCE_AXIS_SIDE_UNIPOLAR, out.side);
}

/* Typed composite sub-bindings.  The bare ints were hardwired to
 * jce_input_key_down, so a D-pad code of 14 silently bound the letter K. */
static void test_composite_sub_sources_are_typed(void)
{
    int id = jce_action_register(g_actions, "menu_nav");
    TEST_ASSERT_TRUE(id >= 0);

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_COMPOSITE, JCE_COMPOSITE_VECTOR_2D);
    b.comp[JCE_COMP_POS].type = (int16_t)JCE_SRC_PAD_BUTTON;
    b.comp[JCE_COMP_POS].code = JCE_GAMEPAD_BUTTON_DPAD_RIGHT;
    b.comp[JCE_COMP_NEG].type = (int16_t)JCE_SRC_PAD_BUTTON;
    b.comp[JCE_COMP_NEG].code = JCE_GAMEPAD_BUTTON_DPAD_LEFT;
    TEST_ASSERT_TRUE(jce_action_bind(g_actions, id, &b));

    JceBinding out;
    TEST_ASSERT_TRUE(jce_action_bind_at(g_actions, id, 0, &out));
    TEST_ASSERT_EQUAL_INT(JCE_SRC_PAD_BUTTON, out.comp[JCE_COMP_POS].type);
    TEST_ASSERT_EQUAL_INT(JCE_GAMEPAD_BUTTON_DPAD_RIGHT,
                          out.comp[JCE_COMP_POS].code);
    TEST_ASSERT_EQUAL_INT(JCE_SRC_NONE, out.comp[JCE_COMP_UP].type);
}

/* Receiver rule: a short record is refused, never partially interpreted. */
static void test_short_record_is_refused(void)
{
    int id = jce_action_register(g_actions, "fire");
    TEST_ASSERT_TRUE(id >= 0);

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_KEY, JCE_KEY_SPACE);
    b.size = JCE_BINDING_SIZE_V2 - 1u;
    TEST_ASSERT_FALSE(jce_action_bind(g_actions, id, &b));
    TEST_ASSERT_EQUAL_INT(0, jce_action_bind_count(g_actions, id));

    b.size = 0u;                          /* the memset-and-forget caller */
    TEST_ASSERT_FALSE(jce_action_bind(g_actions, id, &b));
    TEST_ASSERT_EQUAL_INT(0, jce_action_bind_count(g_actions, id));

    /* And the record a v2 caller actually hands over is accepted. */
    b.size = JCE_BINDING_SIZE_V2;
    TEST_ASSERT_TRUE(jce_action_bind(g_actions, id, &b));
    TEST_ASSERT_EQUAL_INT(1, jce_action_bind_count(g_actions, id));
}

/* A record LARGER than sizeof (a newer caller) is truncated, not misread, and
 * the stored copy always reports the size this build actually holds.
 *
 * The first version of this test could not report the failure it was named
 * for.  It declared `b.size = sizeof + 64` on a plain 76-byte JceBinding and
 * bound it at slot 0, so deleting
 *     if (n > sizeof(JceBinding)) n = sizeof(JceBinding);
 * merely made memcpy read 64 bytes of adjacent stack and write them into
 * binds[1], where nothing ever looked; `dst->size` is stamped afterwards, so
 * both surviving assertions still held and the test stayed green.  Only a
 * sanitizer noticed.
 *
 * This version hands over a REAL 140-byte record with a tail this build must
 * never copy, and binds it into the LAST slot -- ActionEntry.bind_count sits
 * immediately past binds[JCE_ACTION_MAX_BINDS], so an unclamped copy lands on
 * it (and then on the following action) and both canaries below go red. */
static void test_oversized_record_is_truncated(void)
{
    int id = jce_action_register(g_actions, "future");
    TEST_ASSERT_TRUE(id >= 0);
    /* Registered immediately after `future`, so it occupies the next
     * ActionEntry -- the second thing an overrun reaches. */
    int after = jce_action_register(g_actions, "future_canary");
    TEST_ASSERT_TRUE(after >= 0);

    for (int i = 0; i < JCE_ACTION_MAX_BINDS - 1; ++i) {
        JceBinding filler;
        jce_binding_init(&filler, JCE_SRC_KEY, JCE_KEY_A);
        TEST_ASSERT_TRUE(jce_action_bind(g_actions, id, &filler));
    }
    TEST_ASSERT_EQUAL_INT(JCE_ACTION_MAX_BINDS - 1,
                          jce_action_bind_count(g_actions, id));

    /* The v3 caller: a record that really is as large as it says it is. */
    struct { JceBinding b; uint32_t tail[16]; } big;
    memset(&big, 0, sizeof big);
    jce_binding_init(&big.b, JCE_SRC_KEY, JCE_KEY_G);
    for (int i = 0; i < 16; ++i) big.tail[i] = 0xDEADBEEFu;
    big.b.size = (uint32_t)sizeof big;
    TEST_ASSERT_EQUAL_UINT32(JCE_BINDING_SIZE_V2 + 64u, big.b.size);
    TEST_ASSERT_TRUE(jce_action_bind(g_actions, id, &big.b));

    /* THE clamp, named.  Without it the tail overwrites bind_count and the
     * increment that follows reports 0xDEADBEF0 instead of the slot count. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_ACTION_MAX_BINDS,
        jce_action_bind_count(g_actions, id),
        "the declared-but-nonexistent tail of an oversized record reached the "
        "action table: the min(size, sizeof) clamp in jce_action_bind is gone");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("future_canary",
        jce_action_name(g_actions, after),
        "an oversized record overran its ActionEntry into the next action");
    TEST_ASSERT_EQUAL_INT(0, jce_action_bind_count(g_actions, after));

    JceBinding out;
    TEST_ASSERT_TRUE(jce_action_bind_at(g_actions, id,
                                        JCE_ACTION_MAX_BINDS - 1, &out));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(JceBinding), out.size);
    TEST_ASSERT_EQUAL_INT(JCE_KEY_G, out.code);
}

/* The old spellings still compile and still mean the same source. */
static void test_legacy_spellings_are_aliases(void)
{
    TEST_ASSERT_EQUAL_INT(JCE_SRC_KEY,          JCE_BIND_KEY);
    TEST_ASSERT_EQUAL_INT(JCE_SRC_MOUSE_BUTTON, JCE_BIND_MOUSE_BTN);
    TEST_ASSERT_EQUAL_INT(JCE_SRC_PAD_BUTTON,   JCE_BIND_GAMEPAD_BTN);
    TEST_ASSERT_EQUAL_INT(JCE_SRC_PAD_AXIS,     JCE_BIND_GAMEPAD_AXIS);
    TEST_ASSERT_EQUAL_INT(JCE_SRC_COMPOSITE,    JCE_BIND_COMPOSITE);
}

/* The v2 enum values are NOT the schema-1 disk numbers, and NOT the editor's
 * combo indices.  This is the hazard behind two separate translation layers --
 * bind_type_from_v1/_to_v1 in jce_input_actions.c and k_panel_bind_types in
 * the Input Manager panel.  Delete either and an int that used to be a source
 * becomes a DIFFERENT source silently, so the inequality is pinned here rather
 * than left as a comment. */
static void test_v2_enum_is_not_the_v1_disk_numbering(void)
{
    TEST_ASSERT_EQUAL_INT(0, JCE_SRC_NONE);   /* slot 0 is now "absent" */
    TEST_ASSERT_NOT_EQUAL_INT(0, JCE_SRC_KEY);
    TEST_ASSERT_NOT_EQUAL_INT(1, JCE_SRC_MOUSE_BUTTON);
    TEST_ASSERT_NOT_EQUAL_INT(2, JCE_SRC_PAD_BUTTON);
    TEST_ASSERT_NOT_EQUAL_INT(3, JCE_SRC_PAD_AXIS);
    TEST_ASSERT_NOT_EQUAL_INT(4, JCE_SRC_COMPOSITE);
}

/* Eight binds fit; the ninth is refused.  A key + composite + stick + trigger
 * + a raw-device axis is legitimately more than the old four. */
static void test_eight_binds_fit(void)
{
    TEST_ASSERT_EQUAL_INT(8, JCE_ACTION_MAX_BINDS);

    int id = jce_action_register(g_actions, "many");
    TEST_ASSERT_TRUE(id >= 0);
    for (int i = 0; i < 8; ++i) {
        JceBinding b;
        jce_binding_init(&b, JCE_SRC_KEY, JCE_KEY_A + i);
        TEST_ASSERT_TRUE(jce_action_bind(g_actions, id, &b));
    }
    TEST_ASSERT_EQUAL_INT(8, jce_action_bind_count(g_actions, id));

    JceBinding overflow;
    jce_binding_init(&overflow, JCE_SRC_KEY, JCE_KEY_Z);
    TEST_ASSERT_FALSE(jce_action_bind(g_actions, id, &overflow));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_size_prefix_is_sizeof);
    RUN_TEST(test_key_defaults);
    RUN_TEST(test_player_default_is_none_not_zero);
    RUN_TEST(test_pad_axis_defaults_split_trigger_from_stick);
    RUN_TEST(test_joy_axis_does_not_pair);
    RUN_TEST(test_mouse_axis_is_raw_delta);
    RUN_TEST(test_authored_zero_deadzone_survives_binding);
    RUN_TEST(test_composite_sub_sources_are_typed);
    RUN_TEST(test_short_record_is_refused);
    RUN_TEST(test_oversized_record_is_truncated);
    RUN_TEST(test_legacy_spellings_are_aliases);
    RUN_TEST(test_v2_enum_is_not_the_v1_disk_numbering);
    RUN_TEST(test_eight_binds_fit);
    return UNITY_END();
}
