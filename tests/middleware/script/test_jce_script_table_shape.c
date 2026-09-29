/* test_jce_script_table_shape.c
 *
 * The `jce` table's KEY SET, read out of a live VM.
 *
 * Every other gate on this surface scans SOURCE: check_binding_parity.py's
 * LUA_BIND_RE matches `register_binding(` call sites, and so does the
 * manifest's registration-parity condition.  Neither can see
 * `jce.json_null`, which is a lua_pushlightuserdata + lua_setfield pair
 * (jce_script.c:1313-1314), not a register_binding call.  A source regex
 * would license dropping the sentinel silently.
 *
 * So this reads the table itself, with pairs(), from inside a script -- the
 * only vantage point that is blind to HOW a key got there.  The expected list
 * is HAND-AUTHORED here on purpose: a list generated from the same manifest
 * that generated the bindings proves only that the generator agrees with
 * itself, which is exactly the self-consistency spec 4.4 warns about.
 *
 * 110 entries: 101 generated functions + 8 hand-written functions + json_null.
 * This line said 81 long after the list below said 105 -- prose beside a
 * golden list is the one part of the file nothing checks, so re-derive it
 * from the list rather than trusting it.
 *
 * SCOPE, so the next reader does not credit this file with more than it does:
 * it checks the KEY SET and each value's type() only.  A key bound to the
 * WRONG function is invisible here; that is the differential harness's job.
 */

#include <jce/middleware/script/jce_script.h>
#include <jce/os/core/jce_alloc.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

/* The mock host's log callback is the wire back to C: the script builds the
 * sorted key list and hands it over, so a mismatch prints a real diff instead
 * of an opaque "the chunk asserted". */
static char g_keys[8192];

static void rec_log(void *user, const char *msg)
{
    (void)user;
    snprintf(g_keys, sizeof g_keys, "%s", msg);
}

void setUp(void)    { g_keys[0] = '\0'; }
void tearDown(void) {}

static const char *const K_EXPECTED =
    "110\n"
    "anim_set_bool:function\n"
    "anim_set_float:function\n"
    "anim_set_int:function\n"
    "anim_set_trigger:function\n"
    "apply_impulse:function\n"
    "asset_read_json:function\n"
    "asset_read_text:function\n"
    "attack_pressed:function\n"
    "audio_is_playing:function\n"
    "audio_play:function\n"
    "audio_set_volume:function\n"
    "audio_stop:function\n"
    "broadcast:function\n"
    "comp_get:function\n"
    "comp_set:function\n"
    "curve_eval:function\n"
    "destroy:function\n"
    "find_by_name:function\n"
    "find_by_prefix:function\n"
    "find_with_tag:function\n"
    "gas_activate:function\n"
    "gas_apply:function\n"
    "gas_get:function\n"
    "get_axis:function\n"
    "get_locale:function\n"
    "get_move:function\n"
    "get_param:function\n"
    "get_param_text:function\n"
    "get_parent:function\n"
    "get_pointer_delta:function\n"
    "get_pointer_wheel:function\n"
    "get_position:function\n"
    "get_rotation:function\n"
    "get_scale:function\n"
    "get_touch:function\n"
    "get_touch_count:function\n"
    "get_velocity:function\n"
    "get_world_position:function\n"
    "has_component:function\n"
    "is_action_down:function\n"
    "is_action_pressed:function\n"
    "is_component_enabled:function\n"
    "is_key_down:function\n"
    "is_pointer_down:function\n"
    "is_transitioning:function\n"
    "json_null:userdata\n"
    "jump_pressed:function\n"
    "line_set_points:function\n"
    "load_game:function\n"
    "log:function\n"
    "move_axis:function\n"
    "music_get_intensity:function\n"
    "music_request_transition:function\n"
    "music_set_intensity:function\n"
    "net_is_client:function\n"
    "net_is_server:function\n"
    "net_spawn:function\n"
    "overlap_box:function\n"
    "overlap_sphere:function\n"
    "particle_burst:function\n"
    "particle_set_color:function\n"
    "particle_set_emitting:function\n"
    "pause:function\n"
    "play_sound:function\n"
    "raycast:function\n"
    "raycast_all:function\n"
    "raycast_filtered:function\n"
    "render_get:function\n"
    "render_set:function\n"
    "request_scene:function\n"
    "rpc_send:function\n"
    "save_game:function\n"
    "send_message:function\n"
    "set_component_enabled:function\n"
    "set_locale:function\n"
    "set_parent:function\n"
    "set_position:function\n"
    "set_rotation:function\n"
    "set_scale:function\n"
    "set_time_scale:function\n"
    "set_velocity:function\n"
    "shake_camera:function\n"
    "spawn:function\n"
    "sprint:function\n"
    "start_coroutine:function\n"
    "stop_coroutine:function\n"
    "tr:function\n"
    "ui_get_dropdown:function\n"
    "ui_get_input_text:function\n"
    "ui_get_progress:function\n"
    "ui_get_scroll:function\n"
    "ui_get_slider:function\n"
    "ui_get_toggle:function\n"
    "ui_set_dropdown:function\n"
    "ui_set_input_text:function\n"
    "ui_set_progress:function\n"
    "ui_set_scroll:function\n"
    "ui_set_slider:function\n"
    "ui_set_text:function\n"
    "ui_set_toggle:function\n"
    "vcam_activate:function\n"
    "vehicle_get_speed:function\n"
    "vehicle_set_input:function\n"
    "wait_seconds:function\n"
    "world_get_hour:function\n"
    "world_get_weather:function\n"
    "world_get_weather_intensity:function\n"
    "world_get_wind_speed:function\n"
    "world_is_daytime:function\n"
    "world_set_hour:function";

