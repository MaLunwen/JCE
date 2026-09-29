/* test_jce_script_cpp_differential.gen.cpp -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * THE CROSS-LANGUAGE DIFFERENTIAL: Lua is the reference implementation.
 *
 * Batch 1 proved the generated Lua bindings equivalent to the hand-written
 * originals over 87 cases, and said in its own commit that the oracle it spent
 * "disappears the moment they are deleted and never comes back".  What did not
 * disappear is Lua ITSELF: a shipped, tested, independently generated
 * implementation of the same manifest.  So every case below is driven through
 * a real Lua VM and through the C++ wrapper over ONE recording mock host, and
 * two things are compared:
 *
 *   * the FULL RESULT, type and value per slot.  `nil` vs `0` vs `false` are
 *     three different strings, so none of them can pass for another.
 *   * the HOST-CALL TRACE.  A wrapper that produces the right value by calling
 *     the wrong member, or the right member with transposed arguments, is
 *     invisible to a result comparison and visible here.  Argument values are
 *     DISTINCT PER SLOT so a transposition has something to show.
 *
 * NOT CIRCULAR, in the one way that matters: the C++ result is projected onto
 * the comparison form by hand-written, TYPE-KEYED overloads in
 * jce_script_cpp_differential.hpp.  There is no per-entry projection, so a
 * per-entry defect in the wrapper has nothing to be mirrored by.
 *
 * WHAT IT CANNOT SEE, stated rather than left to be discovered:
 *   * an entry the manifest omits entirely -- neither side has it.  The gate's
 *     totality condition covers that.
 *   * a wrong value both implementations agree on because the MOCK is wrong.
 *   * the seven hand-written entries.  Neither the C ABI nor this wrapper
 *     exposes them, by rule; only Lua has them.
 *   * a transposition inside _arg_values in emit_cpp.py, which permutes both
 *     sides identically.  That is the M1 shape from batch 1 and it is why the
 *     values are per-slot distinct: the wrapper is what this compares, and a
 *     transposition THERE is red on the trace.
 *
 * The C ABI in the middle is being exercised too, and by more than its own
 * suite: tests/scripting/c_abi covers ten entry points by hand, and this file
 * drives all 101 of them.
 */

#include "jce_script_cpp_differential.hpp"

#include "doctest.h"

#include <jce/middleware/script/jce_script.h>

#include <cstdio>
#include <cstring>

using jce::script::Api;
using jce::script::Entity;

/* ------------------------------------------------------------------ *
 *  The recording mock host.  ONE host, driven by both languages.
 *
 *  extern "C" because JceScriptHost's members are C function pointers;
 *  a C++-linkage function assigned to one is a type mismatch every
 *  toolchain happens to tolerate and none is required to.
 *
 *  `log` is NOT traced: it is how the Lua side reports its result, so
 *  tracing it would put the answer inside the evidence.
 * ------------------------------------------------------------------ */
