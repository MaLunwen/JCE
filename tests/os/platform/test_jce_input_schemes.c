/* test_jce_input_schemes.c
 *
 * Unit tests for named control schemes (Feature 6.6):
 *   - Two schemes ("KeyboardMouse" vs "Gamepad") bind the SAME action to a
 *     keyboard key (KBM-tagged) vs a gamepad button (GAMEPAD-tagged).
 *   - With KeyboardMouse active, only the key drives the action; the gamepad
 *     button is ignored (its binding is filtered out of jce_actions_update).
 *   - With Gamepad active, only the gamepad button drives the action.
 *   - Auto last-used-device detection: when input arrives from the OTHER
 *     device group, jce_actions_update flips the active scheme to the one that
 *     owns that group.
 *   - Manual override (jce_action_scheme_set_active) pins the scheme and
 *     disables auto switching until re-enabled.
 *   - With NO schemes defined the map behaves exactly like today: every
 *     binding resolves regardless of its device tag.
 *
 * Exercises the REAL jce_actions_update / evaluate path: keys and gamepad
 * state are injected through a JceInput frame (jce_input_capture/apply), then
 * jce_actions_update resolves through the active scheme and the queries are
 * checked.
 */

#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_actions.h>
#include <jce/os/platform/jce_input_device.h>
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

/* Inject a frame holding `key` (or none, when key < 0) and `pad_btn` (or none,
 * when pad_btn < 0) on the player-0 gamepad, then run the real action update.
 *
 * ONE device, in slot 0, rebuilt from scratch every call.  apply() now SHRINKS,
 * so a device_count of 1 both restores this slot and clears every slot above
 * it -- the previous frame cannot leak in, which is what the old comment here
 * had to work around by keeping the pad count pinned at 1.
 *
 * The device carries JCE_INPUT_LAYOUT_GAMEPAD explicitly.  It is not
 * decoration: jce_input_actions.c reads gamepad bindings through
 * jce_input_player_button(), which lands in semantic_rec(), and a record whose
 * layout is RAW answers every semantic query with false.  A frame that forgot
 * to say GAMEPAD here would inject a wheel and drive nothing. */
