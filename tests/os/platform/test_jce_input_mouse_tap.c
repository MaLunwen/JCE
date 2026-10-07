/* Event-seam rapid clicks must survive sampling and the existing wire ABI. */
#include "unity.h"
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_actions.h>
#include <jce/os/platform/jce_input_event.h>
#include <string.h>

static JceInput *input;
void setUp(void) { input = jce_input_create(); }
void tearDown(void) { jce_input_destroy(input); }

static void button(int number, bool down)
{
    JceInputEvent event;
    memset(&event, 0, sizeof event);
    event.size = (uint32_t)sizeof event;
    event.kind = JCE_INPUT_EVENT_MOUSE_BUTTON;
    event.mbutton.button = number;
    event.mbutton.down = down;
    jce_input_submit(input, &event, 1);
}

static void test_tap_all_buttons_and_following_release(void)
{
    int number;
    for (number = 1; number <= 32; ++number) {
        button(number, true);
        button(number, false);
        TEST_ASSERT_TRUE(jce_input_mouse_button(input, number));
        TEST_ASSERT_TRUE(jce_input_mouse_button_pressed(input, number));
        TEST_ASSERT_FALSE(jce_input_mouse_button_released(input, number));
    }
    jce_input_update(input);
    for (number = 1; number <= 32; ++number) {
        TEST_ASSERT_FALSE(jce_input_mouse_button(input, number));
        TEST_ASSERT_FALSE(jce_input_mouse_button_pressed(input, number));
        TEST_ASSERT_TRUE(jce_input_mouse_button_released(input, number));
    }
    jce_input_update(input);
    TEST_ASSERT_FALSE(jce_input_mouse_button_released(input, 1));
}

static void test_adjacent_frame_taps_do_not_become_physical_holds(void)
{
    int frame;
    for (frame = 0; frame < 3; ++frame) {
        jce_input_update(input);
        button(1, true);
        button(1, false);
        TEST_ASSERT_TRUE(jce_input_mouse_button(input, 1));
        TEST_ASSERT_TRUE(jce_input_mouse_button_pressed(input, 1));
        TEST_ASSERT_FALSE(jce_input_mouse_button_released(input, 1));
    }
    jce_input_update(input);
    TEST_ASSERT_TRUE(jce_input_mouse_button_released(input, 1));
}

static void test_held_duplicate_down_and_up_down_preserve_state(void)
{
    button(1, true);
    button(1, true);
    TEST_ASSERT_TRUE(jce_input_mouse_button_pressed(input, 1));
    jce_input_update(input);
    button(1, true);
    TEST_ASSERT_TRUE(jce_input_mouse_button(input, 1));
    TEST_ASSERT_FALSE(jce_input_mouse_button_pressed(input, 1));
    button(1, false);
    button(1, true);
    TEST_ASSERT_TRUE(jce_input_mouse_button(input, 1));
    TEST_ASSERT_FALSE(jce_input_mouse_button_pressed(input, 1));
    TEST_ASSERT_FALSE(jce_input_mouse_button_released(input, 1));
    jce_input_update(input);
    button(1, false);
    TEST_ASSERT_TRUE(jce_input_mouse_button_released(input, 1));
}

static void test_unheld_up_down_and_invalid_buttons(void)
{
    button(1, false);
    button(1, true);
    TEST_ASSERT_TRUE(jce_input_mouse_button_pressed(input, 1));
    button(0, true);
    button(33, true);
    TEST_ASSERT_FALSE(jce_input_mouse_button(input, 0));
    TEST_ASSERT_FALSE(jce_input_mouse_button(input, 33));
    JceInputFrame frame;
    jce_input_capture(input, &frame);
    TEST_ASSERT_EQUAL_UINT32(1u, frame.mouse_buttons);
}

static void test_pointer_capture_replay_and_action_tap_release(void)
{
    JceInput *replay = jce_input_create();
    JceInputActions *actions = jce_actions_create();
    JceInputActions *replay_actions = jce_actions_create();
    JceBinding binding;
    JceInputFrame frame;
    float x, y;
    bool down;
    int action = jce_action_register(actions, "click");
    int replay_action = jce_action_register(replay_actions, "click");
    jce_binding_init(&binding, JCE_SRC_MOUSE_BUTTON, 1);
    TEST_ASSERT_TRUE(jce_action_bind(actions, action, &binding));
    TEST_ASSERT_TRUE(jce_action_bind(replay_actions, replay_action, &binding));
    button(1, true);
    button(1, false);
    TEST_ASSERT_TRUE(jce_input_primary_pointer(input, 100, 100, &x, &y, &down));
    TEST_ASSERT_TRUE(down);
    jce_input_capture(input, &frame);
    TEST_ASSERT_EQUAL_UINT32(1u, frame.mouse_buttons);
    TEST_ASSERT_TRUE(jce_input_apply(replay, &frame));
    TEST_ASSERT_TRUE(jce_input_mouse_button_pressed(replay, 1));
    jce_actions_update(actions, input);
    jce_actions_update(replay_actions, replay);
    TEST_ASSERT_TRUE(jce_action_pressed(actions, action));
    TEST_ASSERT_TRUE(jce_action_pressed(replay_actions, replay_action));
    jce_input_update(input);
    jce_input_update(replay);
    jce_input_capture(input, &frame);
    TEST_ASSERT_EQUAL_UINT32(0u, frame.mouse_buttons);
    TEST_ASSERT_TRUE(jce_input_apply(replay, &frame));
    TEST_ASSERT_TRUE(jce_input_mouse_button_released(input, 1));
    TEST_ASSERT_TRUE(jce_input_mouse_button_released(replay, 1));
    jce_actions_update(actions, input);
    jce_actions_update(replay_actions, replay);
    TEST_ASSERT_TRUE(jce_action_released(actions, action));
    TEST_ASSERT_TRUE(jce_action_released(replay_actions, replay_action));
    TEST_ASSERT_TRUE(jce_input_primary_pointer(input, 100, 100, &x, &y, &down));
    TEST_ASSERT_FALSE(down);
    jce_actions_destroy(actions);
    jce_actions_destroy(replay_actions);
    jce_input_destroy(replay);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tap_all_buttons_and_following_release);
    RUN_TEST(test_adjacent_frame_taps_do_not_become_physical_holds);
    RUN_TEST(test_held_duplicate_down_and_up_down_preserve_state);
    RUN_TEST(test_unheld_up_down_and_invalid_buttons);
    RUN_TEST(test_pointer_capture_replay_and_action_tap_release);
    return UNITY_END();
}