extern "C" {

static void mk_log(void *user, const char *msg)
{
    (void)user;
    jce::diff::capture().log += (msg != NULL ? msg : "(null)");
    jce::diff::capture().log += '\n';
}

static bool mk_get_position(void *user, JceScriptEntity e, float out_xyz[3])
{
    (void)user;
    jce::diff::trace_add("get_position");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    out_xyz[0] = 9.5f;
    out_xyz[1] = 10.5f;
    out_xyz[2] = 11.5f;
    return true;
}

static void mk_set_position(void *user, JceScriptEntity e, float x, float y, float z)
{
    (void)user;
    jce::diff::trace_add("set_position");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_num(static_cast<double>(x));
    jce::diff::trace_num(static_cast<double>(y));
    jce::diff::trace_num(static_cast<double>(z));
    jce::diff::trace_end();
}

static bool mk_get_rotation(void *user, JceScriptEntity e, float out_euler_deg[3])
{
    (void)user;
    jce::diff::trace_add("get_rotation");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    out_euler_deg[0] = 41.5f;
    out_euler_deg[1] = 42.5f;
    out_euler_deg[2] = 43.5f;
    return true;
}

static void mk_set_rotation(void *user, JceScriptEntity e, float x, float y, float z)
{
    (void)user;
    jce::diff::trace_add("set_rotation");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_num(static_cast<double>(x));
    jce::diff::trace_num(static_cast<double>(y));
    jce::diff::trace_num(static_cast<double>(z));
    jce::diff::trace_end();
}

static bool mk_get_scale(void *user, JceScriptEntity e, float out_xyz[3])
{
    (void)user;
    jce::diff::trace_add("get_scale");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    out_xyz[0] = 73.5f;
    out_xyz[1] = 74.5f;
    out_xyz[2] = 75.5f;
    return true;
}

static bool mk_get_world_position(void *user, JceScriptEntity e, float out_xyz[3])
{
    (void)user;
    jce::diff::trace_add("get_world_position");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    out_xyz[0] = 89.5f;
    out_xyz[1] = 90.5f;
    out_xyz[2] = 91.5f;
    return true;
}

static void mk_set_scale(void *user, JceScriptEntity e, float x, float y, float z)
{
    (void)user;
    jce::diff::trace_add("set_scale");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_num(static_cast<double>(x));
    jce::diff::trace_num(static_cast<double>(y));
    jce::diff::trace_num(static_cast<double>(z));
    jce::diff::trace_end();
}

static bool mk_set_parent(void *user, JceScriptEntity child, JceScriptEntity parent, bool preserve_world)
{
    (void)user;
    jce::diff::trace_add("set_parent");
    jce::diff::trace_num(static_cast<double>(child));
    jce::diff::trace_num(static_cast<double>(parent));
    jce::diff::trace_num(preserve_world ? 1 : 0);
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static JceScriptEntity mk_get_parent(void *user, JceScriptEntity child)
{
    (void)user;
    jce::diff::trace_add("get_parent");
    jce::diff::trace_num(static_cast<double>(child));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return 0;
    return static_cast<JceScriptEntity>(408);
}

static bool mk_is_key_down(void *user, int keycode)
{
    (void)user;
    jce::diff::trace_add("is_key_down");
    jce::diff::trace_num(static_cast<double>(keycode));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static JceScriptEntity mk_find_with_tag(void *user, const char * tag)
{
    (void)user;
    jce::diff::trace_add("find_with_tag");
    jce::diff::trace_str(tag);
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return 0;
    return static_cast<JceScriptEntity>(410);
}

static void mk_destroy_entity(void *user, JceScriptEntity e)
{
    (void)user;
    jce::diff::trace_add("destroy_entity");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_end();
}

static JceScriptEntity mk_spawn(void *user, const char * prefab_path, float x, float y, float z)
{
    (void)user;
    jce::diff::trace_add("spawn");
    jce::diff::trace_str(prefab_path);
    jce::diff::trace_num(static_cast<double>(x));
    jce::diff::trace_num(static_cast<double>(y));
    jce::diff::trace_num(static_cast<double>(z));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return 0;
    return static_cast<JceScriptEntity>(412);
}

static void mk_move_axis(void *user, float out_xz[2])
{
    (void)user;
    jce::diff::trace_add("move_axis");
    jce::diff::trace_end();
    out_xz[0] = 217.5f;
    out_xz[1] = 218.5f;
}

static bool mk_input_button(void *user, int button)
{
    (void)user;
    jce::diff::trace_add("input_button");
    jce::diff::trace_num(static_cast<double>(button));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static void mk_set_time_scale(void *user, float scale)
{
    (void)user;
    jce::diff::trace_add("set_time_scale");
    jce::diff::trace_num(static_cast<double>(scale));
    jce::diff::trace_end();
}

static void mk_set_paused(void *user, bool paused)
{
    (void)user;
    jce::diff::trace_add("set_paused");
    jce::diff::trace_num(paused ? 1 : 0);
    jce::diff::trace_end();
}

static void mk_shake_camera(void *user, float amount)
{
    (void)user;
    jce::diff::trace_add("shake_camera");
    jce::diff::trace_num(static_cast<double>(amount));
    jce::diff::trace_end();
}

static void mk_music_set_intensity(void *user, float intensity)
{
    (void)user;
    jce::diff::trace_add("music_set_intensity");
    jce::diff::trace_num(static_cast<double>(intensity));
    jce::diff::trace_end();
}

static float mk_music_get_intensity(void *user)
{
    (void)user;
    jce::diff::trace_add("music_get_intensity");
    jce::diff::trace_end();
    return 28.5f;
}

static float mk_music_request_transition(void *user, int to_segment)
{
    (void)user;
    jce::diff::trace_add("music_request_transition");
    jce::diff::trace_num(static_cast<double>(to_segment));
    jce::diff::trace_end();
    return 29.5f;
}

static bool mk_gas_activate(void *user, JceScriptEntity e, uint32_t ability_id)
{
    (void)user;
    jce::diff::trace_add("gas_activate");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_num(static_cast<double>(ability_id));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static bool mk_gas_get(void *user, JceScriptEntity e, const char * attr_name, float *out_value)
{
    (void)user;
    jce::diff::trace_add("gas_get");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_str(attr_name);
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    *out_value = 361.5f;
    return true;
}

static bool mk_gas_apply(void *user, JceScriptEntity e, const char * attr_name, int op, float magnitude, float duration_seconds)
{
    (void)user;
    jce::diff::trace_add("gas_apply");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_str(attr_name);
    jce::diff::trace_num(static_cast<double>(op));
    jce::diff::trace_num(static_cast<double>(magnitude));
    jce::diff::trace_num(static_cast<double>(duration_seconds));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static bool mk_raycast(void *user, const float origin[3], const float dir[3], float max_dist, JceScriptRaycastHit *out)
{
    (void)user;
    jce::diff::trace_add("raycast");
    jce::diff::trace_num(origin[0]);
    jce::diff::trace_num(origin[1]);
    jce::diff::trace_num(origin[2]);
    jce::diff::trace_num(dir[0]);
    jce::diff::trace_num(dir[1]);
    jce::diff::trace_num(dir[2]);
    jce::diff::trace_num(static_cast<double>(max_dist));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    out->entity = static_cast<JceScriptEntity>(584);
    out->point[0] = 394.5f;
    out->point[1] = 395.5f;
    out->point[2] = 396.5f;
    out->normal[0] = 397.5f;
    out->normal[1] = 398.5f;
    out->normal[2] = 399.5f;
    out->distance = 400.5f;
    return true;
}

static bool mk_raycast_filtered(void *user, const float origin[3], const float dir[3], float max_dist, uint32_t layer_mask, bool hit_triggers, JceScriptRaycastHit *out)
{
    (void)user;
    jce::diff::trace_add("raycast_filtered");
    jce::diff::trace_num(origin[0]);
    jce::diff::trace_num(origin[1]);
    jce::diff::trace_num(origin[2]);
    jce::diff::trace_num(dir[0]);
    jce::diff::trace_num(dir[1]);
    jce::diff::trace_num(dir[2]);
    jce::diff::trace_num(static_cast<double>(max_dist));
    jce::diff::trace_num(static_cast<double>(layer_mask));
    jce::diff::trace_num(hit_triggers ? 1 : 0);
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    out->entity = static_cast<JceScriptEntity>(600);
    out->point[0] = 410.5f;
    out->point[1] = 411.5f;
    out->point[2] = 412.5f;
    out->normal[0] = 413.5f;
    out->normal[1] = 414.5f;
    out->normal[2] = 415.5f;
    out->distance = 416.5f;
    return true;
}

static int mk_raycast_all(void *user, const float origin[3], const float dir[3], float max_dist, uint32_t layer_mask, bool hit_triggers, JceScriptEntity *out, int max)
{
    (void)user;
    jce::diff::trace_add("raycast_all");
    jce::diff::trace_num(origin[0]);
    jce::diff::trace_num(origin[1]);
    jce::diff::trace_num(origin[2]);
    jce::diff::trace_num(dir[0]);
    jce::diff::trace_num(dir[1]);
    jce::diff::trace_num(dir[2]);
    jce::diff::trace_num(static_cast<double>(max_dist));
    jce::diff::trace_num(static_cast<double>(layer_mask));
    jce::diff::trace_num(hit_triggers ? 1 : 0);
    jce::diff::trace_num(static_cast<double>(max));
    jce::diff::trace_end();
    if (jce::diff::g_mode == jce::diff::MOCK_FAIL)
        return 0;
    if (jce::diff::g_mode == jce::diff::MOCK_NEGATIVE)
        return -3;
    for (int i = 0; i < 3; ++i)
        out[i] = static_cast<JceScriptEntity>(126 + i);
    return 3;
}

static void mk_apply_impulse(void *user, JceScriptEntity e, float x, float y, float z)
{
    (void)user;
    jce::diff::trace_add("apply_impulse");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_num(static_cast<double>(x));
    jce::diff::trace_num(static_cast<double>(y));
    jce::diff::trace_num(static_cast<double>(z));
    jce::diff::trace_end();
}

static void mk_set_velocity(void *user, JceScriptEntity e, float x, float y, float z)
{
    (void)user;
    jce::diff::trace_add("set_velocity");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_num(static_cast<double>(x));
    jce::diff::trace_num(static_cast<double>(y));
    jce::diff::trace_num(static_cast<double>(z));
    jce::diff::trace_end();
}

static void mk_anim_set_float(void *user, JceScriptEntity e, const char * name, float v)
{
    (void)user;
    jce::diff::trace_add("anim_set_float");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_str(name);
    jce::diff::trace_num(static_cast<double>(v));
    jce::diff::trace_end();
}

static void mk_anim_set_int(void *user, JceScriptEntity e, const char * name, int v)
{
    (void)user;
    jce::diff::trace_add("anim_set_int");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_str(name);
    jce::diff::trace_num(static_cast<double>(v));
    jce::diff::trace_end();
}

static void mk_anim_set_bool(void *user, JceScriptEntity e, const char * name, bool v)
{
    (void)user;
    jce::diff::trace_add("anim_set_bool");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_str(name);
    jce::diff::trace_num(v ? 1 : 0);
    jce::diff::trace_end();
}

static void mk_anim_set_trigger(void *user, JceScriptEntity e, const char * name)
{
    (void)user;
    jce::diff::trace_add("anim_set_trigger");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_str(name);
    jce::diff::trace_end();
}

static bool mk_action_down(void *user, const char * name)
{
    (void)user;
    jce::diff::trace_add("action_down");
    jce::diff::trace_str(name);
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static bool mk_action_pressed(void *user, const char * name)
{
    (void)user;
    jce::diff::trace_add("action_pressed");
    jce::diff::trace_str(name);
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static float mk_action_axis(void *user, const char * name)
{
    (void)user;
    jce::diff::trace_add("action_axis");
    jce::diff::trace_str(name);
    jce::diff::trace_end();
    return 44.5f;
}

static void mk_pointer_delta(void *user, float out_xy[2])
{
    (void)user;
    jce::diff::trace_add("pointer_delta");
    jce::diff::trace_end();
    out_xy[0] = 585.5f;
    out_xy[1] = 586.5f;
}

static float mk_pointer_wheel(void *user)
{
    (void)user;
    jce::diff::trace_add("pointer_wheel");
    jce::diff::trace_end();
    return 46.5f;
}

static bool mk_pointer_button(void *user, int button)
{
    (void)user;
    jce::diff::trace_add("pointer_button");
    jce::diff::trace_num(static_cast<double>(button));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static int mk_touch_count(void *user)
{
    (void)user;
    jce::diff::trace_add("touch_count");
    jce::diff::trace_end();
    if (jce::diff::g_mode == jce::diff::MOCK_FAIL)
        return 0;
    if (jce::diff::g_mode == jce::diff::MOCK_NEGATIVE)
        return -3;
    return 8;
}

static bool mk_touch_get(void *user, int index, uint64_t *id, float *x, float *y, float *pressure)
{
    (void)user;
    jce::diff::trace_add("touch_get");
    jce::diff::trace_num(static_cast<double>(index));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    *id = static_cast<uint64_t>(940);
    *x = 650.5f;
    *y = 651.5f;
    *pressure = 652.5f;
    return true;
}

static const char * mk_loc_translate(void *user, const char * key)
{
    (void)user;
    jce::diff::trace_add("loc_translate");
    jce::diff::trace_str(key);
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return NULL;   /* a host that answers NULL, not an absent host */
    return "loc_translate_value";
}

static const char * mk_loc_get_locale(void *user)
{
    (void)user;
    jce::diff::trace_add("loc_get_locale");
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return NULL;   /* a host that answers NULL, not an absent host */
    return "loc_get_locale_value";
}

static void mk_loc_set_locale(void *user, const char * locale)
{
    (void)user;
    jce::diff::trace_add("loc_set_locale");
    jce::diff::trace_str(locale);
    jce::diff::trace_end();
}

static bool mk_get_velocity(void *user, JceScriptEntity e, float out[3])
{
    (void)user;
    jce::diff::trace_add("get_velocity");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    out[0] = 713.5f;
    out[1] = 714.5f;
    out[2] = 715.5f;
    return true;
}

static void mk_vehicle_set_input(void *user, JceScriptEntity e, float throttle, float brake, float steer)
{
    (void)user;
    jce::diff::trace_add("vehicle_set_input");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_num(static_cast<double>(throttle));
    jce::diff::trace_num(static_cast<double>(brake));
    jce::diff::trace_num(static_cast<double>(steer));
    jce::diff::trace_end();
}

static float mk_vehicle_get_speed(void *user, JceScriptEntity e)
{
    (void)user;
    jce::diff::trace_add("vehicle_get_speed");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_end();
    return 55.5f;
}

static void mk_get_move(void *user, float out[3])
{
    (void)user;
    jce::diff::trace_add("get_move");
    jce::diff::trace_end();
    out[0] = 761.5f;
    out[1] = 762.5f;
    out[2] = 763.5f;
}

static bool mk_ui_get_slider(void *user, JceScriptEntity e, float *out)
{
    (void)user;
    jce::diff::trace_add("ui_get_slider");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    *out = 777.5f;
    return true;
}

static void mk_ui_set_slider(void *user, JceScriptEntity e, float v)
{
    (void)user;
    jce::diff::trace_add("ui_set_slider");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_num(static_cast<double>(v));
    jce::diff::trace_end();
}

static bool mk_ui_get_progress(void *user, JceScriptEntity e, float *out)
{
    (void)user;
    jce::diff::trace_add("ui_get_progress");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    *out = 809.5f;
    return true;
}

static void mk_ui_set_progress(void *user, JceScriptEntity e, float v)
{
    (void)user;
    jce::diff::trace_add("ui_set_progress");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_num(static_cast<double>(v));
    jce::diff::trace_end();
}

static bool mk_ui_get_toggle(void *user, JceScriptEntity e, bool *out)
{
    (void)user;
    jce::diff::trace_add("ui_get_toggle");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    *out = true;
    return true;
}

static void mk_ui_set_toggle(void *user, JceScriptEntity e, bool v)
{
    (void)user;
    jce::diff::trace_add("ui_set_toggle");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_num(v ? 1 : 0);
    jce::diff::trace_end();
}

static void mk_ui_set_text(void *user, JceScriptEntity e, const char * txt)
{
    (void)user;
    jce::diff::trace_add("ui_set_text");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_str(txt);
    jce::diff::trace_end();
}

static void mk_send_message(void *user, JceScriptEntity target, const char * msg, double number_arg, const char * str_arg)
{
    (void)user;
    jce::diff::trace_add("send_message");
    jce::diff::trace_num(static_cast<double>(target));
    jce::diff::trace_str(msg);
    jce::diff::trace_num(static_cast<double>(number_arg));
    jce::diff::trace_str(str_arg);
    jce::diff::trace_end();
}

static void mk_broadcast(void *user, const char * msg, double number_arg, const char * str_arg)
{
    (void)user;
    jce::diff::trace_add("broadcast");
    jce::diff::trace_str(msg);
    jce::diff::trace_num(static_cast<double>(number_arg));
    jce::diff::trace_str(str_arg);
    jce::diff::trace_end();
}

static bool mk_has_component(void *user, JceScriptEntity e, const char * comp_name)
{
    (void)user;
    jce::diff::trace_add("has_component");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_str(comp_name);
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static bool mk_is_component_enabled(void *user, JceScriptEntity e, const char * comp_name)
{
    (void)user;
    jce::diff::trace_add("is_component_enabled");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_str(comp_name);
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static void mk_set_component_enabled(void *user, JceScriptEntity e, const char * comp_name, bool on)
{
    (void)user;
    jce::diff::trace_add("set_component_enabled");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_str(comp_name);
    jce::diff::trace_num(on ? 1 : 0);
    jce::diff::trace_end();
}

static bool mk_net_is_server(void *user)
{
    (void)user;
    jce::diff::trace_add("net_is_server");
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static bool mk_net_is_client(void *user)
{
    (void)user;
    jce::diff::trace_add("net_is_client");
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static JceScriptEntity mk_net_spawn(void *user, const char * prefab_path, float x, float y, float z)
{
    (void)user;
    jce::diff::trace_add("net_spawn");
    jce::diff::trace_str(prefab_path);
    jce::diff::trace_num(static_cast<double>(x));
    jce::diff::trace_num(static_cast<double>(y));
    jce::diff::trace_num(static_cast<double>(z));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return 0;
    return static_cast<JceScriptEntity>(462);
}

static bool mk_rpc_send(void *user, JceScriptEntity e, const char * event, int target, const char * payload)
{
    (void)user;
    jce::diff::trace_add("rpc_send");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_str(event);
    jce::diff::trace_num(static_cast<double>(target));
    jce::diff::trace_str(payload);
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static void mk_particle_burst(void *user, JceScriptEntity e, int count)
{
    (void)user;
    jce::diff::trace_add("particle_burst");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_num(static_cast<double>(count));
    jce::diff::trace_end();
}

static void mk_particle_set_emitting(void *user, JceScriptEntity e, bool on)
{
    (void)user;
    jce::diff::trace_add("particle_set_emitting");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_num(on ? 1 : 0);
    jce::diff::trace_end();
}

static void mk_particle_set_color(void *user, JceScriptEntity e, float r, float g, float b)
{
    (void)user;
    jce::diff::trace_add("particle_set_color");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_num(static_cast<double>(r));
    jce::diff::trace_num(static_cast<double>(g));
    jce::diff::trace_num(static_cast<double>(b));
    jce::diff::trace_end();
}

static int mk_find_by_name(void *user, const char * name, JceScriptEntity *out, int max)
{
    (void)user;
    jce::diff::trace_add("find_by_name");
    jce::diff::trace_str(name);
    jce::diff::trace_num(static_cast<double>(max));
    jce::diff::trace_end();
    if (jce::diff::g_mode == jce::diff::MOCK_FAIL)
        return 0;
    if (jce::diff::g_mode == jce::diff::MOCK_NEGATIVE)
        return -3;
    for (int i = 0; i < 2; ++i)
        out[i] = static_cast<JceScriptEntity>(167 + i);
    return 2;
}

static int mk_find_by_prefix(void *user, const char * prefix, JceScriptEntity *out, int max)
{
    (void)user;
    jce::diff::trace_add("find_by_prefix");
    jce::diff::trace_str(prefix);
    jce::diff::trace_num(static_cast<double>(max));
    jce::diff::trace_end();
    if (jce::diff::g_mode == jce::diff::MOCK_FAIL)
        return 0;
    if (jce::diff::g_mode == jce::diff::MOCK_NEGATIVE)
        return -3;
    for (int i = 0; i < 3; ++i)
        out[i] = static_cast<JceScriptEntity>(168 + i);
    return 3;
}

static char * mk_comp_get_json(void *user, JceScriptEntity e, const char * type)
{
    (void)user;
    jce::diff::trace_add("comp_get_json");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_str(type);
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return NULL;
    if (jce::diff::g_mode == jce::diff::MOCK_LONG)
        return jce::diff::dup_long_json();
    return jce::diff::dup("{\"comp_get_json\":69}");
}

static bool mk_comp_set_json(void *user, JceScriptEntity e, const char * type, const char * json)
{
    (void)user;
    jce::diff::trace_add("comp_set_json");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_str(type);
    jce::diff::trace_str(json);
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static char * mk_render_get_json(void *user)
{
    (void)user;
    jce::diff::trace_add("render_get_json");
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return NULL;
    if (jce::diff::g_mode == jce::diff::MOCK_LONG)
        return jce::diff::dup_long_json();
    return jce::diff::dup("{\"render_get_json\":71}");
}

static bool mk_render_set_json(void *user, const char * json)
{
    (void)user;
    jce::diff::trace_add("render_set_json");
    jce::diff::trace_str(json);
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static void mk_audio_set_volume(void *user, JceScriptEntity e, float volume)
{
    (void)user;
    jce::diff::trace_add("audio_set_volume");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_num(static_cast<double>(volume));
    jce::diff::trace_end();
}

static bool mk_ui_get_dropdown(void *user, JceScriptEntity e, int *out)
{
    (void)user;
    jce::diff::trace_add("ui_get_dropdown");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    *out = static_cast<int>(1484);
    return true;
}

static void mk_ui_set_dropdown(void *user, JceScriptEntity e, int index)
{
    (void)user;
    jce::diff::trace_add("ui_set_dropdown");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_num(static_cast<double>(index));
    jce::diff::trace_end();
}

static const char * mk_ui_get_input_text(void *user, JceScriptEntity e)
{
    (void)user;
    jce::diff::trace_add("ui_get_input_text");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return NULL;   /* a host that answers NULL, not an absent host */
    return "ui_get_input_text_value";
}

static void mk_ui_set_input_text(void *user, JceScriptEntity e, const char * text)
{
    (void)user;
    jce::diff::trace_add("ui_set_input_text");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_str(text);
    jce::diff::trace_end();
}

static bool mk_ui_get_scroll(void *user, JceScriptEntity e, float out_xy[2])
{
    (void)user;
    jce::diff::trace_add("ui_get_scroll");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    out_xy[0] = 1257.5f;
    out_xy[1] = 1258.5f;
    return true;
}

static void mk_ui_set_scroll(void *user, JceScriptEntity e, float x, float y)
{
    (void)user;
    jce::diff::trace_add("ui_set_scroll");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_num(static_cast<double>(x));
    jce::diff::trace_num(static_cast<double>(y));
    jce::diff::trace_end();
}

static float mk_world_get_hour(void *user)
{
    (void)user;
    jce::diff::trace_add("world_get_hour");
    jce::diff::trace_end();
    return 89.5f;
}

static void mk_world_set_hour(void *user, float hour)
{
    (void)user;
    jce::diff::trace_add("world_set_hour");
    jce::diff::trace_num(static_cast<double>(hour));
    jce::diff::trace_end();
}

static bool mk_world_is_daytime(void *user)
{
    (void)user;
    jce::diff::trace_add("world_is_daytime");
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static int mk_world_get_weather(void *user)
{
    (void)user;
    jce::diff::trace_add("world_get_weather");
    jce::diff::trace_end();
    if (jce::diff::g_mode == jce::diff::MOCK_FAIL)
        return 0;
    if (jce::diff::g_mode == jce::diff::MOCK_NEGATIVE)
        return -3;
    return 7;
}

static float mk_world_get_weather_intensity(void *user)
{
    (void)user;
    jce::diff::trace_add("world_get_weather_intensity");
    jce::diff::trace_end();
    return 93.5f;
}

static float mk_world_get_wind_speed(void *user)
{
    (void)user;
    jce::diff::trace_add("world_get_wind_speed");
    jce::diff::trace_end();
    return 94.5f;
}

static bool mk_request_scene(void *user, const char * scene_path)
{
    (void)user;
    jce::diff::trace_add("request_scene");
    jce::diff::trace_str(scene_path);
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static bool mk_is_transitioning(void *user)
{
    (void)user;
    jce::diff::trace_add("is_transitioning");
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static bool mk_audio_play(void *user, JceScriptEntity e)
{
    (void)user;
    jce::diff::trace_add("audio_play");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static bool mk_audio_stop(void *user, JceScriptEntity e)
{
    (void)user;
    jce::diff::trace_add("audio_stop");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static bool mk_audio_is_playing(void *user, JceScriptEntity e)
{
    (void)user;
    jce::diff::trace_add("audio_is_playing");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static bool mk_save_game(void *user, const char * path)
{
    (void)user;
    jce::diff::trace_add("save_game");
    jce::diff::trace_str(path);
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static bool mk_load_game(void *user, const char * path)
{
    (void)user;
    jce::diff::trace_add("load_game");
    jce::diff::trace_str(path);
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    return true;
}

static int mk_overlap_sphere(void *user, float x, float y, float z, float radius, uint32_t layer_mask, JceScriptEntity *out, int max)
{
    (void)user;
    jce::diff::trace_add("overlap_sphere");
    jce::diff::trace_num(static_cast<double>(x));
    jce::diff::trace_num(static_cast<double>(y));
    jce::diff::trace_num(static_cast<double>(z));
    jce::diff::trace_num(static_cast<double>(radius));
    jce::diff::trace_num(static_cast<double>(layer_mask));
    jce::diff::trace_num(static_cast<double>(max));
    jce::diff::trace_end();
    if (jce::diff::g_mode == jce::diff::MOCK_FAIL)
        return 0;
    if (jce::diff::g_mode == jce::diff::MOCK_NEGATIVE)
        return -3;
    for (int i = 0; i < 3; ++i)
        out[i] = static_cast<JceScriptEntity>(193 + i);
    return 3;
}

static int mk_overlap_box(void *user, float x, float y, float z, float hx, float hy, float hz, uint32_t layer_mask, JceScriptEntity *out, int max)
{
    (void)user;
    jce::diff::trace_add("overlap_box");
    jce::diff::trace_num(static_cast<double>(x));
    jce::diff::trace_num(static_cast<double>(y));
    jce::diff::trace_num(static_cast<double>(z));
    jce::diff::trace_num(static_cast<double>(hx));
    jce::diff::trace_num(static_cast<double>(hy));
    jce::diff::trace_num(static_cast<double>(hz));
    jce::diff::trace_num(static_cast<double>(layer_mask));
    jce::diff::trace_num(static_cast<double>(max));
    jce::diff::trace_end();
    if (jce::diff::g_mode == jce::diff::MOCK_FAIL)
        return 0;
    if (jce::diff::g_mode == jce::diff::MOCK_NEGATIVE)
        return -3;
    for (int i = 0; i < 3; ++i)
        out[i] = static_cast<JceScriptEntity>(194 + i);
    return 3;
}

static bool mk_get_script_param(void *user, JceScriptEntity e, const char * name, int *out_kind, double *out_number, JceScriptEntity *out_entity)
{
    (void)user;
    jce::diff::trace_add("get_script_param");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_str(name);
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    *out_kind = static_cast<int>(1820);
    *out_number = 1530.5f;
    *out_entity = static_cast<JceScriptEntity>(1822);
    return true;
}

static const char * mk_get_script_param_text(void *user, JceScriptEntity e, const char * name)
{
    (void)user;
    jce::diff::trace_add("get_script_param_text");
    jce::diff::trace_num(static_cast<double>(e));
    jce::diff::trace_str(name);
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return NULL;   /* a host that answers NULL, not an absent host */
    return "get_script_param_text_value";
}

static bool mk_curve_eval(void *user, const char * path, const char * channel, double t, double *out_value)
{
    (void)user;
    jce::diff::trace_add("curve_eval");
    jce::diff::trace_str(path);
    jce::diff::trace_str(channel);
    jce::diff::trace_num(static_cast<double>(t));
    jce::diff::trace_end();
    if (!(jce::diff::g_mode != jce::diff::MOCK_FAIL))
        return false;
    *out_value = 1561.5f;
    return true;
}

static int mk_vcam_activate(void *user, const char * name)
{
    (void)user;
    jce::diff::trace_add("vcam_activate");
    jce::diff::trace_str(name);
    jce::diff::trace_end();
    if (jce::diff::g_mode == jce::diff::MOCK_FAIL)
        return 0;
    if (jce::diff::g_mode == jce::diff::MOCK_NEGATIVE)
        return -3;
    return 7;
}

/* the release member of the owned-string shape.  Traced, because
 * 'the string was released' is a fact about the CALL SEQUENCE and
 * nothing in the result can show it. */
static void mk_json_free(void *user, char *s)
{
    (void)user;
    jce::diff::trace_add("json_free");
    jce::diff::trace_str(s);
    jce::diff::trace_end();
    jce::diff::release(s);
}

}  /* extern "C" */

/* Every member any manifest `expose` entry reaches, and nothing else.
 * The members only the seven hand-written entries reach stay NULL: this
 * wrapper does not expose them, so there is nothing to compare. */
void jce::diff::install_recording_host(JceScriptHost *h)
{
    std::memset(h, 0, sizeof *h);
    h->user = &jce::diff::capture();
    h->log = mk_log;
    h->get_position = mk_get_position;
    h->set_position = mk_set_position;
    h->get_rotation = mk_get_rotation;
    h->set_rotation = mk_set_rotation;
    h->get_scale = mk_get_scale;
    h->get_world_position = mk_get_world_position;
    h->set_scale = mk_set_scale;
    h->set_parent = mk_set_parent;
    h->get_parent = mk_get_parent;
    h->is_key_down = mk_is_key_down;
    h->find_with_tag = mk_find_with_tag;
    h->destroy_entity = mk_destroy_entity;
    h->spawn = mk_spawn;
    h->move_axis = mk_move_axis;
    h->input_button = mk_input_button;
    h->set_time_scale = mk_set_time_scale;
    h->set_paused = mk_set_paused;
    h->shake_camera = mk_shake_camera;
    h->music_set_intensity = mk_music_set_intensity;
    h->music_get_intensity = mk_music_get_intensity;
    h->music_request_transition = mk_music_request_transition;
    h->gas_activate = mk_gas_activate;
    h->gas_get = mk_gas_get;
    h->gas_apply = mk_gas_apply;
    h->raycast = mk_raycast;
    h->raycast_filtered = mk_raycast_filtered;
    h->raycast_all = mk_raycast_all;
    h->apply_impulse = mk_apply_impulse;
    h->set_velocity = mk_set_velocity;
    h->anim_set_float = mk_anim_set_float;
    h->anim_set_int = mk_anim_set_int;
    h->anim_set_bool = mk_anim_set_bool;
    h->anim_set_trigger = mk_anim_set_trigger;
    h->action_down = mk_action_down;
    h->action_pressed = mk_action_pressed;
    h->action_axis = mk_action_axis;
    h->pointer_delta = mk_pointer_delta;
    h->pointer_wheel = mk_pointer_wheel;
    h->pointer_button = mk_pointer_button;
    h->touch_count = mk_touch_count;
    h->touch_get = mk_touch_get;
    h->loc_translate = mk_loc_translate;
    h->loc_get_locale = mk_loc_get_locale;
    h->loc_set_locale = mk_loc_set_locale;
    h->get_velocity = mk_get_velocity;
    h->vehicle_set_input = mk_vehicle_set_input;
    h->vehicle_get_speed = mk_vehicle_get_speed;
    h->get_move = mk_get_move;
    h->ui_get_slider = mk_ui_get_slider;
    h->ui_set_slider = mk_ui_set_slider;
    h->ui_get_progress = mk_ui_get_progress;
    h->ui_set_progress = mk_ui_set_progress;
    h->ui_get_toggle = mk_ui_get_toggle;
    h->ui_set_toggle = mk_ui_set_toggle;
    h->ui_set_text = mk_ui_set_text;
    h->send_message = mk_send_message;
    h->broadcast = mk_broadcast;
    h->has_component = mk_has_component;
    h->is_component_enabled = mk_is_component_enabled;
    h->set_component_enabled = mk_set_component_enabled;
    h->net_is_server = mk_net_is_server;
    h->net_is_client = mk_net_is_client;
    h->net_spawn = mk_net_spawn;
    h->rpc_send = mk_rpc_send;
    h->particle_burst = mk_particle_burst;
    h->particle_set_emitting = mk_particle_set_emitting;
    h->particle_set_color = mk_particle_set_color;
    h->find_by_name = mk_find_by_name;
    h->find_by_prefix = mk_find_by_prefix;
    h->comp_get_json = mk_comp_get_json;
    h->comp_set_json = mk_comp_set_json;
    h->render_get_json = mk_render_get_json;
    h->render_set_json = mk_render_set_json;
    h->audio_set_volume = mk_audio_set_volume;
    h->ui_get_dropdown = mk_ui_get_dropdown;
    h->ui_set_dropdown = mk_ui_set_dropdown;
    h->ui_get_input_text = mk_ui_get_input_text;
    h->ui_set_input_text = mk_ui_set_input_text;
    h->ui_get_scroll = mk_ui_get_scroll;
    h->ui_set_scroll = mk_ui_set_scroll;
    h->world_get_hour = mk_world_get_hour;
    h->world_set_hour = mk_world_set_hour;
    h->world_is_daytime = mk_world_is_daytime;
    h->world_get_weather = mk_world_get_weather;
    h->world_get_weather_intensity = mk_world_get_weather_intensity;
    h->world_get_wind_speed = mk_world_get_wind_speed;
    h->request_scene = mk_request_scene;
    h->is_transitioning = mk_is_transitioning;
    h->audio_play = mk_audio_play;
    h->audio_stop = mk_audio_stop;
    h->audio_is_playing = mk_audio_is_playing;
    h->save_game = mk_save_game;
    h->load_game = mk_load_game;
    h->overlap_sphere = mk_overlap_sphere;
    h->overlap_box = mk_overlap_box;
    h->get_script_param = mk_get_script_param;
    h->get_script_param_text = mk_get_script_param_text;
    h->curve_eval = mk_curve_eval;
    h->vcam_activate = mk_vcam_activate;
    h->json_free = mk_json_free;
}

/* `log` and nothing else: the state a host built against an older
 * header leaves the tail of the table in, and the state every member
 * of a host that simply does not implement a subsystem is in.  `log`
 * survives because it is how the Lua side reports its answer; it is
 * not part of the surface under comparison. */
void jce::diff::install_log_only_host(JceScriptHost *h)
{
    std::memset(h, 0, sizeof *h);
    h->user = &jce::diff::capture();
    h->log = mk_log;
}

/* ------------------------------------------------------------------ *
 *  One function per case: the C++ call, and the projection of its
 *  result onto the comparison form.  The projection is a call into a
 *  hand-written, type-keyed overload -- never a rendering emitted
 *  here.
 * ------------------------------------------------------------------ */
static void cpp_case_0(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.get_position(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_1(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.get_position(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_2(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.set_position(static_cast<Entity>(41), 1.5f, 2.5f, 3.5f);
}

static void cpp_case_3(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.set_position(static_cast<Entity>(41), 1.5f, 2.5f, 3.5f);
}

static void cpp_case_4(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.get_rotation(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_5(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.get_rotation(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_6(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.set_rotation(static_cast<Entity>(41), 1.5f, 2.5f, 3.5f);
}

static void cpp_case_7(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.set_rotation(static_cast<Entity>(41), 1.5f, 2.5f, 3.5f);
}

static void cpp_case_8(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.get_scale(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_9(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.get_scale(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_10(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.get_world_position(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_11(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.get_world_position(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_12(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.set_scale(static_cast<Entity>(41), 1.5f, 2.5f, 3.5f);
}

static void cpp_case_13(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.set_scale(static_cast<Entity>(41), 1.5f, 2.5f, 3.5f);
}

static void cpp_case_14(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.set_parent(static_cast<Entity>(41), static_cast<Entity>(51), true));
}

static void cpp_case_15(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.set_parent(static_cast<Entity>(41), static_cast<Entity>(51), true));
}

static void cpp_case_16(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.get_parent(static_cast<Entity>(41)));
}

static void cpp_case_17(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.get_parent(static_cast<Entity>(41)));
}

static void cpp_case_18(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.is_key_down(3));
}

static void cpp_case_19(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.is_key_down(3));
}

static void cpp_case_20(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.find_with_tag("find_with_tag_tag"));
}

static void cpp_case_21(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.find_with_tag("find_with_tag_tag"));
}

static void cpp_case_22(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.destroy(static_cast<Entity>(41));
}

static void cpp_case_23(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.destroy(static_cast<Entity>(41));
}

static void cpp_case_24(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.spawn("spawn_prefab_path", 1.5f, 2.5f, 3.5f));
}

static void cpp_case_25(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.spawn("spawn_prefab_path", 1.5f, 2.5f, 3.5f));
}

static void cpp_case_26(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.spawn("spawn_prefab_path"));
}

static void cpp_case_27(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.move_axis());
}

static void cpp_case_28(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.move_axis());
}

static void cpp_case_29(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.jump_pressed());
}

static void cpp_case_30(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.jump_pressed());
}

static void cpp_case_31(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.sprint());
}

static void cpp_case_32(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.sprint());
}

static void cpp_case_33(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.attack_pressed());
}

static void cpp_case_34(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.attack_pressed());
}

static void cpp_case_35(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.set_time_scale(0.5f);
}

static void cpp_case_36(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.set_time_scale(0.5f);
}

static void cpp_case_37(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.pause(true);
}

static void cpp_case_38(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.pause(true);
}

static void cpp_case_39(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.pause();
}

static void cpp_case_40(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.shake_camera(0.5f);
}

static void cpp_case_41(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.shake_camera(0.5f);
}

static void cpp_case_42(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.shake_camera();
}

static void cpp_case_43(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.music_set_intensity(0.5f);
}

static void cpp_case_44(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.music_set_intensity(0.5f);
}

static void cpp_case_45(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.music_get_intensity());
}

static void cpp_case_46(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.music_get_intensity());
}

static void cpp_case_47(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.music_request_transition(3));
}

static void cpp_case_48(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.music_request_transition(3));
}

static void cpp_case_49(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.gas_activate(static_cast<Entity>(41), static_cast<std::uint32_t>(8)));
}

static void cpp_case_50(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.gas_activate(static_cast<Entity>(41), static_cast<std::uint32_t>(8)));
}

static void cpp_case_51(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.gas_get(static_cast<Entity>(41), "gas_get_attr_name"), "nil:nil");
}

static void cpp_case_52(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.gas_get(static_cast<Entity>(41), "gas_get_attr_name"), "nil:nil");
}

static void cpp_case_53(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.gas_apply(static_cast<Entity>(41), "gas_apply_attr_name", 5, 3.5f, 4.5f));
}

static void cpp_case_54(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.gas_apply(static_cast<Entity>(41), "gas_apply_attr_name", 5, 3.5f, 4.5f));
}

static void cpp_case_55(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.gas_apply(static_cast<Entity>(41), "gas_apply_attr_name", 5, 3.5f));
}

static void cpp_case_56(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.raycast({0.5f, 1.5f, 2.5f}, {3.5f, 4.5f, 5.5f}, 6.5f), "number:0");
}

static void cpp_case_57(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.raycast({0.5f, 1.5f, 2.5f}, {3.5f, 4.5f, 5.5f}, 6.5f), "number:0");
}

static void cpp_case_58(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.raycast_filtered({0.5f, 1.5f, 2.5f}, {3.5f, 4.5f, 5.5f}, 6.5f, static_cast<std::uint32_t>(14), true), "number:0");
}

static void cpp_case_59(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.raycast_filtered({0.5f, 1.5f, 2.5f}, {3.5f, 4.5f, 5.5f}, 6.5f, static_cast<std::uint32_t>(14), true), "number:0");
}

static void cpp_case_60(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.raycast_filtered({0.5f, 1.5f, 2.5f}, {3.5f, 4.5f, 5.5f}, 6.5f), "number:0");
}

static void cpp_case_61(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.raycast_all({0.5f, 1.5f, 2.5f}, {3.5f, 4.5f, 5.5f}, 6.5f, static_cast<std::uint32_t>(14), true));
}

static void cpp_case_62(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.raycast_all({0.5f, 1.5f, 2.5f}, {3.5f, 4.5f, 5.5f}, 6.5f, static_cast<std::uint32_t>(14), true));
}

static void cpp_case_63(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.raycast_all({0.5f, 1.5f, 2.5f}, {3.5f, 4.5f, 5.5f}, 6.5f));
}

