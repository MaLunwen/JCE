/* test_jce_script_action.c
 *
 * Headless coverage for the data-driven action/axis input bridge (generic
 * input, beyond the fixed move/jump/sprint/attack fields):
 *   jce.is_action_down(name)    -> bool
 *   jce.is_action_pressed(name) -> bool
 *   jce.get_axis(name)          -> number
 *
 * The VM (jce_script.c) marshals each to a JceScriptHost callback
 * (action_down / action_pressed / action_axis); the runtime backs them with the
 * live JceInputActions map (jce_action_find + down/pressed/value).  This drives
 * a MOCK host and asserts the C<->Lua marshalling, the bool/number return
 * round-trip, and the NULL-callback safe defaults.
 */

#include <jce/middleware/script/jce_script.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    char last_name[64];
    int  down_calls, pressed_calls, axis_calls;
} Recorder;

static Recorder g_rec;

/* Mock action map: "fire" is held, "jump" is pressed, "throttle" reads 0.75. */
static bool mock_down(void *user, const char *name)
{
    Recorder *r = (Recorder *)user; r->down_calls++;
    snprintf(r->last_name, sizeof r->last_name, "%s", name ? name : "");
    return name && strcmp(name, "fire") == 0;
}
static bool mock_pressed(void *user, const char *name)
{
    Recorder *r = (Recorder *)user; r->pressed_calls++;
    snprintf(r->last_name, sizeof r->last_name, "%s", name ? name : "");
    return name && strcmp(name, "jump") == 0;
}
static float mock_axis(void *user, const char *name)
{
    Recorder *r = (Recorder *)user; r->axis_calls++;
    snprintf(r->last_name, sizeof r->last_name, "%s", name ? name : "");
    return (name && strcmp(name, "throttle") == 0) ? 0.75f : 0.0f;
}

void setUp(void)    { memset(&g_rec, 0, sizeof g_rec); }
void tearDown(void) {}

/* ── down / pressed / axis return round-trip + name marshalling ─────────── */
static void test_action_round_trip(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user           = &g_rec;
    h.action_down    = mock_down;
    h.action_pressed = mock_pressed;
    h.action_axis    = mock_axis;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance a = jce_script_instantiate_source(s, "@act",
        "RES = {\n"
        "  fire   = jce.is_action_down('fire'),\n"
        "  nofire = jce.is_action_down('walk'),\n"
        "  jmp    = jce.is_action_pressed('jump'),\n"
        "  thr    = jce.get_axis('throttle'),\n"
        "  zero   = jce.get_axis('steer'),\n"
        "}\n"
        "return {}\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, a);

    JceScriptInstance chk = jce_script_instantiate_source(s, "@act_check",
        "assert(RES.fire   == true,  'fire held -> true')\n"
        "assert(RES.nofire == false, 'walk held -> false')\n"
        "assert(RES.jmp    == true,  'jump pressed -> true')\n"
        "assert(math.abs(RES.thr - 0.75) < 1e-5, 'throttle axis -> 0.75')\n"
        "assert(RES.zero   == 0.0,   'steer axis -> 0')\n"
        "return {}\n", 2);
    TEST_ASSERT_NOT_EQUAL(0, chk);

    TEST_ASSERT_TRUE(g_rec.down_calls >= 2);
    TEST_ASSERT_TRUE(g_rec.axis_calls >= 2);

    jce_script_destroy(s);
}

/* ── NULL action callbacks → safe defaults (false / 0) ──────────────────── */
static void test_action_null_defaults(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user = &g_rec;   /* have_host true, all action cbs NULL */

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance a = jce_script_instantiate_source(s, "@nul",
        "RES = { d = jce.is_action_down('x'), "
        "p = jce.is_action_pressed('x'), ax = jce.get_axis('x') }\n"
        "return {}\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, a);
    TEST_ASSERT_EQUAL_INT(0, g_rec.down_calls);

    JceScriptInstance chk = jce_script_instantiate_source(s, "@nul_check",
        "assert(RES.d  == false, 'NULL down -> false')\n"
        "assert(RES.p  == false, 'NULL pressed -> false')\n"
        "assert(RES.ax == 0.0,   'NULL axis -> 0')\n"
        "return {}\n", 2);
    TEST_ASSERT_NOT_EQUAL(0, chk);

    jce_script_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_action_round_trip);
    RUN_TEST(test_action_null_defaults);
    return UNITY_END();
}