static void inject_and_update(int key, int pad_btn)
{
    JceInputFrame frame;
    jce_input_capture(g_input, &frame);     /* version-stamped */
    /* capture() snapshots currently-HELD state; clear it so this helper
     * injects exactly what is requested each call. */
    memset(frame.keys_bits, 0, sizeof(frame.keys_bits));
    frame.mouse_buttons = 0;

    memset(frame.devices, 0, sizeof(frame.devices));
    frame.device_count         = 1;
    frame.devices[0].device_id = (uint32_t)JCE_DEVICE_ID_FIRST_HW;
    frame.devices[0].cls       = (uint8_t)JCE_DEVCLASS_GAMEPAD;
    frame.devices[0].layout    = (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD;
    frame.devices[0].player    = 0;
    frame.devices[0].flags     = (uint8_t)(JCE_INPUT_DEVFRAME_FLAG_ACTIVE |
                                           JCE_INPUT_DEVFRAME_FLAG_SEMANTIC);

    if (key >= 0)
        frame.keys_bits[key >> 6] |= (uint64_t)1 << (key & 63);

    if (pad_btn >= 0)
        frame.devices[0].buttons[pad_btn >> 5] |=
            (uint32_t)1 << (pad_btn & 31);

    TEST_ASSERT_TRUE(jce_input_apply(g_input, &frame));
    jce_actions_update(g_actions, g_input);
}

/* Register a "fire" action bound to SPACE (KBM) and pad South (Gamepad), plus
 * the two named schemes.  Returns the action id; scheme ids via out params. */
static int setup_two_schemes(int *kbm_out, int *pad_out)
{
    int fire = jce_action_register(g_actions, "fire");
    TEST_ASSERT_TRUE(fire >= 0);

    JceBinding kb;
    jce_binding_init(&kb, JCE_SRC_KEY, JCE_KEY_SPACE);
    kb.device_group = JCE_DEVICE_KBM;
    TEST_ASSERT_TRUE(jce_action_bind(g_actions, fire, &kb));

    JceBinding gb;
    jce_binding_init(&gb, JCE_SRC_PAD_BUTTON, JCE_GAMEPAD_BUTTON_SOUTH);
    gb.device_group = JCE_DEVICE_GAMEPAD;
    TEST_ASSERT_TRUE(jce_action_bind(g_actions, fire, &gb));

    int kbm = jce_action_scheme_register(g_actions, "KeyboardMouse",
                                         JCE_DEVICE_BIT(JCE_DEVICE_KBM));
    int pad = jce_action_scheme_register(g_actions, "Gamepad",
                                         JCE_DEVICE_BIT(JCE_DEVICE_GAMEPAD));
    TEST_ASSERT_TRUE(kbm >= 0);
    TEST_ASSERT_TRUE(pad >= 0);

    if (kbm_out) *kbm_out = kbm;
    if (pad_out) *pad_out = pad;
    return fire;
}

/* First registered scheme becomes active, auto-switch defaults ON. */
static void test_first_scheme_is_active_auto_on(void)
{
    int kbm, pad;
    (void)setup_two_schemes(&kbm, &pad);

    TEST_ASSERT_EQUAL_INT(2, jce_action_scheme_count(g_actions));
    TEST_ASSERT_EQUAL_INT(kbm, jce_action_scheme_active(g_actions));
    TEST_ASSERT_TRUE(jce_action_scheme_auto(g_actions));
    TEST_ASSERT_EQUAL_INT(kbm, jce_action_scheme_find(g_actions, "KeyboardMouse"));
    TEST_ASSERT_EQUAL_INT(pad, jce_action_scheme_find(g_actions, "Gamepad"));
    TEST_ASSERT_EQUAL_STRING("Gamepad", jce_action_scheme_name(g_actions, pad));
}

/* KeyboardMouse active + auto OFF: only the key drives the action; the gamepad
 * button is ignored even though it is held. */
static void test_kbm_active_ignores_gamepad(void)
{
    int kbm, pad;
    int fire = setup_two_schemes(&kbm, &pad);

    /* Pin KBM so the auto-switcher does not flip on the injected pad input. */
    TEST_ASSERT_TRUE(jce_action_scheme_set_active(g_actions, kbm));
    TEST_ASSERT_FALSE(jce_action_scheme_auto(g_actions));

    /* Gamepad South held, no key -> action stays inactive (binding filtered). */
    inject_and_update(-1, JCE_GAMEPAD_BUTTON_SOUTH);
    TEST_ASSERT_FALSE(jce_action_down(g_actions, fire));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_action_value(g_actions, fire));
    TEST_ASSERT_EQUAL_INT(kbm, jce_action_scheme_active(g_actions)); /* pinned */

    /* Now the SPACE key -> action active. */
    inject_and_update(JCE_KEY_SPACE, -1);
    TEST_ASSERT_TRUE(jce_action_down(g_actions, fire));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_action_value(g_actions, fire));
}

/* Gamepad active + auto OFF: only the gamepad button drives the action; the
 * key is ignored. */
static void test_gamepad_active_ignores_key(void)
{
    int kbm, pad;
    int fire = setup_two_schemes(&kbm, &pad);

    TEST_ASSERT_TRUE(jce_action_scheme_set_active(g_actions, pad));

    /* SPACE held, no pad -> ignored. */
    inject_and_update(JCE_KEY_SPACE, -1);
    TEST_ASSERT_FALSE(jce_action_down(g_actions, fire));
    TEST_ASSERT_EQUAL_INT(pad, jce_action_scheme_active(g_actions));

    /* Pad South -> active. */
    inject_and_update(-1, JCE_GAMEPAD_BUTTON_SOUTH);
    TEST_ASSERT_TRUE(jce_action_down(g_actions, fire));
}

/* Auto last-used-device: starting on KBM, holding a gamepad button flips the
 * active scheme to Gamepad and the gamepad binding then drives the action.
 * Pressing a key flips it back. */