static void cpp_case_64(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.raycast_all({0.5f, 1.5f, 2.5f}, {3.5f, 4.5f, 5.5f}, 6.5f, static_cast<std::uint32_t>(14), true));
}

static void cpp_case_65(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.apply_impulse(static_cast<Entity>(41), 1.5f, 2.5f, 3.5f);
}

static void cpp_case_66(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.apply_impulse(static_cast<Entity>(41), 1.5f, 2.5f, 3.5f);
}

static void cpp_case_67(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.set_velocity(static_cast<Entity>(41), 1.5f, 2.5f, 3.5f);
}

static void cpp_case_68(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.set_velocity(static_cast<Entity>(41), 1.5f, 2.5f, 3.5f);
}

static void cpp_case_69(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.anim_set_float(static_cast<Entity>(41), "anim_set_float_name", 2.5f);
}

static void cpp_case_70(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.anim_set_float(static_cast<Entity>(41), "anim_set_float_name", 2.5f);
}

static void cpp_case_71(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.anim_set_int(static_cast<Entity>(41), "anim_set_int_name", 5);
}

static void cpp_case_72(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.anim_set_int(static_cast<Entity>(41), "anim_set_int_name", 5);
}

static void cpp_case_73(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.anim_set_bool(static_cast<Entity>(41), "anim_set_bool_name", true);
}

