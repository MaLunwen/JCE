/*
 * test_jce_runtime_is_key_down.c — jce.is_key_down must answer the real
 * keyboard, and must stop answering when the host stops supplying one.
 *
 * WHY THIS EXISTS.  jce.is_key_down(keycode) was registered, callable and
 * documented in <jce/middleware/script/jce_script.h>, and it RETURNED FALSE
 * FOREVER.  host.is_key_down was the one JceScriptHost member with no writer
 * in jce_rt_script.c, so the slot stayed NULL and the generated binding took
 * its `: false` branch -- in the editor's Play and in a shipped game alike.
 * The runtime had nothing to answer with: it holds a JceRuntimeInput, not a
 * JceInput, and jce_actions_update(a, input) uses the raw input transiently
 * and keeps no pointer.
 *
 * The mock-host binding tests could not have caught it: they install a mock
 * whose callbacks are all non-NULL, so they measure the marshalling and never
 * the runtime's own host table.  This one runs the REAL
 * jce_runtime_create / jce_runtime_step path.
 *
 * The keyboard is fabricated headlessly through jce_input_apply, the public
 * replay entry point -- jce_input_create installs no backend, which is what
 * lets the unit suite hold an input object with no window and no SDL.
 *
 * THREE properties, and the third is the one that is easy to get wrong:
 *
 *   1. no keyboard supplied  -> false   (also the pre-fix behaviour, so it
 *      doubles as a positive control that the script runs at all)
 *   2. key held              -> true
 *   3. supplied ONCE, stepped TWICE -> false on the second step.  The borrow
 *      expires with the frame sample, which is what stops an unfocused game
 *      view from letting a script read keys the user is typing into another
 *      panel.  Without the clear this test would read true and the editor
 *      would leak keystrokes into Play.
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_keys.h>

#include "unity.h"

#include "jce_test_file_util.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define LUA_FILE "test_is_key_down.lua"

/* Reports the binding's answer as position.x, which is readable through the
 * public scene API.  1 = held, 0 = not. */
static const char *const PROBE_LUA =
    "local M = {}\n"
    "function M:on_update(dt)\n"
    "  local down = jce.is_key_down(26)\n"   /* 26 = JCE_KEY_W (HID scancode) */
    "  jce.set_position(self.entity, down and 1 or 0, 0, 0)\n"
    "end\n"
    "return M\n";

void setUp(void)    {}
void tearDown(void) { remove(LUA_FILE); }

static JceEntity make_scripted(JceScene *s, const char *path)
{
    JceEntity          e = jce_scene_create_entity(s, "probe");
    JceTransform       t;
    JceScriptComponent sc;

    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    /* Deliberately not the value the script writes for either answer, so a
     * script that never ran cannot be mistaken for one that answered 0. */
    t.position.x = -7.0f;
    jce_scene_set_transform(s, e, &t);

    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", path);
    jce_scene_set_script(s, e, &sc);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_SCRIPT, true);
    return e;
}

static float probe_x(JceScene *s, JceEntity e)
{
    JceTransform *t = jce_scene_get_transform(s, e);
    TEST_ASSERT_NOT_NULL(t);
    return t->position.x;
}

static void hold_key(JceInput *in, int scancode, bool down)
{
    JceInputFrame f;
    /* Captured rather than zeroed: apply() rejects a frame whose `version` is
     * not the current one, and capture fills that (plus the device
     * high-water mark) without this test hardcoding either. */
    jce_input_capture(in, &f);
    memset(f.keys_bits, 0, sizeof f.keys_bits);
    if (down)
        f.keys_bits[scancode >> 6] |= (uint64_t)1 << (scancode & 63);
    TEST_ASSERT_TRUE_MESSAGE(jce_input_apply(in, &f),
        "jce_input_apply refused the fabricated frame - the rest of this test "
        "would be measuring an empty keyboard");
}

static void supply(JceRuntime *rt, const JceInput *kb)
{
    JceRuntimeInput ri;
    memset(&ri, 0, sizeof ri);
    ri.speed_mult = 1.0f;
    ri.keyboard   = kb;
    jce_runtime_set_input(rt, &ri);
}

static void test_is_key_down_answers_the_real_keyboard(void)
{
    JceScene      *s;
    JceEntity      e;
    JceRuntimeDesc desc;
    JceRuntime    *rt;
    JceInput      *kb;

    jce_test_write_file(LUA_FILE, PROBE_LUA);

    s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    e = make_scripted(s, LUA_FILE);

    memset(&desc, 0, sizeof desc);
    desc.scene          = s;
    desc.enable_physics = false;
    rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    kb = jce_input_create();
    TEST_ASSERT_NOT_NULL_MESSAGE(kb,
        "jce_input_create failed headlessly; it installs no backend and is "
        "supposed to work without a window");

    /* (1) No keyboard supplied.  This is exactly what every host did before
     * the field existed, so a failure here means the script is not running
     * and nothing below would mean anything. */
    supply(rt, NULL);
    jce_runtime_step(rt, 0.016f);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, probe_x(s, e),
        "with no keyboard the binding must answer false - and -7 here would "
        "mean the probe script never ran at all");

    /* (2) W held. */
    hold_key(kb, JCE_KEY_W, true);
    TEST_ASSERT_TRUE_MESSAGE(jce_input_key_down(kb, JCE_KEY_W),
        "the fabricated keyboard does not report W held; the assertion below "
        "would then pass or fail for the wrong reason");
    supply(rt, kb);
    jce_runtime_step(rt, 0.016f);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, probe_x(s, e),
        "jce.is_key_down answered false while W was held - the binding is "
        "inert, which is the defect this test exists for");

    /* (3) Supplied once, stepped twice.  The borrow expires with the sample. */
    jce_runtime_step(rt, 0.016f);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, probe_x(s, e),
        "the keyboard borrow outlived its frame - an unfocused game view "
        "would keep feeding a Play script the keys being typed elsewhere");

    /* (4) Released, and supplied again: the answer tracks rather than latching. */
    hold_key(kb, JCE_KEY_W, false);
    supply(rt, kb);
    jce_runtime_step(rt, 0.016f);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, probe_x(s, e),
        "released W still read as held");

    hold_key(kb, JCE_KEY_W, true);
    supply(rt, kb);
    jce_runtime_step(rt, 0.016f);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, probe_x(s, e),
        "W did not read as held again after a release - the answer latches "
        "instead of tracking");

    jce_input_destroy(kb);
    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_is_key_down_answers_the_real_keyboard);
    return UNITY_END();
}