static void test_auto_switch_follows_last_device(void)
{
    int kbm, pad;
    int fire = setup_two_schemes(&kbm, &pad);

    /* Default: KBM active, auto ON. */
    TEST_ASSERT_EQUAL_INT(kbm, jce_action_scheme_active(g_actions));

    /* Gamepad input arrives -> auto-switch to Gamepad, and because the switch
     * happens BEFORE evaluation this same frame, the action is already live. */
    inject_and_update(-1, JCE_GAMEPAD_BUTTON_SOUTH);
    TEST_ASSERT_EQUAL_INT(pad, jce_action_scheme_active(g_actions));
    TEST_ASSERT_EQUAL_INT(JCE_DEVICE_GAMEPAD, jce_action_last_device(g_actions));
    TEST_ASSERT_TRUE(jce_action_down(g_actions, fire));

    /* Keyboard input arrives -> auto-switch back to KeyboardMouse. */
    inject_and_update(JCE_KEY_SPACE, -1);
    TEST_ASSERT_EQUAL_INT(kbm, jce_action_scheme_active(g_actions));
    TEST_ASSERT_EQUAL_INT(JCE_DEVICE_KBM, jce_action_last_device(g_actions));
    TEST_ASSERT_TRUE(jce_action_down(g_actions, fire));
}

/* While on KBM with the key held, an arriving gamepad input flips to Gamepad,
 * which makes the still-held KEY stop driving the action (it is now filtered)
 * and the gamepad button drives it instead. */
static void test_auto_switch_filters_old_device_same_frame(void)
{
    int kbm, pad;
    int fire = setup_two_schemes(&kbm, &pad);

    /* Key drives it under KBM. */
    inject_and_update(JCE_KEY_SPACE, -1);
    TEST_ASSERT_EQUAL_INT(kbm, jce_action_scheme_active(g_actions));
    TEST_ASSERT_TRUE(jce_action_down(g_actions, fire));

    /* Only gamepad now -> switch to Gamepad, gamepad drives it. */
    inject_and_update(-1, JCE_GAMEPAD_BUTTON_SOUTH);
    TEST_ASSERT_EQUAL_INT(pad, jce_action_scheme_active(g_actions));
    TEST_ASSERT_TRUE(jce_action_down(g_actions, fire));
}

/* Manual override pins the scheme: a subsequent other-device input must NOT
 * auto-flip it.  Re-enabling auto restores last-used-device switching. */
static void test_manual_override_pins_then_reenable(void)
{
    int kbm, pad;
    int fire = setup_two_schemes(&kbm, &pad);

    /* Pin Gamepad. */
    TEST_ASSERT_TRUE(jce_action_scheme_set_active(g_actions, pad));
    TEST_ASSERT_FALSE(jce_action_scheme_auto(g_actions));

    /* Keyboard input arrives but must NOT switch away from the pinned scheme;
     * and the key is filtered so the action stays inactive. */
    inject_and_update(JCE_KEY_SPACE, -1);
    TEST_ASSERT_EQUAL_INT(pad, jce_action_scheme_active(g_actions));
    TEST_ASSERT_FALSE(jce_action_down(g_actions, fire));

    /* Re-enable auto -> next keyboard input flips back to KBM. */
    jce_action_scheme_set_auto(g_actions, true);
    TEST_ASSERT_TRUE(jce_action_scheme_auto(g_actions));
    inject_and_update(JCE_KEY_SPACE, -1);
    TEST_ASSERT_EQUAL_INT(kbm, jce_action_scheme_active(g_actions));
    TEST_ASSERT_TRUE(jce_action_down(g_actions, fire));
}

/* A device-agnostic (JCE_DEVICE_NONE) binding resolves under EVERY scheme. */
static void test_device_agnostic_binding_always_active(void)
{
    int pause = jce_action_register(g_actions, "pause");
    TEST_ASSERT_TRUE(pause >= 0);

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_KEY, JCE_KEY_ESCAPE);
    /* device_group left at JCE_DEVICE_NONE on purpose. */
    TEST_ASSERT_TRUE(jce_action_bind(g_actions, pause, &b));

    int kbm = jce_action_scheme_register(g_actions, "KeyboardMouse",
                                         JCE_DEVICE_BIT(JCE_DEVICE_KBM));
    int pad = jce_action_scheme_register(g_actions, "Gamepad",
                                         JCE_DEVICE_BIT(JCE_DEVICE_GAMEPAD));
    TEST_ASSERT_TRUE(kbm >= 0 && pad >= 0);

    /* Pin Gamepad: a KBM-tagged binding would be filtered, but a NONE binding
     * stays active. */
    TEST_ASSERT_TRUE(jce_action_scheme_set_active(g_actions, pad));
    inject_and_update(JCE_KEY_ESCAPE, -1);
    TEST_ASSERT_TRUE(jce_action_down(g_actions, pause));
}