static void cpp_case_74(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.anim_set_bool(static_cast<Entity>(41), "anim_set_bool_name", true);
}

static void cpp_case_75(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.anim_set_trigger(static_cast<Entity>(41), "anim_set_trigger_name");
}

static void cpp_case_76(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.anim_set_trigger(static_cast<Entity>(41), "anim_set_trigger_name");
}

static void cpp_case_77(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.is_action_down("is_action_down_name"));
}

static void cpp_case_78(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.is_action_down("is_action_down_name"));
}

static void cpp_case_79(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.is_action_pressed("is_action_pressed_name"));
}

static void cpp_case_80(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.is_action_pressed("is_action_pressed_name"));
}

static void cpp_case_81(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.get_axis("get_axis_name"));
}

static void cpp_case_82(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.get_axis("get_axis_name"));
}

static void cpp_case_83(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.get_pointer_delta());
}

static void cpp_case_84(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.get_pointer_delta());
}

static void cpp_case_85(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.get_pointer_wheel());
}

static void cpp_case_86(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.get_pointer_wheel());
}

static void cpp_case_87(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.is_pointer_down(3));
}

static void cpp_case_88(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.is_pointer_down(3));
}

static void cpp_case_89(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.get_touch_count());
}

static void cpp_case_90(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.get_touch_count());
}

static void cpp_case_91(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.get_touch_count());
}

static void cpp_case_92(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.get_touch(3), "nil:nil");
}

static void cpp_case_93(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.get_touch(3), "nil:nil");
}

static void cpp_case_94(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.get_touch(-1), "nil:nil");
}

static void cpp_case_95(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.tr("tr_key"));
}

