/* test_jce_actions_from_input_frame.c
 *
 * Top 6 seam: a synthesized JceInputFrame -> jce_input_apply -> jce_actions_update
 * resolves a NAMED action with correct edge semantics.  This is exactly the
 * editor's ImGui->frame->actions chain (jce_editor_input_actions_live), minus
 * the ImGui half — the mechanism that lets editor-Play scripts read authored
 * data-driven actions (jce.is_action_down / is_action_pressed) like a shipped
 * game.  Mirrors tests/os/platform/test_jce_input_serialize.c's harness.
 */

#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_actions.h>
#include <jce/os/platform/jce_keys.h>

#include "unity.h"

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static void frame_set(JceInputFrame *fr, const int *codes, int n)
{
    memset(fr, 0, sizeof *fr);
    fr->version   = JCE_INPUT_FRAME_VERSION;
    fr->key_count = JCE_KEY_COUNT;
    for (int i = 0; i < n; ++i) {
        int sc = codes[i];
        fr->keys_bits[sc >> 6] |= (uint64_t)1 << (sc & 63);
    }
}

static void test_frame_drives_named_action(void)
{
    JceInputActions *a = jce_actions_create();
    TEST_ASSERT_NOT_NULL(a);
    int fire = jce_action_register(a, "fire");
    TEST_ASSERT_TRUE(fire >= 0);
    /* jce_binding_init, not memset: a memset leaves size == 0 and
     * jce_action_bind refuses the record.  It also gives scale its 1.0 --
     * scale 0 silences the binding (see test_jce_input_composite). */
    JceBinding b;
    jce_binding_init(&b, JCE_SRC_KEY, JCE_KEY_F);
    TEST_ASSERT_TRUE(jce_action_bind(a, fire, &b));
    TEST_ASSERT_EQUAL_INT(fire, jce_action_find(a, "fire"));

    JceInput *in = jce_input_create();
    TEST_ASSERT_NOT_NULL(in);
    JceInputFrame fr;
    const int down[] = { JCE_KEY_F };

    /* Frame 1: F down -> held + pressed (rising edge). */
    jce_input_update(in);
    frame_set(&fr, down, 1);
    TEST_ASSERT_TRUE(jce_input_apply(in, &fr));
    jce_actions_update(a, in);
    TEST_ASSERT_TRUE(jce_action_down(a, fire));
    TEST_ASSERT_TRUE(jce_action_pressed(a, fire));

    /* Frame 2: still down -> held, NOT pressed. */
    jce_input_update(in);
    frame_set(&fr, down, 1);
    TEST_ASSERT_TRUE(jce_input_apply(in, &fr));
    jce_actions_update(a, in);
    TEST_ASSERT_TRUE(jce_action_down(a, fire));
    TEST_ASSERT_FALSE(jce_action_pressed(a, fire));

    /* Frame 3: released -> up + released edge. */
    jce_input_update(in);
    frame_set(&fr, NULL, 0);
    TEST_ASSERT_TRUE(jce_input_apply(in, &fr));
    jce_actions_update(a, in);
    TEST_ASSERT_FALSE(jce_action_down(a, fire));
    TEST_ASSERT_TRUE(jce_action_released(a, fire));

    jce_input_destroy(in);
    jce_actions_destroy(a);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_frame_drives_named_action);
    return UNITY_END();
}