/* table.sort's default comparator is Lua's `<` on strings, i.e. strcoll order,
 * which is strcmp order in the "C" locale no test ever leaves.  That is the
 * order K_EXPECTED is written in. */
static const char *const K_PROBE =
    "local n = {}\n"
    "for k, v in pairs(jce) do n[#n+1] = k .. ':' .. type(v) end\n"
    "table.sort(n)\n"
    "jce.log(#n .. '\\n' .. table.concat(n, '\\n'))\n"
    "local M = {}; return M\n";

static void test_jce_table_holds_exactly_the_authored_keys(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.log = rec_log;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_EQUAL(0,
        jce_script_instantiate_source(s, "@tableshape", K_PROBE, 1));
    TEST_ASSERT_EQUAL_STRING(K_EXPECTED, g_keys);
    jce_script_destroy(s);
}

/* jce.json_null must be the SAME lightuserdata the JSON reader pushes for a
 * JSON null -- jce_script.c:1313 and :517 must name one token.  Two tokens at
 * two addresses compare unequal and every script written `v == jce.json_null`
 * silently stops matching; silently, because both sides are still userdata and
 * type() still says so.
 *
 * `jce.json_null == jce.json_null` does NOT test that.  It compares the field
 * with itself, is true of any value whatever, and survives repointing :1313 at
 * a second token -- measured, see the commit message.  The only assertion that
 * discriminates routes a null through the READER, so this drives a mock
 * read_file rather than reading the field twice.
 */
static const char *const JSON_NULL_DOC = "{\"missing\":null}";

static void *mock_read_file(void *user, const char *path, uint64_t *out_size)
{
    size_t n;
    void *bytes;

    (void)user;
    if (!path || strcmp(path, "data/null.json") != 0) {
        *out_size = 0u;
        return NULL;
    }
    n = strlen(JSON_NULL_DOC);
    bytes = jce_malloc(n);          /* the engine frees this with jce_free */
    if (!bytes) {
        *out_size = 0u;
        return NULL;
    }
    memcpy(bytes, JSON_NULL_DOC, n);
    *out_size = (uint64_t)n;
    return bytes;
}

static void test_json_null_is_the_reader_sentinel(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.log       = rec_log;
    h.read_file = mock_read_file;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_EQUAL(0, jce_script_instantiate_source(s, "@sentinel",
        "local d, err = jce.asset_read_json('data/null.json')\n"
        "assert(d, 'read failed: ' .. tostring(err))\n"
        "jce.log(tostring(d.missing == jce.json_null)\n"
        "        .. ' ' .. type(jce.json_null))\n"
        "local M = {}; return M\n", 1));
    TEST_ASSERT_EQUAL_STRING("true userdata", g_keys);
    jce_script_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_jce_table_holds_exactly_the_authored_keys);
    RUN_TEST(test_json_null_is_the_reader_sentinel);
    return UNITY_END();
}