static void cpp_case_96(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.tr("tr_key"));
}

static void cpp_case_97(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.get_locale());
}

static void cpp_case_98(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.get_locale());
}

static void cpp_case_99(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.set_locale("set_locale_locale");
}

static void cpp_case_100(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.set_locale("set_locale_locale");
}

static void cpp_case_101(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.get_velocity(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_102(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.get_velocity(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_103(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.vehicle_set_input(static_cast<Entity>(41), 1.5f, 2.5f, 3.5f);
}

static void cpp_case_104(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.vehicle_set_input(static_cast<Entity>(41), 1.5f, 2.5f, 3.5f);
}

static void cpp_case_105(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.vehicle_get_speed(static_cast<Entity>(41)));
}

static void cpp_case_106(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.vehicle_get_speed(static_cast<Entity>(41)));
}

static void cpp_case_107(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.get_move());
}

static void cpp_case_108(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.get_move());
}

static void cpp_case_109(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.ui_get_slider(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_110(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.ui_get_slider(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_111(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.ui_set_slider(static_cast<Entity>(41), 1.5f);
}

static void cpp_case_112(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.ui_set_slider(static_cast<Entity>(41), 1.5f);
}

static void cpp_case_113(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.ui_get_progress(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_114(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.ui_get_progress(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_115(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.ui_set_progress(static_cast<Entity>(41), 1.5f);
}

static void cpp_case_116(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.ui_set_progress(static_cast<Entity>(41), 1.5f);
}

static void cpp_case_117(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.ui_get_toggle(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_118(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.ui_get_toggle(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_119(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.ui_set_toggle(static_cast<Entity>(41), true);
}

static void cpp_case_120(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.ui_set_toggle(static_cast<Entity>(41), true);
}

static void cpp_case_121(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.ui_set_text(static_cast<Entity>(41), "ui_set_text_txt");
}

static void cpp_case_122(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.ui_set_text(static_cast<Entity>(41), "ui_set_text_txt");
}

static void cpp_case_123(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.send_message(static_cast<Entity>(41), "send_message_msg", 2.5, "send_message_str_arg");
}

static void cpp_case_124(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.send_message(static_cast<Entity>(41), "send_message_msg", 2.5, "send_message_str_arg");
}

static void cpp_case_125(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.send_message(static_cast<Entity>(41), "send_message_msg");
}

static void cpp_case_126(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.broadcast("broadcast_msg", 1.5, "broadcast_str_arg");
}

static void cpp_case_127(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.broadcast("broadcast_msg", 1.5, "broadcast_str_arg");
}

static void cpp_case_128(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.broadcast("broadcast_msg");
}

static void cpp_case_129(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.has_component(static_cast<Entity>(41), "has_component_comp_name"));
}

static void cpp_case_130(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.has_component(static_cast<Entity>(41), "has_component_comp_name"));
}

static void cpp_case_131(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.is_component_enabled(static_cast<Entity>(41), "is_component_enabled_comp_name"));
}

static void cpp_case_132(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.is_component_enabled(static_cast<Entity>(41), "is_component_enabled_comp_name"));
}

static void cpp_case_133(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.set_component_enabled(static_cast<Entity>(41), "set_component_enabled_comp_name", true);
}

static void cpp_case_134(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.set_component_enabled(static_cast<Entity>(41), "set_component_enabled_comp_name", true);
}

static void cpp_case_135(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.net_is_server());
}

static void cpp_case_136(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.net_is_server());
}

static void cpp_case_137(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.net_is_client());
}

static void cpp_case_138(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.net_is_client());
}

static void cpp_case_139(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.net_spawn("net_spawn_prefab_path", 1.5f, 2.5f, 3.5f));
}

static void cpp_case_140(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.net_spawn("net_spawn_prefab_path", 1.5f, 2.5f, 3.5f));
}

static void cpp_case_141(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.rpc_send(static_cast<Entity>(41), "rpc_send_event", 5, "rpc_send_payload"));
}

static void cpp_case_142(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.rpc_send(static_cast<Entity>(41), "rpc_send_event", 5, "rpc_send_payload"));
}

static void cpp_case_143(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.rpc_send(static_cast<Entity>(41), "rpc_send_event"));
}

static void cpp_case_144(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.particle_burst(static_cast<Entity>(41), 4);
}

static void cpp_case_145(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.particle_burst(static_cast<Entity>(41), 4);
}

static void cpp_case_146(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.particle_set_emitting(static_cast<Entity>(41), true);
}

static void cpp_case_147(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.particle_set_emitting(static_cast<Entity>(41), true);
}

static void cpp_case_148(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.particle_set_color(static_cast<Entity>(41), 1.5f, 2.5f, 3.5f);
}

static void cpp_case_149(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.particle_set_color(static_cast<Entity>(41), 1.5f, 2.5f, 3.5f);
}

static void cpp_case_150(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.find_by_name("find_by_name_name"));
}

static void cpp_case_151(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.find_by_name("find_by_name_name"));
}

static void cpp_case_152(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.find_by_name("find_by_name_name"));
}

static void cpp_case_153(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.find_by_prefix("find_by_prefix_prefix"));
}

static void cpp_case_154(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.find_by_prefix("find_by_prefix_prefix"));
}

static void cpp_case_155(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.find_by_prefix("find_by_prefix_prefix"));
}

static void cpp_case_156(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.comp_get(static_cast<Entity>(41), "comp_get_type"), "nil:nil");
}

static void cpp_case_157(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.comp_get(static_cast<Entity>(41), "comp_get_type"), "nil:nil");
}

static void cpp_case_158(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.comp_set(static_cast<Entity>(41), "comp_set_type", "comp_set_json"));
}

static void cpp_case_159(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.comp_set(static_cast<Entity>(41), "comp_set_type", "comp_set_json"));
}

static void cpp_case_160(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.render_get(), "nil:nil");
}

static void cpp_case_161(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.render_get(), "nil:nil");
}

static void cpp_case_162(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.render_set("render_set_json"));
}

static void cpp_case_163(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.render_set("render_set_json"));
}

static void cpp_case_164(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.audio_set_volume(static_cast<Entity>(41), 1.5f);
}

static void cpp_case_165(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.audio_set_volume(static_cast<Entity>(41), 1.5f);
}

static void cpp_case_166(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.ui_get_dropdown(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_167(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.ui_get_dropdown(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_168(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.ui_set_dropdown(static_cast<Entity>(41), 4);
}

static void cpp_case_169(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.ui_set_dropdown(static_cast<Entity>(41), 4);
}

static void cpp_case_170(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.ui_get_input_text(static_cast<Entity>(41)));
}

static void cpp_case_171(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.ui_get_input_text(static_cast<Entity>(41)));
}

static void cpp_case_172(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.ui_set_input_text(static_cast<Entity>(41), "ui_set_input_text_text");
}

static void cpp_case_173(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.ui_set_input_text(static_cast<Entity>(41), "ui_set_input_text_text");
}

static void cpp_case_174(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.ui_get_scroll(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_175(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.ui_get_scroll(static_cast<Entity>(41)), "nil:nil");
}

static void cpp_case_176(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.ui_set_scroll(static_cast<Entity>(41), 1.5f, 2.5f);
}

static void cpp_case_177(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.ui_set_scroll(static_cast<Entity>(41), 1.5f, 2.5f);
}

static void cpp_case_178(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.world_get_hour());
}

static void cpp_case_179(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.world_get_hour());
}

static void cpp_case_180(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.world_set_hour(0.5f);
}

static void cpp_case_181(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        api.world_set_hour(0.5f);
}

static void cpp_case_182(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.world_is_daytime());
}

static void cpp_case_183(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.world_is_daytime());
}

static void cpp_case_184(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.world_get_weather());
}

static void cpp_case_185(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.world_get_weather());
}

static void cpp_case_186(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.world_get_weather_intensity());
}

static void cpp_case_187(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.world_get_weather_intensity());
}

static void cpp_case_188(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.world_get_wind_speed());
}

static void cpp_case_189(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.world_get_wind_speed());
}

static void cpp_case_190(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.request_scene("request_scene_scene_path"));
}

static void cpp_case_191(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.request_scene("request_scene_scene_path"));
}

static void cpp_case_192(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.is_transitioning());
}

static void cpp_case_193(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.is_transitioning());
}

static void cpp_case_194(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.audio_play(static_cast<Entity>(41)));
}

static void cpp_case_195(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.audio_play(static_cast<Entity>(41)));
}

static void cpp_case_196(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.audio_stop(static_cast<Entity>(41)));
}

static void cpp_case_197(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.audio_stop(static_cast<Entity>(41)));
}

static void cpp_case_198(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.audio_is_playing(static_cast<Entity>(41)));
}

static void cpp_case_199(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.audio_is_playing(static_cast<Entity>(41)));
}

static void cpp_case_200(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.save_game("save_game_path"));
}

static void cpp_case_201(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.save_game("save_game_path"));
}

static void cpp_case_202(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.load_game("load_game_path"));
}

static void cpp_case_203(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.load_game("load_game_path"));
}

static void cpp_case_204(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.overlap_sphere(0.5f, 1.5f, 2.5f, 3.5f, static_cast<std::uint32_t>(11)));
}

static void cpp_case_205(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.overlap_sphere(0.5f, 1.5f, 2.5f, 3.5f, static_cast<std::uint32_t>(11)));
}

static void cpp_case_206(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.overlap_sphere(0.5f, 1.5f, 2.5f, 3.5f));
}

static void cpp_case_207(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.overlap_sphere(0.5f, 1.5f, 2.5f, 3.5f, static_cast<std::uint32_t>(11)));
}

static void cpp_case_208(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.overlap_box(0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f, static_cast<std::uint32_t>(13)));
}

static void cpp_case_209(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.overlap_box(0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f, static_cast<std::uint32_t>(13)));
}

static void cpp_case_210(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.overlap_box(0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f));
}

static void cpp_case_211(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.overlap_box(0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f, static_cast<std::uint32_t>(13)));
}

static void cpp_case_212(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.get_param(static_cast<Entity>(41), "get_param_name"), "nil:nil");
}

static void cpp_case_213(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.get_param(static_cast<Entity>(41), "get_param_name"), "nil:nil");
}

static void cpp_case_214(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.get_param_text(static_cast<Entity>(41), "get_param_text_name"));
}

static void cpp_case_215(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.get_param_text(static_cast<Entity>(41), "get_param_text_name"));
}

static void cpp_case_216(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.curve_eval("curve_eval_path", "curve_eval_channel", 2.5), "nil:nil");
}

static void cpp_case_217(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record_optional(s, api.curve_eval("curve_eval_path", "curve_eval_channel", 2.5), "nil:nil");
}

static void cpp_case_218(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.vcam_activate("vcam_activate_name"));
}

static void cpp_case_219(Api &api, jce::diff::Slots &s)
{
    (void)api; (void)s;
        jce::diff::record(s, api.vcam_activate("vcam_activate_name"));
}

struct DiffCase {
    const char       *label;
    jce::diff::MockMode mode;
    const char       *lua;
    void            (*run_cpp)(Api &, jce::diff::Slots &);
};

