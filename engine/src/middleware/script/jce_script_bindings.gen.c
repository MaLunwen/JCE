/* jce_script_bindings.gen.c — GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * Source of truth: struct JceScriptHost in
 * engine/include/jce/middleware/script/jce_script.h (C types, arity,
 * parameter names) joined with the exposure decisions in
 * engine/src/middleware/script/script_exposure.json (name, shape, modifiers).
 *
 * This file is owned WHOLE by the generator, which never opens jce_script.c.
 * There are no sentinel-delimited regions: a tool that rewrites part of a file
 * containing hand-written code eventually eats the hand-written code.
 *
 * EVERY function guards the host member before calling it, and that is not
 * optional.  jce_script_create_sized copies min(host_size, sizeof
 * s->host) over a zeroed table, so a member an older caller's header did not
 * have stays NULL and calling it unguarded jumps through whatever followed
 * the caller's shorter object.  That is the crash
 * tests/middleware/script/test_jce_script_host_abi.c exists to catch.
 *
 * The guard has TWO spellings and grepping for only the first will convince
 * you this file is broken when it is not:
 *
 *     s->have_host && s->host.<member>          70 functions
 *     !s->have_host || !s->host.<member>        1 function (jce.get_touch),
 *                                               where an index_base binding
 *                                               folds the guard into its
 *                                               early return
 *
 * Both are checked, per function and against the member the manifest names,
 * by test_script_bindings_gate.py:
 *     test_the_have_host_guard_is_in_every_emitted_function
 *
 * No line numbers are cited into jce_script.c from this banner ON PURPOSE.
 * Line citations here would rot on the next edit to that file -- which is
 * this repository's most-repeated failure and the reason the manifest's own
 * citations are gated at all.  Symbols do not rot.
 */

#include "jce_script_bindings.gen.h"
#include "jce_script_internal.h"

#include <string.h>

/* jce.get_position — shape: fallible_out
 * World-space position of `entity`. Absent when the entity has no transform. */
static int l_jce_get_position(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float out_xyz[3];
    if (s->have_host && s->host.get_position &&
        s->host.get_position(s->host.user, e, out_xyz)) {
        lua_pushnumber(L, (lua_Number)out_xyz[0]);
        lua_pushnumber(L, (lua_Number)out_xyz[1]);
        lua_pushnumber(L, (lua_Number)out_xyz[2]);
        return 3;
    }
    lua_pushnil(L);
    return 1;
}

/* jce.set_position — shape: void_call */
static int l_jce_set_position(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    if (s->have_host && s->host.set_position)
        s->host.set_position(s->host.user, e, x, y, z);
    return 0;
}

/* jce.get_rotation — shape: fallible_out */
static int l_jce_get_rotation(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float out_euler_deg[3];
    if (s->have_host && s->host.get_rotation &&
        s->host.get_rotation(s->host.user, e, out_euler_deg)) {
        lua_pushnumber(L, (lua_Number)out_euler_deg[0]);
        lua_pushnumber(L, (lua_Number)out_euler_deg[1]);
        lua_pushnumber(L, (lua_Number)out_euler_deg[2]);
        return 3;
    }
    lua_pushnil(L);
    return 1;
}

/* jce.set_rotation — shape: void_call */
static int l_jce_set_rotation(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    if (s->have_host && s->host.set_rotation)
        s->host.set_rotation(s->host.user, e, x, y, z);
    return 0;
}

/* jce.get_scale — shape: fallible_out */
static int l_jce_get_scale(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float out_xyz[3];
    if (s->have_host && s->host.get_scale &&
        s->host.get_scale(s->host.user, e, out_xyz)) {
        lua_pushnumber(L, (lua_Number)out_xyz[0]);
        lua_pushnumber(L, (lua_Number)out_xyz[1]);
        lua_pushnumber(L, (lua_Number)out_xyz[2]);
        return 3;
    }
    lua_pushnil(L);
    return 1;
}

/* jce.set_scale — shape: void_call */
static int l_jce_set_scale(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    if (s->have_host && s->host.set_scale)
        s->host.set_scale(s->host.user, e, x, y, z);
    return 0;
}

/* jce.set_parent — shape: value_return
 * The ONLY boolean argument on this surface that is type-checked; the other five accept any truthy value. The luaL_checktype is emitted from this entry's `strict` modifier -- no line citation, because the hand-written body that carried it is gone. test_strict_emits_a_type_check_only_for_the_named_parameter is what fails if the emitter drops it. */