/* No schemes defined => byte-identical to today: every binding resolves
 * regardless of its device tag, the key and the gamepad both drive it, and
 * the active scheme query reports -1. */
static void test_no_schemes_legacy_behavior(void)
{
    int fire = jce_action_register(g_actions, "fire");
    TEST_ASSERT_TRUE(fire >= 0);

    JceBinding kb;
    jce_binding_init(&kb, JCE_SRC_KEY, JCE_KEY_SPACE);
    kb.device_group = JCE_DEVICE_KBM;     /* tag present but no schemes */
    TEST_ASSERT_TRUE(jce_action_bind(g_actions, fire, &kb));

    JceBinding gb;
    jce_binding_init(&gb, JCE_SRC_PAD_BUTTON, JCE_GAMEPAD_BUTTON_SOUTH);
    gb.device_group = JCE_DEVICE_GAMEPAD;
    TEST_ASSERT_TRUE(jce_action_bind(g_actions, fire, &gb));

    TEST_ASSERT_EQUAL_INT(0, jce_action_scheme_count(g_actions));
    TEST_ASSERT_EQUAL_INT(-1, jce_action_scheme_active(g_actions));

    /* Key alone drives it. */
    inject_and_update(JCE_KEY_SPACE, -1);
    TEST_ASSERT_TRUE(jce_action_down(g_actions, fire));

    /* Gamepad alone drives it (no scheme filtering). */
    inject_and_update(-1, JCE_GAMEPAD_BUTTON_SOUTH);
    TEST_ASSERT_TRUE(jce_action_down(g_actions, fire));

    /* No input -> inactive. */
    inject_and_update(-1, -1);
    TEST_ASSERT_FALSE(jce_action_down(g_actions, fire));
    /* Active scheme stays -1 with no schemes defined. */
    TEST_ASSERT_EQUAL_INT(-1, jce_action_scheme_active(g_actions));
}

/* Scheme registration guards: duplicate name rejected; the FIRST scheme is
 * the one that becomes active. */
static void test_scheme_register_guards(void)
{
    int a = jce_action_scheme_register(g_actions, "KeyboardMouse",
                                       JCE_DEVICE_BIT(JCE_DEVICE_KBM));
    TEST_ASSERT_EQUAL_INT(0, a);
    /* duplicate */
    TEST_ASSERT_EQUAL_INT(-1,
        jce_action_scheme_register(g_actions, "KeyboardMouse",
                                   JCE_DEVICE_BIT(JCE_DEVICE_KBM)));
    /* bad args */
    TEST_ASSERT_EQUAL_INT(-1, jce_action_scheme_register(g_actions, NULL, 0));
    TEST_ASSERT_EQUAL_INT(-1, jce_action_scheme_register(g_actions, "", 0));

    /* set_active out-of-range fails. */
    TEST_ASSERT_FALSE(jce_action_scheme_set_active(g_actions, 99));
    TEST_ASSERT_FALSE(jce_action_scheme_set_active(g_actions, -1));

    TEST_ASSERT_EQUAL_INT(JCE_DEVICE_BIT(JCE_DEVICE_KBM),
                          (int)jce_action_scheme_mask(g_actions, a));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_scheme_is_active_auto_on);
    RUN_TEST(test_kbm_active_ignores_gamepad);
    RUN_TEST(test_gamepad_active_ignores_key);
    RUN_TEST(test_auto_switch_follows_last_device);
    RUN_TEST(test_auto_switch_filters_old_device_same_frame);
    RUN_TEST(test_manual_override_pins_then_reenable);
    RUN_TEST(test_device_agnostic_binding_always_active);
    RUN_TEST(test_no_schemes_legacy_behavior);
    RUN_TEST(test_scheme_register_guards);
    return UNITY_END();
}