static const DiffCase g_cases[] = {
    { "get_position", jce::diff::MOCK_OK, "jce.get_position(41)", cpp_case_0 },
    { "get_position [host says no]", jce::diff::MOCK_FAIL, "jce.get_position(41)", cpp_case_1 },
    { "set_position", jce::diff::MOCK_OK, "jce.set_position(41, 1.5, 2.5, 3.5)", cpp_case_2 },
    { "set_position [host says no]", jce::diff::MOCK_FAIL, "jce.set_position(41, 1.5, 2.5, 3.5)", cpp_case_3 },
    { "get_rotation", jce::diff::MOCK_OK, "jce.get_rotation(41)", cpp_case_4 },
    { "get_rotation [host says no]", jce::diff::MOCK_FAIL, "jce.get_rotation(41)", cpp_case_5 },
    { "set_rotation", jce::diff::MOCK_OK, "jce.set_rotation(41, 1.5, 2.5, 3.5)", cpp_case_6 },
    { "set_rotation [host says no]", jce::diff::MOCK_FAIL, "jce.set_rotation(41, 1.5, 2.5, 3.5)", cpp_case_7 },
    { "get_scale", jce::diff::MOCK_OK, "jce.get_scale(41)", cpp_case_8 },
    { "get_scale [host says no]", jce::diff::MOCK_FAIL, "jce.get_scale(41)", cpp_case_9 },
    { "get_world_position", jce::diff::MOCK_OK, "jce.get_world_position(41)", cpp_case_10 },
    { "get_world_position [host says no]", jce::diff::MOCK_FAIL, "jce.get_world_position(41)", cpp_case_11 },
    { "set_scale", jce::diff::MOCK_OK, "jce.set_scale(41, 1.5, 2.5, 3.5)", cpp_case_12 },
    { "set_scale [host says no]", jce::diff::MOCK_FAIL, "jce.set_scale(41, 1.5, 2.5, 3.5)", cpp_case_13 },
    { "set_parent", jce::diff::MOCK_OK, "jce.set_parent(41, 51, true)", cpp_case_14 },
    { "set_parent [host says no]", jce::diff::MOCK_FAIL, "jce.set_parent(41, 51, true)", cpp_case_15 },
    { "get_parent", jce::diff::MOCK_OK, "jce.get_parent(41)", cpp_case_16 },
    { "get_parent [host says no]", jce::diff::MOCK_FAIL, "jce.get_parent(41)", cpp_case_17 },
    { "is_key_down", jce::diff::MOCK_OK, "jce.is_key_down(3)", cpp_case_18 },
    { "is_key_down [host says no]", jce::diff::MOCK_FAIL, "jce.is_key_down(3)", cpp_case_19 },
    { "find_with_tag", jce::diff::MOCK_OK, "jce.find_with_tag('find_with_tag_tag')", cpp_case_20 },
    { "find_with_tag [host says no]", jce::diff::MOCK_FAIL, "jce.find_with_tag('find_with_tag_tag')", cpp_case_21 },
    { "destroy", jce::diff::MOCK_OK, "jce.destroy(41)", cpp_case_22 },
    { "destroy [host says no]", jce::diff::MOCK_FAIL, "jce.destroy(41)", cpp_case_23 },
    { "spawn", jce::diff::MOCK_OK, "jce.spawn('spawn_prefab_path', 1.5, 2.5, 3.5)", cpp_case_24 },
    { "spawn [host says no]", jce::diff::MOCK_FAIL, "jce.spawn('spawn_prefab_path', 1.5, 2.5, 3.5)", cpp_case_25 },
    { "spawn [trailing defaults omitted]", jce::diff::MOCK_OK, "jce.spawn('spawn_prefab_path')", cpp_case_26 },
    { "move_axis", jce::diff::MOCK_OK, "jce.move_axis()", cpp_case_27 },
    { "move_axis [host says no]", jce::diff::MOCK_FAIL, "jce.move_axis()", cpp_case_28 },
    { "jump_pressed", jce::diff::MOCK_OK, "jce.jump_pressed()", cpp_case_29 },
    { "jump_pressed [host says no]", jce::diff::MOCK_FAIL, "jce.jump_pressed()", cpp_case_30 },
    { "sprint", jce::diff::MOCK_OK, "jce.sprint()", cpp_case_31 },
    { "sprint [host says no]", jce::diff::MOCK_FAIL, "jce.sprint()", cpp_case_32 },
    { "attack_pressed", jce::diff::MOCK_OK, "jce.attack_pressed()", cpp_case_33 },
    { "attack_pressed [host says no]", jce::diff::MOCK_FAIL, "jce.attack_pressed()", cpp_case_34 },
    { "set_time_scale", jce::diff::MOCK_OK, "jce.set_time_scale(0.5)", cpp_case_35 },
    { "set_time_scale [host says no]", jce::diff::MOCK_FAIL, "jce.set_time_scale(0.5)", cpp_case_36 },
    { "pause", jce::diff::MOCK_OK, "jce.pause(true)", cpp_case_37 },
    { "pause [host says no]", jce::diff::MOCK_FAIL, "jce.pause(true)", cpp_case_38 },
    { "pause [trailing defaults omitted]", jce::diff::MOCK_OK, "jce.pause()", cpp_case_39 },
    { "shake_camera", jce::diff::MOCK_OK, "jce.shake_camera(0.5)", cpp_case_40 },
    { "shake_camera [host says no]", jce::diff::MOCK_FAIL, "jce.shake_camera(0.5)", cpp_case_41 },
    { "shake_camera [trailing defaults omitted]", jce::diff::MOCK_OK, "jce.shake_camera()", cpp_case_42 },
    { "music_set_intensity", jce::diff::MOCK_OK, "jce.music_set_intensity(0.5)", cpp_case_43 },
    { "music_set_intensity [host says no]", jce::diff::MOCK_FAIL, "jce.music_set_intensity(0.5)", cpp_case_44 },
    { "music_get_intensity", jce::diff::MOCK_OK, "jce.music_get_intensity()", cpp_case_45 },
    { "music_get_intensity [host says no]", jce::diff::MOCK_FAIL, "jce.music_get_intensity()", cpp_case_46 },
    { "music_request_transition", jce::diff::MOCK_OK, "jce.music_request_transition(3)", cpp_case_47 },
    { "music_request_transition [host says no]", jce::diff::MOCK_FAIL, "jce.music_request_transition(3)", cpp_case_48 },
    { "gas_activate", jce::diff::MOCK_OK, "jce.gas_activate(41, 8)", cpp_case_49 },
    { "gas_activate [host says no]", jce::diff::MOCK_FAIL, "jce.gas_activate(41, 8)", cpp_case_50 },
    { "gas_get", jce::diff::MOCK_OK, "jce.gas_get(41, 'gas_get_attr_name')", cpp_case_51 },
    { "gas_get [host says no]", jce::diff::MOCK_FAIL, "jce.gas_get(41, 'gas_get_attr_name')", cpp_case_52 },
    { "gas_apply", jce::diff::MOCK_OK, "jce.gas_apply(41, 'gas_apply_attr_name', 5, 3.5, 4.5)", cpp_case_53 },
    { "gas_apply [host says no]", jce::diff::MOCK_FAIL, "jce.gas_apply(41, 'gas_apply_attr_name', 5, 3.5, 4.5)", cpp_case_54 },
    { "gas_apply [trailing defaults omitted]", jce::diff::MOCK_OK, "jce.gas_apply(41, 'gas_apply_attr_name', 5, 3.5)", cpp_case_55 },
    { "raycast", jce::diff::MOCK_OK, "jce.raycast(0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5)", cpp_case_56 },
    { "raycast [host says no]", jce::diff::MOCK_FAIL, "jce.raycast(0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5)", cpp_case_57 },
    { "raycast_filtered", jce::diff::MOCK_OK, "jce.raycast_filtered(0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 14, true)", cpp_case_58 },
    { "raycast_filtered [host says no]", jce::diff::MOCK_FAIL, "jce.raycast_filtered(0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 14, true)", cpp_case_59 },
    { "raycast_filtered [trailing defaults omitted]", jce::diff::MOCK_OK, "jce.raycast_filtered(0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5)", cpp_case_60 },
    { "raycast_all", jce::diff::MOCK_OK, "jce.raycast_all(0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 14, true)", cpp_case_61 },
    { "raycast_all [host says no]", jce::diff::MOCK_FAIL, "jce.raycast_all(0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 14, true)", cpp_case_62 },
    { "raycast_all [trailing defaults omitted]", jce::diff::MOCK_OK, "jce.raycast_all(0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5)", cpp_case_63 },
    { "raycast_all [host returns a negative count]", jce::diff::MOCK_NEGATIVE, "jce.raycast_all(0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 14, true)", cpp_case_64 },
    { "apply_impulse", jce::diff::MOCK_OK, "jce.apply_impulse(41, 1.5, 2.5, 3.5)", cpp_case_65 },
    { "apply_impulse [host says no]", jce::diff::MOCK_FAIL, "jce.apply_impulse(41, 1.5, 2.5, 3.5)", cpp_case_66 },
    { "set_velocity", jce::diff::MOCK_OK, "jce.set_velocity(41, 1.5, 2.5, 3.5)", cpp_case_67 },
    { "set_velocity [host says no]", jce::diff::MOCK_FAIL, "jce.set_velocity(41, 1.5, 2.5, 3.5)", cpp_case_68 },
    { "anim_set_float", jce::diff::MOCK_OK, "jce.anim_set_float(41, 'anim_set_float_name', 2.5)", cpp_case_69 },
    { "anim_set_float [host says no]", jce::diff::MOCK_FAIL, "jce.anim_set_float(41, 'anim_set_float_name', 2.5)", cpp_case_70 },
    { "anim_set_int", jce::diff::MOCK_OK, "jce.anim_set_int(41, 'anim_set_int_name', 5)", cpp_case_71 },
    { "anim_set_int [host says no]", jce::diff::MOCK_FAIL, "jce.anim_set_int(41, 'anim_set_int_name', 5)", cpp_case_72 },
    { "anim_set_bool", jce::diff::MOCK_OK, "jce.anim_set_bool(41, 'anim_set_bool_name', true)", cpp_case_73 },
    { "anim_set_bool [host says no]", jce::diff::MOCK_FAIL, "jce.anim_set_bool(41, 'anim_set_bool_name', true)", cpp_case_74 },
    { "anim_set_trigger", jce::diff::MOCK_OK, "jce.anim_set_trigger(41, 'anim_set_trigger_name')", cpp_case_75 },
    { "anim_set_trigger [host says no]", jce::diff::MOCK_FAIL, "jce.anim_set_trigger(41, 'anim_set_trigger_name')", cpp_case_76 },
    { "is_action_down", jce::diff::MOCK_OK, "jce.is_action_down('is_action_down_name')", cpp_case_77 },
    { "is_action_down [host says no]", jce::diff::MOCK_FAIL, "jce.is_action_down('is_action_down_name')", cpp_case_78 },
    { "is_action_pressed", jce::diff::MOCK_OK, "jce.is_action_pressed('is_action_pressed_name')", cpp_case_79 },
    { "is_action_pressed [host says no]", jce::diff::MOCK_FAIL, "jce.is_action_pressed('is_action_pressed_name')", cpp_case_80 },
    { "get_axis", jce::diff::MOCK_OK, "jce.get_axis('get_axis_name')", cpp_case_81 },
    { "get_axis [host says no]", jce::diff::MOCK_FAIL, "jce.get_axis('get_axis_name')", cpp_case_82 },
    { "get_pointer_delta", jce::diff::MOCK_OK, "jce.get_pointer_delta()", cpp_case_83 },
    { "get_pointer_delta [host says no]", jce::diff::MOCK_FAIL, "jce.get_pointer_delta()", cpp_case_84 },
    { "get_pointer_wheel", jce::diff::MOCK_OK, "jce.get_pointer_wheel()", cpp_case_85 },
    { "get_pointer_wheel [host says no]", jce::diff::MOCK_FAIL, "jce.get_pointer_wheel()", cpp_case_86 },
    { "is_pointer_down", jce::diff::MOCK_OK, "jce.is_pointer_down(3)", cpp_case_87 },
    { "is_pointer_down [host says no]", jce::diff::MOCK_FAIL, "jce.is_pointer_down(3)", cpp_case_88 },
    { "get_touch_count", jce::diff::MOCK_OK, "jce.get_touch_count()", cpp_case_89 },
    { "get_touch_count [host says no]", jce::diff::MOCK_FAIL, "jce.get_touch_count()", cpp_case_90 },
    { "get_touch_count [host returns a negative count]", jce::diff::MOCK_NEGATIVE, "jce.get_touch_count()", cpp_case_91 },
    { "get_touch", jce::diff::MOCK_OK, "jce.get_touch(4)", cpp_case_92 },
    { "get_touch [host says no]", jce::diff::MOCK_FAIL, "jce.get_touch(4)", cpp_case_93 },
    { "get_touch [below the index base]", jce::diff::MOCK_OK, "jce.get_touch(0)", cpp_case_94 },
    { "tr", jce::diff::MOCK_OK, "jce.tr('tr_key')", cpp_case_95 },
    { "tr [host says no]", jce::diff::MOCK_FAIL, "jce.tr('tr_key')", cpp_case_96 },
    { "get_locale", jce::diff::MOCK_OK, "jce.get_locale()", cpp_case_97 },
    { "get_locale [host says no]", jce::diff::MOCK_FAIL, "jce.get_locale()", cpp_case_98 },
    { "set_locale", jce::diff::MOCK_OK, "jce.set_locale('set_locale_locale')", cpp_case_99 },
    { "set_locale [host says no]", jce::diff::MOCK_FAIL, "jce.set_locale('set_locale_locale')", cpp_case_100 },
    { "get_velocity", jce::diff::MOCK_OK, "jce.get_velocity(41)", cpp_case_101 },
    { "get_velocity [host says no]", jce::diff::MOCK_FAIL, "jce.get_velocity(41)", cpp_case_102 },
    { "vehicle_set_input", jce::diff::MOCK_OK, "jce.vehicle_set_input(41, 1.5, 2.5, 3.5)", cpp_case_103 },
    { "vehicle_set_input [host says no]", jce::diff::MOCK_FAIL, "jce.vehicle_set_input(41, 1.5, 2.5, 3.5)", cpp_case_104 },
    { "vehicle_get_speed", jce::diff::MOCK_OK, "jce.vehicle_get_speed(41)", cpp_case_105 },
    { "vehicle_get_speed [host says no]", jce::diff::MOCK_FAIL, "jce.vehicle_get_speed(41)", cpp_case_106 },
    { "get_move", jce::diff::MOCK_OK, "jce.get_move()", cpp_case_107 },
    { "get_move [host says no]", jce::diff::MOCK_FAIL, "jce.get_move()", cpp_case_108 },
    { "ui_get_slider", jce::diff::MOCK_OK, "jce.ui_get_slider(41)", cpp_case_109 },
    { "ui_get_slider [host says no]", jce::diff::MOCK_FAIL, "jce.ui_get_slider(41)", cpp_case_110 },
    { "ui_set_slider", jce::diff::MOCK_OK, "jce.ui_set_slider(41, 1.5)", cpp_case_111 },
    { "ui_set_slider [host says no]", jce::diff::MOCK_FAIL, "jce.ui_set_slider(41, 1.5)", cpp_case_112 },
    { "ui_get_progress", jce::diff::MOCK_OK, "jce.ui_get_progress(41)", cpp_case_113 },
    { "ui_get_progress [host says no]", jce::diff::MOCK_FAIL, "jce.ui_get_progress(41)", cpp_case_114 },
    { "ui_set_progress", jce::diff::MOCK_OK, "jce.ui_set_progress(41, 1.5)", cpp_case_115 },
    { "ui_set_progress [host says no]", jce::diff::MOCK_FAIL, "jce.ui_set_progress(41, 1.5)", cpp_case_116 },
    { "ui_get_toggle", jce::diff::MOCK_OK, "jce.ui_get_toggle(41)", cpp_case_117 },
    { "ui_get_toggle [host says no]", jce::diff::MOCK_FAIL, "jce.ui_get_toggle(41)", cpp_case_118 },
    { "ui_set_toggle", jce::diff::MOCK_OK, "jce.ui_set_toggle(41, true)", cpp_case_119 },
    { "ui_set_toggle [host says no]", jce::diff::MOCK_FAIL, "jce.ui_set_toggle(41, true)", cpp_case_120 },
    { "ui_set_text", jce::diff::MOCK_OK, "jce.ui_set_text(41, 'ui_set_text_txt')", cpp_case_121 },
    { "ui_set_text [host says no]", jce::diff::MOCK_FAIL, "jce.ui_set_text(41, 'ui_set_text_txt')", cpp_case_122 },
    { "send_message", jce::diff::MOCK_OK, "jce.send_message(41, 'send_message_msg', 2.5, 'send_message_str_arg')", cpp_case_123 },
    { "send_message [host says no]", jce::diff::MOCK_FAIL, "jce.send_message(41, 'send_message_msg', 2.5, 'send_message_str_arg')", cpp_case_124 },
    { "send_message [trailing defaults omitted]", jce::diff::MOCK_OK, "jce.send_message(41, 'send_message_msg')", cpp_case_125 },
    { "broadcast", jce::diff::MOCK_OK, "jce.broadcast('broadcast_msg', 1.5, 'broadcast_str_arg')", cpp_case_126 },
    { "broadcast [host says no]", jce::diff::MOCK_FAIL, "jce.broadcast('broadcast_msg', 1.5, 'broadcast_str_arg')", cpp_case_127 },
    { "broadcast [trailing defaults omitted]", jce::diff::MOCK_OK, "jce.broadcast('broadcast_msg')", cpp_case_128 },
    { "has_component", jce::diff::MOCK_OK, "jce.has_component(41, 'has_component_comp_name')", cpp_case_129 },
    { "has_component [host says no]", jce::diff::MOCK_FAIL, "jce.has_component(41, 'has_component_comp_name')", cpp_case_130 },
    { "is_component_enabled", jce::diff::MOCK_OK, "jce.is_component_enabled(41, 'is_component_enabled_comp_name')", cpp_case_131 },
    { "is_component_enabled [host says no]", jce::diff::MOCK_FAIL, "jce.is_component_enabled(41, 'is_component_enabled_comp_name')", cpp_case_132 },
    { "set_component_enabled", jce::diff::MOCK_OK, "jce.set_component_enabled(41, 'set_component_enabled_comp_name', true)", cpp_case_133 },
    { "set_component_enabled [host says no]", jce::diff::MOCK_FAIL, "jce.set_component_enabled(41, 'set_component_enabled_comp_name', true)", cpp_case_134 },
    { "net_is_server", jce::diff::MOCK_OK, "jce.net_is_server()", cpp_case_135 },
    { "net_is_server [host says no]", jce::diff::MOCK_FAIL, "jce.net_is_server()", cpp_case_136 },
    { "net_is_client", jce::diff::MOCK_OK, "jce.net_is_client()", cpp_case_137 },
    { "net_is_client [host says no]", jce::diff::MOCK_FAIL, "jce.net_is_client()", cpp_case_138 },
    { "net_spawn", jce::diff::MOCK_OK, "jce.net_spawn('net_spawn_prefab_path', 1.5, 2.5, 3.5)", cpp_case_139 },
    { "net_spawn [host says no]", jce::diff::MOCK_FAIL, "jce.net_spawn('net_spawn_prefab_path', 1.5, 2.5, 3.5)", cpp_case_140 },
    { "rpc_send", jce::diff::MOCK_OK, "jce.rpc_send(41, 'rpc_send_event', 5, 'rpc_send_payload')", cpp_case_141 },
    { "rpc_send [host says no]", jce::diff::MOCK_FAIL, "jce.rpc_send(41, 'rpc_send_event', 5, 'rpc_send_payload')", cpp_case_142 },
    { "rpc_send [trailing defaults omitted]", jce::diff::MOCK_OK, "jce.rpc_send(41, 'rpc_send_event')", cpp_case_143 },
    { "particle_burst", jce::diff::MOCK_OK, "jce.particle_burst(41, 4)", cpp_case_144 },
    { "particle_burst [host says no]", jce::diff::MOCK_FAIL, "jce.particle_burst(41, 4)", cpp_case_145 },
    { "particle_set_emitting", jce::diff::MOCK_OK, "jce.particle_set_emitting(41, true)", cpp_case_146 },
    { "particle_set_emitting [host says no]", jce::diff::MOCK_FAIL, "jce.particle_set_emitting(41, true)", cpp_case_147 },
    { "particle_set_color", jce::diff::MOCK_OK, "jce.particle_set_color(41, 1.5, 2.5, 3.5)", cpp_case_148 },
    { "particle_set_color [host says no]", jce::diff::MOCK_FAIL, "jce.particle_set_color(41, 1.5, 2.5, 3.5)", cpp_case_149 },
    { "find_by_name", jce::diff::MOCK_OK, "jce.find_by_name('find_by_name_name')", cpp_case_150 },
    { "find_by_name [host says no]", jce::diff::MOCK_FAIL, "jce.find_by_name('find_by_name_name')", cpp_case_151 },
    { "find_by_name [host returns a negative count]", jce::diff::MOCK_NEGATIVE, "jce.find_by_name('find_by_name_name')", cpp_case_152 },
    { "find_by_prefix", jce::diff::MOCK_OK, "jce.find_by_prefix('find_by_prefix_prefix')", cpp_case_153 },
    { "find_by_prefix [host says no]", jce::diff::MOCK_FAIL, "jce.find_by_prefix('find_by_prefix_prefix')", cpp_case_154 },
    { "find_by_prefix [host returns a negative count]", jce::diff::MOCK_NEGATIVE, "jce.find_by_prefix('find_by_prefix_prefix')", cpp_case_155 },
    { "comp_get", jce::diff::MOCK_OK, "jce.comp_get(41, 'comp_get_type')", cpp_case_156 },
    { "comp_get [host says no]", jce::diff::MOCK_FAIL, "jce.comp_get(41, 'comp_get_type')", cpp_case_157 },
    { "comp_set", jce::diff::MOCK_OK, "jce.comp_set(41, 'comp_set_type', 'comp_set_json')", cpp_case_158 },
    { "comp_set [host says no]", jce::diff::MOCK_FAIL, "jce.comp_set(41, 'comp_set_type', 'comp_set_json')", cpp_case_159 },
    { "render_get", jce::diff::MOCK_OK, "jce.render_get()", cpp_case_160 },
    { "render_get [host says no]", jce::diff::MOCK_FAIL, "jce.render_get()", cpp_case_161 },
    { "render_set", jce::diff::MOCK_OK, "jce.render_set('render_set_json')", cpp_case_162 },
    { "render_set [host says no]", jce::diff::MOCK_FAIL, "jce.render_set('render_set_json')", cpp_case_163 },
    { "audio_set_volume", jce::diff::MOCK_OK, "jce.audio_set_volume(41, 1.5)", cpp_case_164 },
    { "audio_set_volume [host says no]", jce::diff::MOCK_FAIL, "jce.audio_set_volume(41, 1.5)", cpp_case_165 },
    { "ui_get_dropdown", jce::diff::MOCK_OK, "jce.ui_get_dropdown(41)", cpp_case_166 },
    { "ui_get_dropdown [host says no]", jce::diff::MOCK_FAIL, "jce.ui_get_dropdown(41)", cpp_case_167 },
    { "ui_set_dropdown", jce::diff::MOCK_OK, "jce.ui_set_dropdown(41, 4)", cpp_case_168 },
    { "ui_set_dropdown [host says no]", jce::diff::MOCK_FAIL, "jce.ui_set_dropdown(41, 4)", cpp_case_169 },
    { "ui_get_input_text", jce::diff::MOCK_OK, "jce.ui_get_input_text(41)", cpp_case_170 },
    { "ui_get_input_text [host says no]", jce::diff::MOCK_FAIL, "jce.ui_get_input_text(41)", cpp_case_171 },
    { "ui_set_input_text", jce::diff::MOCK_OK, "jce.ui_set_input_text(41, 'ui_set_input_text_text')", cpp_case_172 },
    { "ui_set_input_text [host says no]", jce::diff::MOCK_FAIL, "jce.ui_set_input_text(41, 'ui_set_input_text_text')", cpp_case_173 },
    { "ui_get_scroll", jce::diff::MOCK_OK, "jce.ui_get_scroll(41)", cpp_case_174 },
    { "ui_get_scroll [host says no]", jce::diff::MOCK_FAIL, "jce.ui_get_scroll(41)", cpp_case_175 },
    { "ui_set_scroll", jce::diff::MOCK_OK, "jce.ui_set_scroll(41, 1.5, 2.5)", cpp_case_176 },
    { "ui_set_scroll [host says no]", jce::diff::MOCK_FAIL, "jce.ui_set_scroll(41, 1.5, 2.5)", cpp_case_177 },
    { "world_get_hour", jce::diff::MOCK_OK, "jce.world_get_hour()", cpp_case_178 },
    { "world_get_hour [host says no]", jce::diff::MOCK_FAIL, "jce.world_get_hour()", cpp_case_179 },
    { "world_set_hour", jce::diff::MOCK_OK, "jce.world_set_hour(0.5)", cpp_case_180 },
    { "world_set_hour [host says no]", jce::diff::MOCK_FAIL, "jce.world_set_hour(0.5)", cpp_case_181 },
    { "world_is_daytime", jce::diff::MOCK_OK, "jce.world_is_daytime()", cpp_case_182 },
    { "world_is_daytime [host says no]", jce::diff::MOCK_FAIL, "jce.world_is_daytime()", cpp_case_183 },
    { "world_get_weather", jce::diff::MOCK_OK, "jce.world_get_weather()", cpp_case_184 },
    { "world_get_weather [host says no]", jce::diff::MOCK_FAIL, "jce.world_get_weather()", cpp_case_185 },
    { "world_get_weather_intensity", jce::diff::MOCK_OK, "jce.world_get_weather_intensity()", cpp_case_186 },
    { "world_get_weather_intensity [host says no]", jce::diff::MOCK_FAIL, "jce.world_get_weather_intensity()", cpp_case_187 },
    { "world_get_wind_speed", jce::diff::MOCK_OK, "jce.world_get_wind_speed()", cpp_case_188 },
    { "world_get_wind_speed [host says no]", jce::diff::MOCK_FAIL, "jce.world_get_wind_speed()", cpp_case_189 },
    { "request_scene", jce::diff::MOCK_OK, "jce.request_scene('request_scene_scene_path')", cpp_case_190 },
    { "request_scene [host says no]", jce::diff::MOCK_FAIL, "jce.request_scene('request_scene_scene_path')", cpp_case_191 },
    { "is_transitioning", jce::diff::MOCK_OK, "jce.is_transitioning()", cpp_case_192 },
    { "is_transitioning [host says no]", jce::diff::MOCK_FAIL, "jce.is_transitioning()", cpp_case_193 },
    { "audio_play", jce::diff::MOCK_OK, "jce.audio_play(41)", cpp_case_194 },
    { "audio_play [host says no]", jce::diff::MOCK_FAIL, "jce.audio_play(41)", cpp_case_195 },
    { "audio_stop", jce::diff::MOCK_OK, "jce.audio_stop(41)", cpp_case_196 },
    { "audio_stop [host says no]", jce::diff::MOCK_FAIL, "jce.audio_stop(41)", cpp_case_197 },
    { "audio_is_playing", jce::diff::MOCK_OK, "jce.audio_is_playing(41)", cpp_case_198 },
    { "audio_is_playing [host says no]", jce::diff::MOCK_FAIL, "jce.audio_is_playing(41)", cpp_case_199 },
    { "save_game", jce::diff::MOCK_OK, "jce.save_game('save_game_path')", cpp_case_200 },
    { "save_game [host says no]", jce::diff::MOCK_FAIL, "jce.save_game('save_game_path')", cpp_case_201 },
    { "load_game", jce::diff::MOCK_OK, "jce.load_game('load_game_path')", cpp_case_202 },
    { "load_game [host says no]", jce::diff::MOCK_FAIL, "jce.load_game('load_game_path')", cpp_case_203 },
    { "overlap_sphere", jce::diff::MOCK_OK, "jce.overlap_sphere(0.5, 1.5, 2.5, 3.5, 11)", cpp_case_204 },
    { "overlap_sphere [host says no]", jce::diff::MOCK_FAIL, "jce.overlap_sphere(0.5, 1.5, 2.5, 3.5, 11)", cpp_case_205 },
    { "overlap_sphere [trailing defaults omitted]", jce::diff::MOCK_OK, "jce.overlap_sphere(0.5, 1.5, 2.5, 3.5)", cpp_case_206 },
    { "overlap_sphere [host returns a negative count]", jce::diff::MOCK_NEGATIVE, "jce.overlap_sphere(0.5, 1.5, 2.5, 3.5, 11)", cpp_case_207 },
    { "overlap_box", jce::diff::MOCK_OK, "jce.overlap_box(0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 13)", cpp_case_208 },
    { "overlap_box [host says no]", jce::diff::MOCK_FAIL, "jce.overlap_box(0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 13)", cpp_case_209 },
    { "overlap_box [trailing defaults omitted]", jce::diff::MOCK_OK, "jce.overlap_box(0.5, 1.5, 2.5, 3.5, 4.5, 5.5)", cpp_case_210 },
    { "overlap_box [host returns a negative count]", jce::diff::MOCK_NEGATIVE, "jce.overlap_box(0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 13)", cpp_case_211 },
    { "get_param", jce::diff::MOCK_OK, "jce.get_param(41, 'get_param_name')", cpp_case_212 },
    { "get_param [host says no]", jce::diff::MOCK_FAIL, "jce.get_param(41, 'get_param_name')", cpp_case_213 },
    { "get_param_text", jce::diff::MOCK_OK, "jce.get_param_text(41, 'get_param_text_name')", cpp_case_214 },
    { "get_param_text [host says no]", jce::diff::MOCK_FAIL, "jce.get_param_text(41, 'get_param_text_name')", cpp_case_215 },
    { "curve_eval", jce::diff::MOCK_OK, "jce.curve_eval('curve_eval_path', 'curve_eval_channel', 2.5)", cpp_case_216 },
    { "curve_eval [host says no]", jce::diff::MOCK_FAIL, "jce.curve_eval('curve_eval_path', 'curve_eval_channel', 2.5)", cpp_case_217 },
    { "vcam_activate", jce::diff::MOCK_OK, "jce.vcam_activate('vcam_activate_name')", cpp_case_218 },
    { "vcam_activate [host says no]", jce::diff::MOCK_FAIL, "jce.vcam_activate('vcam_activate_name')", cpp_case_219 },
};