static int l_jce_set_parent(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity child = (JceScriptEntity)luaL_checkinteger(L, 1);
    JceScriptEntity parent = (JceScriptEntity)luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TBOOLEAN);
    bool preserve_world = lua_toboolean(L, 3) != 0;
    bool v = (s->have_host && s->host.set_parent)
                ? s->host.set_parent(s->host.user, child, parent, preserve_world) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.get_parent — shape: value_return */
static int l_jce_get_parent(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity child = (JceScriptEntity)luaL_checkinteger(L, 1);
    JceScriptEntity v = (s->have_host && s->host.get_parent)
                ? s->host.get_parent(s->host.user, child) : 0;
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

/* jce.is_key_down — shape: value_return */
static int l_jce_is_key_down(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    int keycode = (int)luaL_checkinteger(L, 1);
    bool v = (s->have_host && s->host.is_key_down)
                ? s->host.is_key_down(s->host.user, keycode) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.find_with_tag — shape: value_return */
static int l_jce_find_with_tag(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    const char *tag = luaL_checkstring(L, 1);
    JceScriptEntity v = (s->have_host && s->host.find_with_tag)
                ? s->host.find_with_tag(s->host.user, tag) : 0;
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

/* jce.destroy — shape: void_call */
static int l_jce_destroy(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    if (s->have_host && s->host.destroy_entity)
        s->host.destroy_entity(s->host.user, e);
    return 0;
}

/* jce.spawn — shape: value_return */
static int l_jce_spawn(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    const char *prefab_path = luaL_checkstring(L, 1);
    float x = (float)luaL_optnumber(L, 2, 0.0);
    float y = (float)luaL_optnumber(L, 3, 0.0);
    float z = (float)luaL_optnumber(L, 4, 0.0);
    JceScriptEntity v = (s->have_host && s->host.spawn)
                ? s->host.spawn(s->host.user, prefab_path, x, y, z) : 0;
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

/* jce.move_axis — shape: void_out_array */
static int l_jce_move_axis(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    float out_xz[2] = { 0.0f, 0.0f };
    if (s->have_host && s->host.move_axis)
        s->host.move_axis(s->host.user, out_xz);
    lua_pushnumber(L, (lua_Number)out_xz[0]);
    lua_pushnumber(L, (lua_Number)out_xz[1]);
    return 2;
}

/* jce.jump_pressed — shape: value_return */
static int l_jce_jump_pressed(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    bool v = (s->have_host && s->host.input_button)
                ? s->host.input_button(s->host.user, (int)0) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.sprint — shape: value_return */
static int l_jce_sprint(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    bool v = (s->have_host && s->host.input_button)
                ? s->host.input_button(s->host.user, (int)1) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.attack_pressed — shape: value_return */
static int l_jce_attack_pressed(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    bool v = (s->have_host && s->host.input_button)
                ? s->host.input_button(s->host.user, (int)2) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.set_time_scale — shape: void_call */
static int l_jce_set_time_scale(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    float scale = (float)luaL_checknumber(L, 1);
    if (s->have_host && s->host.set_time_scale)
        s->host.set_time_scale(s->host.user, scale);
    return 0;
}

/* jce.pause — shape: void_call
 * jce.pause() with no argument pauses; jce.pause(false) resumes. */
static int l_jce_pause(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    bool paused = lua_isnoneornil(L, 1) ? true : lua_toboolean(L, 1);
    if (s->have_host && s->host.set_paused)
        s->host.set_paused(s->host.user, paused);
    return 0;
}

/* jce.shake_camera — shape: void_call */
static int l_jce_shake_camera(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    float amount = (float)luaL_optnumber(L, 1, 0.5);
    if (s->have_host && s->host.shake_camera)
        s->host.shake_camera(s->host.user, amount);
    return 0;
}

/* jce.music_set_intensity — shape: void_call */
static int l_jce_music_set_intensity(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    float intensity = (float)luaL_checknumber(L, 1);
    if (s->have_host && s->host.music_set_intensity)
        s->host.music_set_intensity(s->host.user, intensity);
    return 0;
}

/* jce.music_get_intensity — shape: value_return */
static int l_jce_music_get_intensity(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    float v = (s->have_host && s->host.music_get_intensity)
                ? s->host.music_get_intensity(s->host.user) : 0.0f;
    lua_pushnumber(L, (lua_Number)v);
    return 1;
}

/* jce.music_request_transition — shape: value_return
 * Absolute playhead time of the quantized switch; negative on miss or no track, which is why the no-host value is -1 and not 0. */
static int l_jce_music_request_transition(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    int to_segment = (int)luaL_checkinteger(L, 1);
    float v = (s->have_host && s->host.music_request_transition)
                ? s->host.music_request_transition(s->host.user, to_segment) : -1.0f;
    lua_pushnumber(L, (lua_Number)v);
    return 1;
}

/* jce.gas_activate — shape: value_return */
static int l_jce_gas_activate(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    uint32_t ability_id = (uint32_t)luaL_checkinteger(L, 2);
    bool v = (s->have_host && s->host.gas_activate)
                ? s->host.gas_activate(s->host.user, e, ability_id) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.gas_get — shape: fallible_out */
static int l_jce_gas_get(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *attr_name = luaL_checkstring(L, 2);
    float out_value = 0.0f;
    if (s->have_host && s->host.gas_get &&
        s->host.gas_get(s->host.user, e, attr_name, &out_value)) {
        lua_pushnumber(L, (lua_Number)out_value);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

/* jce.gas_apply — shape: value_return */
static int l_jce_gas_apply(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *attr_name = luaL_checkstring(L, 2);
    int op = (int)luaL_optinteger(L, 3, 0);
    float magnitude = (float)luaL_checknumber(L, 4);
    float duration_seconds = (float)luaL_optnumber(L, 5, 0.0);
    bool v = (s->have_host && s->host.gas_apply)
                ? s->host.gas_apply(s->host.user, e, attr_name, op, magnitude, duration_seconds) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.raycast — shape: fallible_out
 * 8 values on a hit; a MISS pushes integer 0, not nil -- scripts branch on `e == 0`. */
static int l_jce_raycast(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    float origin[3];
    origin[0] = (float)luaL_checknumber(L, 1);
    origin[1] = (float)luaL_checknumber(L, 2);
    origin[2] = (float)luaL_checknumber(L, 3);
    float dir[3];
    dir[0] = (float)luaL_checknumber(L, 4);
    dir[1] = (float)luaL_checknumber(L, 5);
    dir[2] = (float)luaL_checknumber(L, 6);
    float max_dist = (float)luaL_checknumber(L, 7);
    JceScriptRaycastHit out;
    memset(&out, 0, sizeof out);
    if (s->have_host && s->host.raycast &&
        s->host.raycast(s->host.user, origin, dir, max_dist, &out)) {
        lua_pushinteger(L, (lua_Integer)out.entity);
        lua_pushnumber(L, (lua_Number)out.point[0]);
        lua_pushnumber(L, (lua_Number)out.point[1]);
        lua_pushnumber(L, (lua_Number)out.point[2]);
        lua_pushnumber(L, (lua_Number)out.normal[0]);
        lua_pushnumber(L, (lua_Number)out.normal[1]);
        lua_pushnumber(L, (lua_Number)out.normal[2]);
        lua_pushnumber(L, (lua_Number)out.distance);
        return 8;
    }
    lua_pushinteger(L, 0);   /* miss */
    return 1;
}

/* jce.apply_impulse — shape: void_call */
static int l_jce_apply_impulse(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    if (s->have_host && s->host.apply_impulse)
        s->host.apply_impulse(s->host.user, e, x, y, z);
    return 0;
}

/* jce.set_velocity — shape: void_call */
static int l_jce_set_velocity(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    if (s->have_host && s->host.set_velocity)
        s->host.set_velocity(s->host.user, e, x, y, z);
    return 0;
}

/* jce.anim_set_float — shape: void_call */
static int l_jce_anim_set_float(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *name = luaL_checkstring(L, 2);
    float v = (float)luaL_checknumber(L, 3);
    if (s->have_host && s->host.anim_set_float)
        s->host.anim_set_float(s->host.user, e, name, v);
    return 0;
}

/* jce.anim_set_int — shape: void_call */
static int l_jce_anim_set_int(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *name = luaL_checkstring(L, 2);
    int v = (int)luaL_checkinteger(L, 3);
    if (s->have_host && s->host.anim_set_int)
        s->host.anim_set_int(s->host.user, e, name, v);
    return 0;
}

/* jce.anim_set_bool — shape: void_call */
static int l_jce_anim_set_bool(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *name = luaL_checkstring(L, 2);
    bool v = lua_toboolean(L, 3) != 0;
    if (s->have_host && s->host.anim_set_bool)
        s->host.anim_set_bool(s->host.user, e, name, v);
    return 0;
}

/* jce.anim_set_trigger — shape: void_call */
static int l_jce_anim_set_trigger(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *name = luaL_checkstring(L, 2);
    if (s->have_host && s->host.anim_set_trigger)
        s->host.anim_set_trigger(s->host.user, e, name);
    return 0;
}

/* jce.is_action_down — shape: value_return */
static int l_jce_is_action_down(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    const char *name = luaL_checkstring(L, 1);
    bool v = (s->have_host && s->host.action_down)
                ? s->host.action_down(s->host.user, name) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.is_action_pressed — shape: value_return */
static int l_jce_is_action_pressed(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    const char *name = luaL_checkstring(L, 1);
    bool v = (s->have_host && s->host.action_pressed)
                ? s->host.action_pressed(s->host.user, name) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.get_axis — shape: value_return */
static int l_jce_get_axis(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    const char *name = luaL_checkstring(L, 1);
    float v = (s->have_host && s->host.action_axis)
                ? s->host.action_axis(s->host.user, name) : 0.0f;
    lua_pushnumber(L, (lua_Number)v);
    return 1;
}

/* jce.get_pointer_delta — shape: void_out_array */
static int l_jce_get_pointer_delta(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    float out_xy[2] = { 0.0f, 0.0f };
    if (s->have_host && s->host.pointer_delta)
        s->host.pointer_delta(s->host.user, out_xy);
    lua_pushnumber(L, (lua_Number)out_xy[0]);
    lua_pushnumber(L, (lua_Number)out_xy[1]);
    return 2;
}

/* jce.get_pointer_wheel — shape: value_return */
static int l_jce_get_pointer_wheel(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    float v = (s->have_host && s->host.pointer_wheel)
                ? s->host.pointer_wheel(s->host.user) : 0.0f;
    lua_pushnumber(L, (lua_Number)v);
    return 1;
}

/* jce.is_pointer_down — shape: value_return */
static int l_jce_is_pointer_down(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    int button = (int)luaL_checkinteger(L, 1);
    bool v = (s->have_host && s->host.pointer_button)
                ? s->host.pointer_button(s->host.user, button) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.get_touch_count — shape: value_return
 * A host returning a negative count is clamped to 0 so `for i = 1, jce.get_touch_count()` cannot underflow. */
static int l_jce_get_touch_count(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    int v = (s->have_host && s->host.touch_count)
                ? s->host.touch_count(s->host.user) : 0;
    if (v < 0)
        v = 0;
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

/* jce.get_touch — shape: fallible_out
 * 1-based Lua index mapped to 0-based C; an index below 1 returns nil without calling the host. */
static int l_jce_get_touch(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    lua_Integer lua_index = luaL_checkinteger(L, 1);
    uint64_t id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float pressure = 0.0f;
    if (lua_index < 1 || !s->have_host || !s->host.touch_get ||
        !s->host.touch_get(s->host.user, (int)(lua_index - 1), &id, &x, &y, &pressure)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)id);
    lua_pushnumber(L, (lua_Number)x);
    lua_pushnumber(L, (lua_Number)y);
    lua_pushnumber(L, (lua_Number)pressure);
    return 4;
}

/* jce.tr — shape: value_return
 * Passthrough is the contract, not a fallback: an unlocalized build shows readable keys instead of blank UI. */
static int l_jce_tr(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    const char *key = luaL_checkstring(L, 1);
    const char * v = (s->have_host && s->host.loc_translate)
                ? s->host.loc_translate(s->host.user, key) : key;
    lua_pushstring(L, v ? v : key);
    return 1;
}

/* jce.get_locale — shape: value_return */
static int l_jce_get_locale(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    const char * v = (s->have_host && s->host.loc_get_locale)
                ? s->host.loc_get_locale(s->host.user) : "";
    lua_pushstring(L, v ? v : "");
    return 1;
}

/* jce.set_locale — shape: void_call */
static int l_jce_set_locale(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    const char *locale = luaL_checkstring(L, 1);
    if (s->have_host && s->host.loc_set_locale)
        s->host.loc_set_locale(s->host.user, locale);
    return 0;
}

/* jce.get_velocity — shape: fallible_out */
static int l_jce_get_velocity(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float out[3];
    if (s->have_host && s->host.get_velocity &&
        s->host.get_velocity(s->host.user, e, out)) {
        lua_pushnumber(L, (lua_Number)out[0]);
        lua_pushnumber(L, (lua_Number)out[1]);
        lua_pushnumber(L, (lua_Number)out[2]);
        return 3;
    }
    lua_pushnil(L);
    return 1;
}

/* jce.vehicle_set_input — shape: void_call */
static int l_jce_vehicle_set_input(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float throttle = (float)luaL_checknumber(L, 2);
    float brake = (float)luaL_checknumber(L, 3);
    float steer = (float)luaL_checknumber(L, 4);
    if (s->have_host && s->host.vehicle_set_input)
        s->host.vehicle_set_input(s->host.user, e, throttle, brake, steer);
    return 0;
}

/* jce.vehicle_get_speed — shape: value_return */
static int l_jce_vehicle_get_speed(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float v = (s->have_host && s->host.vehicle_get_speed)
                ? s->host.vehicle_get_speed(s->host.user, e) : 0.0f;
    lua_pushnumber(L, (lua_Number)v);
    return 1;
}

/* jce.get_move — shape: void_out_array */
static int l_jce_get_move(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    float out[3] = { 0.0f, 0.0f, 0.0f };
    if (s->have_host && s->host.get_move)
        s->host.get_move(s->host.user, out);
    lua_pushnumber(L, (lua_Number)out[0]);
    lua_pushnumber(L, (lua_Number)out[1]);
    lua_pushnumber(L, (lua_Number)out[2]);
    return 3;
}

/* jce.ui_get_slider — shape: fallible_out */
static int l_jce_ui_get_slider(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float out = 0.0f;
    if (s->have_host && s->host.ui_get_slider &&
        s->host.ui_get_slider(s->host.user, e, &out)) {
        lua_pushnumber(L, (lua_Number)out);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

/* jce.ui_set_slider — shape: void_call */
static int l_jce_ui_set_slider(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float v = (float)luaL_checknumber(L, 2);
    if (s->have_host && s->host.ui_set_slider)
        s->host.ui_set_slider(s->host.user, e, v);
    return 0;
}

/* jce.ui_get_toggle — shape: fallible_out */
static int l_jce_ui_get_toggle(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    bool out = false;
    if (s->have_host && s->host.ui_get_toggle &&
        s->host.ui_get_toggle(s->host.user, e, &out)) {
        lua_pushboolean(L, out ? 1 : 0);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

/* jce.ui_set_toggle — shape: void_call */
static int l_jce_ui_set_toggle(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    bool v = lua_toboolean(L, 2) != 0;
    if (s->have_host && s->host.ui_set_toggle)
        s->host.ui_set_toggle(s->host.user, e, v);
    return 0;
}

/* jce.ui_set_text — shape: void_call */
static int l_jce_ui_set_text(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *txt = luaL_checkstring(L, 2);
    if (s->have_host && s->host.ui_set_text)
        s->host.ui_set_text(s->host.user, e, txt);
    return 0;
}

/* jce.send_message — shape: void_call */
static int l_jce_send_message(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity target = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *msg = luaL_checkstring(L, 2);
    double number_arg = luaL_optnumber(L, 3, 0.0);
    const char *str_arg = lua_isstring(L, 4) ? lua_tostring(L, 4) : NULL;
    if (s->have_host && s->host.send_message)
        s->host.send_message(s->host.user, target, msg, number_arg, str_arg);
    return 0;
}

/* jce.broadcast — shape: void_call */
static int l_jce_broadcast(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    const char *msg = luaL_checkstring(L, 1);
    double number_arg = luaL_optnumber(L, 2, 0.0);
    const char *str_arg = lua_isstring(L, 3) ? lua_tostring(L, 3) : NULL;
    if (s->have_host && s->host.broadcast)
        s->host.broadcast(s->host.user, msg, number_arg, str_arg);
    return 0;
}

/* jce.has_component — shape: value_return */
static int l_jce_has_component(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *comp_name = luaL_checkstring(L, 2);
    bool v = (s->have_host && s->host.has_component)
                ? s->host.has_component(s->host.user, e, comp_name) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.is_component_enabled — shape: value_return */
static int l_jce_is_component_enabled(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *comp_name = luaL_checkstring(L, 2);
    bool v = (s->have_host && s->host.is_component_enabled)
                ? s->host.is_component_enabled(s->host.user, e, comp_name) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.set_component_enabled — shape: void_call */
static int l_jce_set_component_enabled(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *comp_name = luaL_checkstring(L, 2);
    bool on = lua_toboolean(L, 3) != 0;
    if (s->have_host && s->host.set_component_enabled)
        s->host.set_component_enabled(s->host.user, e, comp_name, on);
    return 0;
}

/* jce.net_is_server — shape: value_return */
static int l_jce_net_is_server(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    bool v = (s->have_host && s->host.net_is_server)
                ? s->host.net_is_server(s->host.user) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.net_is_client — shape: value_return */
static int l_jce_net_is_client(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    bool v = (s->have_host && s->host.net_is_client)
                ? s->host.net_is_client(s->host.user) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.net_spawn — shape: value_return */
static int l_jce_net_spawn(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    const char *prefab_path = luaL_checkstring(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    JceScriptEntity v = (s->have_host && s->host.net_spawn)
                ? s->host.net_spawn(s->host.user, prefab_path, x, y, z) : 0;
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

/* jce.rpc_send — shape: value_return */
static int l_jce_rpc_send(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *event = luaL_checkstring(L, 2);
    int target = (int)luaL_optinteger(L, 3, 0);
    const char *payload = lua_isstring(L, 4) ? lua_tostring(L, 4) : NULL;
    bool v = (s->have_host && s->host.rpc_send)
                ? s->host.rpc_send(s->host.user, e, event, target, payload) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.particle_burst — shape: void_call */
static int l_jce_particle_burst(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    int count = (int)luaL_checkinteger(L, 2);
    if (s->have_host && s->host.particle_burst)
        s->host.particle_burst(s->host.user, e, count);
    return 0;
}

/* jce.particle_set_emitting — shape: void_call */
static int l_jce_particle_set_emitting(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    bool on = lua_toboolean(L, 2) != 0;
    if (s->have_host && s->host.particle_set_emitting)
        s->host.particle_set_emitting(s->host.user, e, on);
    return 0;
}

/* jce.particle_set_color — shape: void_call */
static int l_jce_particle_set_color(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float r = (float)luaL_checknumber(L, 2);
    float g = (float)luaL_checknumber(L, 3);
    float b = (float)luaL_checknumber(L, 4);
    if (s->have_host && s->host.particle_set_color)
        s->host.particle_set_color(s->host.user, e, r, g, b);
    return 0;
}

/* jce.find_by_name — shape: first_and_count
 * first-or-nil AND a match count, so a strict scene director can reject duplicate authored names. IDENTICAL C signature to find_by_prefix and a DIFFERENT contract. */
static int l_jce_find_by_name(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    const char *name = luaL_checkstring(L, 1);
    JceScriptEntity found[2];
    int n = 0;
    if (s->have_host && s->host.find_by_name)
        n = s->host.find_by_name(s->host.user, name, found, 2);
    if (n > 0)
        lua_pushinteger(L, (lua_Integer)found[0]);
    else
        lua_pushnil(L);
    lua_pushinteger(L, (lua_Integer)n);
    return 2;
}

/* jce.find_by_prefix — shape: entity_table
 * One Lua array. IDENTICAL C signature to find_by_name and a DIFFERENT contract. */
static int l_jce_find_by_prefix(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    const char *prefix = luaL_checkstring(L, 1);
    JceScriptEntity found[1024];
    int n = 0;
    if (s->have_host && s->host.find_by_prefix)
        n = s->host.find_by_prefix(s->host.user, prefix, found, 1024);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        lua_pushinteger(L, (lua_Integer)found[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* jce.comp_get — shape: owned_string_release */
static int l_jce_comp_get(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *type = luaL_checkstring(L, 2);
    char *json = NULL;
    if (s->have_host && s->host.comp_get_json)
        json = s->host.comp_get_json(s->host.user, e, type);
    if (json) {
        lua_pushstring(L, json);
        if (s->host.json_free) s->host.json_free(s->host.user, json);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/* jce.comp_set — shape: value_return */
static int l_jce_comp_set(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *type = luaL_checkstring(L, 2);
    const char *json = luaL_checkstring(L, 3);
    bool v = (s->have_host && s->host.comp_set_json)
                ? s->host.comp_set_json(s->host.user, e, type, json) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.render_get — shape: owned_string_release */
static int l_jce_render_get(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    char *json = NULL;
    if (s->have_host && s->host.render_get_json)
        json = s->host.render_get_json(s->host.user);
    if (json) {
        lua_pushstring(L, json);
        if (s->host.json_free) s->host.json_free(s->host.user, json);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/* jce.render_set — shape: value_return */
static int l_jce_render_set(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    const char *json = luaL_checkstring(L, 1);
    bool v = (s->have_host && s->host.render_set_json)
                ? s->host.render_set_json(s->host.user, json) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.audio_set_volume — shape: void_call */
static int l_jce_audio_set_volume(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float volume = (float)luaL_checknumber(L, 2);
    if (s->have_host && s->host.audio_set_volume)
        s->host.audio_set_volume(s->host.user, e, volume);
    return 0;
}

const char *const JCE_SCRIPT_GENERATED_BINDING_NAMES[] = {
    "get_position",
    "set_position",
    "get_rotation",
    "set_rotation",
    "get_scale",
    "set_scale",
    "set_parent",
    "get_parent",
    "is_key_down",
    "find_with_tag",
    "destroy",
    "spawn",
    "move_axis",
    "jump_pressed",
    "sprint",
    "attack_pressed",
    "set_time_scale",
    "pause",
    "shake_camera",
    "music_set_intensity",
    "music_get_intensity",
    "music_request_transition",
    "gas_activate",
    "gas_get",
    "gas_apply",
    "raycast",
    "apply_impulse",
    "set_velocity",
    "anim_set_float",
    "anim_set_int",
    "anim_set_bool",
    "anim_set_trigger",
    "is_action_down",
    "is_action_pressed",
    "get_axis",
    "get_pointer_delta",
    "get_pointer_wheel",
    "is_pointer_down",
    "get_touch_count",
    "get_touch",
    "tr",
    "get_locale",
    "set_locale",
    "get_velocity",
    "vehicle_set_input",
    "vehicle_get_speed",
    "get_move",
    "ui_get_slider",
    "ui_set_slider",
    "ui_get_toggle",
    "ui_set_toggle",
    "ui_set_text",
    "send_message",
    "broadcast",
    "has_component",
    "is_component_enabled",
    "set_component_enabled",
    "net_is_server",
    "net_is_client",
    "net_spawn",
    "rpc_send",
    "particle_burst",
    "particle_set_emitting",
    "particle_set_color",
    "find_by_name",
    "find_by_prefix",
    "comp_get",
    "comp_set",
    "render_get",
    "render_set",
    "audio_set_volume",
};

void jce_script_install_generated_bindings(JceScript *s)
{
    lua_State *L = s->L;
    jce_script_register_binding(L, s, "get_position", l_jce_get_position);
    jce_script_register_binding(L, s, "set_position", l_jce_set_position);
    jce_script_register_binding(L, s, "get_rotation", l_jce_get_rotation);
    jce_script_register_binding(L, s, "set_rotation", l_jce_set_rotation);
    jce_script_register_binding(L, s, "get_scale", l_jce_get_scale);
    jce_script_register_binding(L, s, "set_scale", l_jce_set_scale);
    jce_script_register_binding(L, s, "set_parent", l_jce_set_parent);
    jce_script_register_binding(L, s, "get_parent", l_jce_get_parent);
    jce_script_register_binding(L, s, "is_key_down", l_jce_is_key_down);
    jce_script_register_binding(L, s, "find_with_tag", l_jce_find_with_tag);
    jce_script_register_binding(L, s, "destroy", l_jce_destroy);
    jce_script_register_binding(L, s, "spawn", l_jce_spawn);
    jce_script_register_binding(L, s, "move_axis", l_jce_move_axis);
    jce_script_register_binding(L, s, "jump_pressed", l_jce_jump_pressed);
    jce_script_register_binding(L, s, "sprint", l_jce_sprint);
    jce_script_register_binding(L, s, "attack_pressed", l_jce_attack_pressed);
    jce_script_register_binding(L, s, "set_time_scale", l_jce_set_time_scale);
    jce_script_register_binding(L, s, "pause", l_jce_pause);
    jce_script_register_binding(L, s, "shake_camera", l_jce_shake_camera);
    jce_script_register_binding(L, s, "music_set_intensity", l_jce_music_set_intensity);
    jce_script_register_binding(L, s, "music_get_intensity", l_jce_music_get_intensity);
    jce_script_register_binding(L, s, "music_request_transition", l_jce_music_request_transition);
    jce_script_register_binding(L, s, "gas_activate", l_jce_gas_activate);
    jce_script_register_binding(L, s, "gas_get", l_jce_gas_get);
    jce_script_register_binding(L, s, "gas_apply", l_jce_gas_apply);
    jce_script_register_binding(L, s, "raycast", l_jce_raycast);
    jce_script_register_binding(L, s, "apply_impulse", l_jce_apply_impulse);
    jce_script_register_binding(L, s, "set_velocity", l_jce_set_velocity);
    jce_script_register_binding(L, s, "anim_set_float", l_jce_anim_set_float);
    jce_script_register_binding(L, s, "anim_set_int", l_jce_anim_set_int);
    jce_script_register_binding(L, s, "anim_set_bool", l_jce_anim_set_bool);
    jce_script_register_binding(L, s, "anim_set_trigger", l_jce_anim_set_trigger);
    jce_script_register_binding(L, s, "is_action_down", l_jce_is_action_down);
    jce_script_register_binding(L, s, "is_action_pressed", l_jce_is_action_pressed);
    jce_script_register_binding(L, s, "get_axis", l_jce_get_axis);
    jce_script_register_binding(L, s, "get_pointer_delta", l_jce_get_pointer_delta);
    jce_script_register_binding(L, s, "get_pointer_wheel", l_jce_get_pointer_wheel);
    jce_script_register_binding(L, s, "is_pointer_down", l_jce_is_pointer_down);
    jce_script_register_binding(L, s, "get_touch_count", l_jce_get_touch_count);
    jce_script_register_binding(L, s, "get_touch", l_jce_get_touch);
    jce_script_register_binding(L, s, "tr", l_jce_tr);
    jce_script_register_binding(L, s, "get_locale", l_jce_get_locale);
    jce_script_register_binding(L, s, "set_locale", l_jce_set_locale);
    jce_script_register_binding(L, s, "get_velocity", l_jce_get_velocity);
    jce_script_register_binding(L, s, "vehicle_set_input", l_jce_vehicle_set_input);
    jce_script_register_binding(L, s, "vehicle_get_speed", l_jce_vehicle_get_speed);
    jce_script_register_binding(L, s, "get_move", l_jce_get_move);
    jce_script_register_binding(L, s, "ui_get_slider", l_jce_ui_get_slider);
    jce_script_register_binding(L, s, "ui_set_slider", l_jce_ui_set_slider);
    jce_script_register_binding(L, s, "ui_get_toggle", l_jce_ui_get_toggle);
    jce_script_register_binding(L, s, "ui_set_toggle", l_jce_ui_set_toggle);
    jce_script_register_binding(L, s, "ui_set_text", l_jce_ui_set_text);
    jce_script_register_binding(L, s, "send_message", l_jce_send_message);
    jce_script_register_binding(L, s, "broadcast", l_jce_broadcast);
    jce_script_register_binding(L, s, "has_component", l_jce_has_component);
    jce_script_register_binding(L, s, "is_component_enabled", l_jce_is_component_enabled);
    jce_script_register_binding(L, s, "set_component_enabled", l_jce_set_component_enabled);
    jce_script_register_binding(L, s, "net_is_server", l_jce_net_is_server);
    jce_script_register_binding(L, s, "net_is_client", l_jce_net_is_client);
    jce_script_register_binding(L, s, "net_spawn", l_jce_net_spawn);
    jce_script_register_binding(L, s, "rpc_send", l_jce_rpc_send);
    jce_script_register_binding(L, s, "particle_burst", l_jce_particle_burst);
    jce_script_register_binding(L, s, "particle_set_emitting", l_jce_particle_set_emitting);
    jce_script_register_binding(L, s, "particle_set_color", l_jce_particle_set_color);
    jce_script_register_binding(L, s, "find_by_name", l_jce_find_by_name);
    jce_script_register_binding(L, s, "find_by_prefix", l_jce_find_by_prefix);
    jce_script_register_binding(L, s, "comp_get", l_jce_comp_get);
    jce_script_register_binding(L, s, "comp_set", l_jce_comp_set);
    jce_script_register_binding(L, s, "render_get", l_jce_render_get);
    jce_script_register_binding(L, s, "render_set", l_jce_render_set);
    jce_script_register_binding(L, s, "audio_set_volume", l_jce_audio_set_volume);
    /* json_null: lightuserdata_sentinel — no register_binding call,
     * so no source regex can see it.
     * tests/middleware/script/test_jce_script_table_shape.c can. */
    lua_pushlightuserdata(L, &jce_script_json_null_token);
    lua_setfield(L, -2, "json_null");
}
