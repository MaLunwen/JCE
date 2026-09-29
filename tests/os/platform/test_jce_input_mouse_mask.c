/* test_jce_input_mouse_mask.c
 *
 * The mouse button mask is off by one BY DESIGN and this test says so.
 *
 * JCE numbers mouse buttons from 1 (left=1, middle=2, right=3), matching SDL.
 * The bit for button b is therefore 1u << (b - 1): button 1 is bit 0.  That
 * one subtraction is the entire contract shared by three things that must
 * agree byte for byte:
 *
 *   - the query API      jce_input_mouse_button(input, 1)
 *   - the wire format    JceInputFrame.mouse_buttons  (.jirc record/replay)
 *   - the action evaluator's JCE_SRC_MOUSE_BUTTON case
 *
 * Writing the obvious `1u << button` shifts all three at once and every
 * existing input test stays green, because none of them touches a mouse
 * button.  This test is deliberately written while SDL_BUTTON_MASK is still
 * the implementation: it pins the behaviour being preserved, not the
 * behaviour of whatever replaces it.
 *
 * State is injected through jce_input_apply (the record/replay path) so the
 * test needs no window, no SDL event pump and no mouse.
 */

#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_keys.h>

#include "unity.h"

#include <string.h>

static JceInput *g_input = NULL;

void setUp(void)
{
    g_input = jce_input_create();
}

void tearDown(void)
{
    jce_input_destroy(g_input);
    g_input = NULL;
}

/* Push an exact mouse-button bitmask into the live state. */
static void apply_mouse_bits(uint32_t bits)
{
    JceInputFrame f;
    memset(&f, 0, sizeof(f));
    f.version       = JCE_INPUT_FRAME_VERSION;
    f.key_count     = (uint32_t)JCE_KEY_COUNT;
    f.mouse_buttons = bits;
    TEST_ASSERT_TRUE(jce_input_apply(g_input, &f));
}

static void test_button_one_is_bit_zero(void)
{
    apply_mouse_bits(1u << 0);
    TEST_ASSERT_TRUE (jce_input_mouse_button(g_input, 1));
    TEST_ASSERT_FALSE(jce_input_mouse_button(g_input, 2));
    TEST_ASSERT_FALSE(jce_input_mouse_button(g_input, 3));
}

static void test_button_two_is_bit_one(void)
{
    apply_mouse_bits(1u << 1);
    TEST_ASSERT_FALSE(jce_input_mouse_button(g_input, 1));
    TEST_ASSERT_TRUE (jce_input_mouse_button(g_input, 2));
    TEST_ASSERT_FALSE(jce_input_mouse_button(g_input, 3));
}

static void test_button_three_is_bit_two(void)
{
    apply_mouse_bits(1u << 2);
    TEST_ASSERT_FALSE(jce_input_mouse_button(g_input, 1));
    TEST_ASSERT_FALSE(jce_input_mouse_button(g_input, 2));
    TEST_ASSERT_TRUE (jce_input_mouse_button(g_input, 3));
}

static void test_capture_round_trips_the_same_bits(void)
{
    /* Left + right held: bits 0 and 2, and nothing else. */
    apply_mouse_bits((1u << 0) | (1u << 2));

    JceInputFrame out;
    memset(&out, 0, sizeof(out));
    jce_input_capture(g_input, &out);

    TEST_ASSERT_EQUAL_UINT32((1u << 0) | (1u << 2), out.mouse_buttons);
}

static void test_pressed_and_released_use_the_same_bit(void)
{
    /* Frame 1: nothing held. */
    apply_mouse_bits(0u);
    jce_input_update(g_input);          /* cur -> prev */

    /* Frame 2: button 1 goes down. */
    apply_mouse_bits(1u << 0);
    TEST_ASSERT_TRUE (jce_input_mouse_button_pressed(g_input, 1));
    TEST_ASSERT_FALSE(jce_input_mouse_button_pressed(g_input, 2));
    TEST_ASSERT_FALSE(jce_input_mouse_button_released(g_input, 1));

    /* Frame 3: button 1 comes back up. */
    jce_input_update(g_input);
    apply_mouse_bits(0u);
    TEST_ASSERT_TRUE (jce_input_mouse_button_released(g_input, 1));
    TEST_ASSERT_FALSE(jce_input_mouse_button(g_input, 1));
}

static void test_high_button_still_lands_one_bit_below_its_number(void)
{
    /* Button 5 (a thumb button) is bit 4, not bit 5. */
    apply_mouse_bits(1u << 4);
    TEST_ASSERT_TRUE (jce_input_mouse_button(g_input, 5));
    TEST_ASSERT_FALSE(jce_input_mouse_button(g_input, 6));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_button_one_is_bit_zero);
    RUN_TEST(test_button_two_is_bit_one);
    RUN_TEST(test_button_three_is_bit_two);
    RUN_TEST(test_capture_round_trips_the_same_bits);
    RUN_TEST(test_pressed_and_released_use_the_same_bit);
    RUN_TEST(test_high_button_still_lands_one_bit_below_its_number);
    return UNITY_END();
}