#define JCE_DIFF_CASES ((int)(sizeof g_cases / sizeof g_cases[0]))

/* THE COVERAGE COUNT IS ANCHORED TO A DIFFERENT GENERATOR'S ARTEFACT,
 * and the first version of this file got that wrong.
 *
 * It compared JCE_DIFF_CASES against a literal emit_cpp.py had
 * printed from the same list it built g_cases from.  Those two
 * cannot disagree: it was an assertion that could not fail, which is
 * the twelfth of its kind this campaign has caught and the first I
 * wrote.  JCE_SCRIPT_API_ENTRY_COUNT comes from
 * scripting/c_abi/include/jce/script_api/jce_script_api.h, emitted by
 * tools/scriptgen/gen_script_c_abi.py -- a different tool reading the
 * same manifest.  If THIS backend silently stops emitting a case per
 * entry, that number does not move with it.
 */
static int cases_with_label(const char *suffix)
{
    int n = 0;
    for (int i = 0; i < JCE_DIFF_CASES; ++i) {
        const char *bracket = std::strchr(g_cases[i].label, '[');
        if (suffix == NULL) {
            if (bracket == NULL)
                ++n;
        } else if (bracket != NULL && std::strcmp(bracket, suffix) == 0) {
            ++n;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ *
 *  The comparison.
 * ------------------------------------------------------------------ */

static std::string run_cpp_case(const DiffCase &c, const JceScriptHost *host,
                                std::size_t host_size)
{
    jce::diff::Slots s;
    jce::diff::trace_reset();
    if (host != NULL) {
        Api api = Api::open(*host, host_size);
        c.run_cpp(api, s);
    } else {
        Api api;                      /* never opened: the empty handle */
        c.run_cpp(api, s);
    }
    return s.str();
}

TEST_CASE("every C ABI entry point has a case, in both directions")
{
    /* One BARE-labelled case per entry (the full-argument, host-succeeds
     * case), and one "[host says no]" case per entry.  Both counted against
     * the C ABI header's own macro, which this backend does not produce. */
    CHECK(cases_with_label(NULL) == JCE_SCRIPT_API_ENTRY_COUNT);
    CHECK(cases_with_label("[host says no]") == JCE_SCRIPT_API_ENTRY_COUNT);
    CHECK(JCE_DIFF_CASES > 2 * JCE_SCRIPT_API_ENTRY_COUNT);
}

TEST_CASE("Lua and C++ agree on every case, in result and in host calls")
{
    for (int i = 0; i < JCE_DIFF_CASES; ++i) {
        const DiffCase &c = g_cases[i];
        INFO("case: " << std::string(c.label));

        JceScriptHost ha;
        jce::diff::install_recording_host(&ha);
        jce::diff::g_mode = c.mode;
        std::string lua_out = jce::diff::run_lua_case(&ha, sizeof ha, c.lua);
        std::string lua_trace = jce::diff::trace_text();

        JceScriptHost hb;
        jce::diff::install_recording_host(&hb);
        jce::diff::g_mode = c.mode;
        std::string cpp_out = run_cpp_case(c, &hb, sizeof hb);
        std::string cpp_trace = jce::diff::trace_text();

        /* The Lua chunk must RUN.  Two sides erroring identically would
         * satisfy every comparison below; that is the shape of a test that
         * cannot fail, and batch 1's harness shipped exactly it once. */
        INFO("lua said: " << lua_out);
        REQUIRE(jce::diff::last_run_ok());
        CHECK(lua_out == cpp_out);
        CHECK(lua_trace == cpp_trace);
    }
    jce::diff::g_mode = jce::diff::MOCK_OK;
}

TEST_CASE("Lua and C++ agree when every host member is absent")
{
    for (int i = 0; i < JCE_DIFF_CASES; ++i) {
        const DiffCase &c = g_cases[i];
        INFO("absent-host case: " << std::string(c.label));

        JceScriptHost ha;
        jce::diff::install_log_only_host(&ha);
        jce::diff::g_mode = c.mode;
        std::string lua_out = jce::diff::run_lua_case(&ha, sizeof ha, c.lua);
        std::string lua_trace = jce::diff::trace_text();

        JceScriptHost hb;
        jce::diff::install_log_only_host(&hb);
        jce::diff::g_mode = c.mode;
        std::string cpp_out = run_cpp_case(c, &hb, sizeof hb);
        std::string cpp_trace = jce::diff::trace_text();

        INFO("lua said: " << lua_out);
        REQUIRE(jce::diff::last_run_ok());
        CHECK(lua_out == cpp_out);
        CHECK(lua_trace == cpp_trace);
        CHECK(lua_trace.empty());   /* there was nothing to call */
    }
    jce::diff::g_mode = jce::diff::MOCK_OK;
}

/* Not a differential: a C++-side invariant the header's banner promises.
 * A never-opened handle and a host whose members are all NULL are different
 * situations, and every entry point must answer them identically. */
TEST_CASE("an empty handle answers like an absent host")
{
    for (int i = 0; i < JCE_DIFF_CASES; ++i) {
        const DiffCase &c = g_cases[i];
        INFO("empty-handle case: " << std::string(c.label));

        JceScriptHost h;
        jce::diff::install_log_only_host(&h);
        jce::diff::g_mode = c.mode;
        std::string absent = run_cpp_case(c, &h, sizeof h);
        std::string empty = run_cpp_case(c, NULL, 0);
        CHECK(absent == empty);
    }
    jce::diff::g_mode = jce::diff::MOCK_OK;
}
