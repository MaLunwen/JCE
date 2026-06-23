/*
 * jce_script.c  Gameplay scripting VM — Lua 5.4 host implementation.
 *
 * Generic Lua host (Phase 0 keystone). Knows nothing of the scene/ECS: all
 * engine access goes through the JceScriptHost callback bridge the runtime
 * installs at jce_script_create(). See api_script.h for the scripting model.
 *
 * Layering: depends only on jce_core + Lua. The `jce.*` Lua bindings retrieve
 * the owning JceScript* via a C-closure upvalue and dispatch to host->*.
 */

#include <jce/middleware/script/jce_script.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <string.h>

#define LOG_TAG "script"

/* Re-arms the per-dispatch instruction-budget watchdog (defined below); used by
 * the coroutine + chunk paths above its definition. */
static void script_arm_watchdog(lua_State *L);

/* Coroutine scheduler slot: a Lua thread (luaL_ref'd into the registry) parked
 * on a jce.wait_seconds() timer.  thread_ref == LUA_NOREF marks a free slot. */
#define JCE_SCRIPT_MAX_COROUTINES 256
typedef struct {
    int   thread_ref;   /* luaL_ref to the coroutine thread, or LUA_NOREF */
    float remaining;    /* seconds left on the current wait */
} JceScriptCoro;

struct JceScript {
    lua_State    *L;
    JceScriptHost host;       /* copied; callbacks may be NULL */
    bool          have_host;
    int           instance_count;

    /* Coroutine timer scheduler (jce.start_coroutine / jce.wait_seconds). */
    JceScriptCoro coros[JCE_SCRIPT_MAX_COROUTINES];
    int           coro_count;  /* high-water bound for iteration */
};

/* ── Host accessor ─────────────────────────────────────────────────────── */
/* Each jce.* binding is a C closure carrying the JceScript* as upvalue 1. */
static JceScript *self_from_upvalue(lua_State *L)
{
    return (JceScript *)lua_touserdata(L, lua_upvalueindex(1));
}

/* ── jce.* bindings ─────────────────────────────────────────────────────── */

static int l_jce_log(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    const char *msg = luaL_optstring(L, 1, "");
    if (s->have_host && s->host.log)
        s->host.log(s->host.user, msg);
    else
        LOG_INFO(LOG_TAG, "[lua] %s", msg);
    return 0;
}

static int l_jce_get_position(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float p[3];
    if (s->have_host && s->host.get_position &&
        s->host.get_position(s->host.user, e, p)) {
        lua_pushnumber(L, (lua_Number)p[0]);
        lua_pushnumber(L, (lua_Number)p[1]);
        lua_pushnumber(L, (lua_Number)p[2]);
        return 3;
    }
    lua_pushnil(L);
    return 1;
}

static int l_jce_set_position(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    if (s->have_host && s->host.set_position)
        s->host.set_position(s->host.user, e, x, y, z);
    return 0;
}

static int l_jce_is_key_down(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    int keycode = (int)luaL_checkinteger(L, 1);
    bool down = (s->have_host && s->host.is_key_down)
                    ? s->host.is_key_down(s->host.user, keycode)
                    : false;
    lua_pushboolean(L, down ? 1 : 0);
    return 1;
}

static int l_jce_set_time_scale(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    float scale = (float)luaL_checknumber(L, 1);
    if (s->have_host && s->host.set_time_scale)
        s->host.set_time_scale(s->host.user, scale);
    return 0;
}

static int l_jce_pause(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    /* jce.pause()        -> pause (default true)
       jce.pause(false)   -> resume */
    bool paused = lua_isnoneornil(L, 1) ? true : lua_toboolean(L, 1);
    if (s->have_host && s->host.set_paused)
        s->host.set_paused(s->host.user, paused);
    return 0;
}

static int l_jce_shake_camera(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    /* jce.shake_camera(amount) -- amount defaults to a solid 0.5 hit. */
    float amount = (float)luaL_optnumber(L, 1, 0.5);
    if (s->have_host && s->host.shake_camera)
        s->host.shake_camera(s->host.user, amount);
    return 0;
}

/* jce.music_set_intensity(value) -- set adaptive-music intensity (0..1). */
static int l_jce_music_set_intensity(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    float v = (float)luaL_checknumber(L, 1);
    if (s->have_host && s->host.music_set_intensity)
        s->host.music_set_intensity(s->host.user, v);
    return 0;
}

/* jce.music_get_intensity() -> number : current intensity (0 when no track). */
static int l_jce_music_get_intensity(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    float v = (s->have_host && s->host.music_get_intensity)
              ? s->host.music_get_intensity(s->host.user) : 0.0f;
    lua_pushnumber(L, v);
    return 1;
}

/* jce.music_request_transition(segment) -> number : the absolute playhead time
 * the beat/bar-quantized switch will fire (negative on miss / no track). */
static int l_jce_music_request_transition(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    int seg = (int)luaL_checkinteger(L, 1);
    float when = (s->have_host && s->host.music_request_transition)
                 ? s->host.music_request_transition(s->host.user, seg) : -1.0f;
    lua_pushnumber(L, when);
    return 1;
}

/* jce.gas_activate(entity, ability_id) -> bool
 * Fire the named ability on the entity's live GAS (cost + cooldown gated). */
static int l_jce_gas_activate(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    uint32_t ability_id = (uint32_t)luaL_checkinteger(L, 2);
    bool ok = (s->have_host && s->host.gas_activate)
                  ? s->host.gas_activate(s->host.user, e, ability_id) : false;
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

/* jce.gas_get(entity, attr_name) -> number | nil
 * Read the CURRENT (clamped, modifier-folded) value of an attribute. */
static int l_jce_gas_get(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *name = luaL_checkstring(L, 2);
    float v = 0.0f;
    if (s->have_host && s->host.gas_get &&
        s->host.gas_get(s->host.user, e, name, &v)) {
        lua_pushnumber(L, (lua_Number)v);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

/* jce.gas_apply(entity, attr_name, op, magnitude [, duration]) -> bool
 * op: 0=add 1=mult 2=override.  duration<=0 (or omitted) => INSTANT to base
 * (instant damage/heal); duration>0 => a continuous TIMED modifier. */
static int l_jce_gas_apply(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *name = luaL_checkstring(L, 2);
    int   op  = (int)luaL_optinteger(L, 3, 0);
    float mag = (float)luaL_checknumber(L, 4);
    float dur = (float)luaL_optnumber(L, 5, 0.0);
    bool ok = (s->have_host && s->host.gas_apply)
                  ? s->host.gas_apply(s->host.user, e, name, op, mag, dur)
                  : false;
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

/* jce.raycast(ox,oy,oz, dx,dy,dz, max_dist)
 *   -> hit_entity, px,py,pz, nx,ny,nz, dist   (8 values on a hit)
 *   -> 0                                        (single value on a miss)
 * Cast a ray through the live physics world; the host maps the hit body back
 * to its entity id (0 if untagged). */
static int l_jce_raycast(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    float origin[3], dir[3];
    origin[0] = (float)luaL_checknumber(L, 1);
    origin[1] = (float)luaL_checknumber(L, 2);
    origin[2] = (float)luaL_checknumber(L, 3);
    dir[0]    = (float)luaL_checknumber(L, 4);
    dir[1]    = (float)luaL_checknumber(L, 5);
    dir[2]    = (float)luaL_checknumber(L, 6);
    float max_dist = (float)luaL_checknumber(L, 7);

    JceScriptRaycastHit hit;
    memset(&hit, 0, sizeof hit);
    if (s->have_host && s->host.raycast &&
        s->host.raycast(s->host.user, origin, dir, max_dist, &hit)) {
        lua_pushinteger(L, (lua_Integer)hit.entity);
        lua_pushnumber(L, (lua_Number)hit.point[0]);
        lua_pushnumber(L, (lua_Number)hit.point[1]);
        lua_pushnumber(L, (lua_Number)hit.point[2]);
        lua_pushnumber(L, (lua_Number)hit.normal[0]);
        lua_pushnumber(L, (lua_Number)hit.normal[1]);
        lua_pushnumber(L, (lua_Number)hit.normal[2]);
        lua_pushnumber(L, (lua_Number)hit.distance);
        return 8;
    }
    lua_pushinteger(L, 0);   /* miss → single 0 */
    return 1;
}

/* jce.apply_impulse(entity, x,y,z): add an instantaneous impulse. */
static int l_jce_apply_impulse(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    if (s->have_host && s->host.apply_impulse)
        s->host.apply_impulse(s->host.user, e, x, y, z);
    return 0;
}

/* jce.set_velocity(entity, x,y,z): set the body's linear velocity (m/s). */
static int l_jce_set_velocity(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    if (s->have_host && s->host.set_velocity)
        s->host.set_velocity(s->host.user, e, x, y, z);
    return 0;
}

/* jce.anim_set_float(entity, name, v) — set an animator SM float param. */
static int l_jce_anim_set_float(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *name = luaL_checkstring(L, 2);
    float v = (float)luaL_checknumber(L, 3);
    if (s->have_host && s->host.anim_set_float) s->host.anim_set_float(s->host.user, e, name, v);
    return 0;
}
/* jce.anim_set_int(entity, name, v) — set an animator SM int param. */
static int l_jce_anim_set_int(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *name = luaL_checkstring(L, 2);
    int v = (int)luaL_checkinteger(L, 3);
    if (s->have_host && s->host.anim_set_int) s->host.anim_set_int(s->host.user, e, name, v);
    return 0;
}
/* jce.anim_set_bool(entity, name, on) — set an animator SM bool param. */
static int l_jce_anim_set_bool(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *name = luaL_checkstring(L, 2);
    bool v = lua_toboolean(L, 3) != 0;
    if (s->have_host && s->host.anim_set_bool) s->host.anim_set_bool(s->host.user, e, name, v);
    return 0;
}
/* jce.anim_set_trigger(entity, name) — fire a one-shot animator SM trigger. */
static int l_jce_anim_set_trigger(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *name = luaL_checkstring(L, 2);
    if (s->have_host && s->host.anim_set_trigger) s->host.anim_set_trigger(s->host.user, e, name);
    return 0;
}

/* jce.is_action_down(name) -> bool : is the named input action held this frame
 * (data-driven action map; false when unbound/unknown). */
static int l_jce_is_action_down(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    const char *name = luaL_checkstring(L, 1);
    bool down = (s->have_host && s->host.action_down)
                ? s->host.action_down(s->host.user, name) : false;
    lua_pushboolean(L, down);
    return 1;
}
/* jce.is_action_pressed(name) -> bool : did the named action go down THIS frame. */
static int l_jce_is_action_pressed(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    const char *name = luaL_checkstring(L, 1);
    bool pressed = (s->have_host && s->host.action_pressed)
                   ? s->host.action_pressed(s->host.user, name) : false;
    lua_pushboolean(L, pressed);
    return 1;
}
/* jce.get_axis(name) -> number : analog value of the named action (0 if none). */
static int l_jce_get_axis(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    const char *name = luaL_checkstring(L, 1);
    float v = (s->have_host && s->host.action_axis)
              ? s->host.action_axis(s->host.user, name) : 0.0f;
    lua_pushnumber(L, v);
    return 1;
}

/* jce.get_velocity(entity) -> x,y,z | nil (nil when the entity has no body). */
static int l_jce_get_velocity(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float v[3];
    if (s->have_host && s->host.get_velocity &&
        s->host.get_velocity(s->host.user, e, v)) {
        lua_pushnumber(L, (lua_Number)v[0]);
        lua_pushnumber(L, (lua_Number)v[1]);
        lua_pushnumber(L, (lua_Number)v[2]);
        return 3;
    }
    lua_pushnil(L);
    return 1;
}

/* jce.vehicle_set_input(entity, throttle, brake, steer): drive an authored
 * Vehicle (SCRIPT input mode).  throttle/brake in [0..1] (throttle <0 = reverse),
 * steer in [-1..1].  No-op if the entity has no live vehicle. */
static int l_jce_vehicle_set_input(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float throttle = (float)luaL_checknumber(L, 2);
    float brake    = (float)luaL_checknumber(L, 3);
    float steer    = (float)luaL_checknumber(L, 4);
    if (s->have_host && s->host.vehicle_set_input)
        s->host.vehicle_set_input(s->host.user, e, throttle, brake, steer);
    return 0;
}

/* jce.vehicle_get_speed(entity) -> number : signed forward speed (m/s), 0 if no
 * live vehicle. */
static int l_jce_vehicle_get_speed(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float spd = (s->have_host && s->host.vehicle_get_speed)
                ? s->host.vehicle_get_speed(s->host.user, e) : 0.0f;
    lua_pushnumber(L, (lua_Number)spd);
    return 1;
}

/* jce.get_move() -> steer, throttle, brake : raw movement intent the host fed
 * this frame (steer/throttle in [-1..1], brake 0/1).  Lets a script read WASD
 * without an authored action map. */
static int l_jce_get_move(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    float m[3] = { 0.0f, 0.0f, 0.0f };
    if (s->have_host && s->host.get_move) s->host.get_move(s->host.user, m);
    lua_pushnumber(L, (lua_Number)m[0]);
    lua_pushnumber(L, (lua_Number)m[1]);
    lua_pushnumber(L, (lua_Number)m[2]);
    return 3;
}

/* jce.play_sound(path [, x,y,z] [, volume])
 *   path required.  Optional 3 coords make it positional (3D); optional final
 *   number is the volume (default 1.0).  Arities supported:
 *     play_sound(path)                  -> 2D, vol 1.0
 *     play_sound(path, vol)             -> 2D, vol
 *     play_sound(path, x,y,z)           -> 3D, vol 1.0
 *     play_sound(path, x,y,z, vol)      -> 3D, vol */
static int l_jce_play_sound(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    const char *path = luaL_checkstring(L, 1);
    int top = lua_gettop(L);

    const float *pos_ptr = NULL;
    float pos[3];
    float volume = 1.0f;

    if (top >= 4) {
        /* coords present (args 2,3,4); optional volume at 5. */
        pos[0] = (float)luaL_checknumber(L, 2);
        pos[1] = (float)luaL_checknumber(L, 3);
        pos[2] = (float)luaL_checknumber(L, 4);
        pos_ptr = pos;
        volume = (float)luaL_optnumber(L, 5, 1.0);
    } else if (top == 2) {
        /* single trailing number => 2D volume. */
        volume = (float)luaL_optnumber(L, 2, 1.0);
    }
    /* top == 1 (or 3, ambiguous) => 2D default volume. */

    if (s->have_host && s->host.play_sound)
        s->host.play_sound(s->host.user, path, pos_ptr, volume);
    return 0;
}

/* jce.ui_get_slider(entity) -> number | nil */
static int l_jce_ui_get_slider(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float v = 0.0f;
    if (s->have_host && s->host.ui_get_slider &&
        s->host.ui_get_slider(s->host.user, e, &v)) {
        lua_pushnumber(L, (lua_Number)v);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

/* jce.ui_set_slider(entity, value) */
static int l_jce_ui_set_slider(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float v = (float)luaL_checknumber(L, 2);
    if (s->have_host && s->host.ui_set_slider)
        s->host.ui_set_slider(s->host.user, e, v);
    return 0;
}

/* jce.ui_get_toggle(entity) -> bool | nil */
static int l_jce_ui_get_toggle(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    bool on = false;
    if (s->have_host && s->host.ui_get_toggle &&
        s->host.ui_get_toggle(s->host.user, e, &on)) {
        lua_pushboolean(L, on ? 1 : 0);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

/* jce.ui_set_toggle(entity, on) */
static int l_jce_ui_set_toggle(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    bool on = lua_toboolean(L, 2) ? true : false;
    if (s->have_host && s->host.ui_set_toggle)
        s->host.ui_set_toggle(s->host.user, e, on);
    return 0;
}

/* jce.ui_set_text(entity, str) */
static int l_jce_ui_set_text(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *txt = luaL_checkstring(L, 2);
    if (s->have_host && s->host.ui_set_text)
        s->host.ui_set_text(s->host.user, e, txt);
    return 0;
}

/* jce.send_message(target_entity, msg [, number] [, string])
 * Deliver a message to another entity's live script (decoupled gameplay comms):
 * calls method `msg` on the target's instance with the (number, string) payload.
 * `number` defaults to 0; `string` is optional (omitted/non-string -> nil on the
 * receiver side).  No return value; a no-op when no host / no send_message
 * callback / the target has no matching handler. */
static int l_jce_send_message(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity target = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *msg = luaL_checkstring(L, 2);
    double num = luaL_optnumber(L, 3, 0.0);
    const char *str = lua_isstring(L, 4) ? lua_tostring(L, 4) : NULL;
    if (s->have_host && s->host.send_message)
        s->host.send_message(s->host.user, target, msg, num, str);
    return 0;
}

static int l_jce_broadcast(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    const char *msg = luaL_checkstring(L, 1);
    double num = luaL_optnumber(L, 2, 0.0);
    const char *str = lua_isstring(L, 3) ? lua_tostring(L, 3) : NULL;
    if (s->have_host && s->host.broadcast)
        s->host.broadcast(s->host.user, msg, num, str);
    return 0;
}

static int l_jce_has_component(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *name = luaL_checkstring(L, 2);
    bool has = (s->have_host && s->host.has_component)
                   ? s->host.has_component(s->host.user, e, name) : false;
    lua_pushboolean(L, has ? 1 : 0);
    return 1;
}

static int l_jce_is_component_enabled(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *name = luaL_checkstring(L, 2);
    bool on = (s->have_host && s->host.is_component_enabled)
                  ? s->host.is_component_enabled(s->host.user, e, name) : false;
    lua_pushboolean(L, on ? 1 : 0);
    return 1;
}

static int l_jce_set_component_enabled(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *name = luaL_checkstring(L, 2);
    bool on = lua_toboolean(L, 3) ? true : false;
    if (s->have_host && s->host.set_component_enabled)
        s->host.set_component_enabled(s->host.user, e, name, on);
    return 0;
}

/* jce.net_is_server() -> bool
 * True when the live replication role is the authoritative server; false (the
 * safe default) with no host / no net_is_server callback. */
static int l_jce_net_is_server(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    bool v = (s->have_host && s->host.net_is_server)
                 ? s->host.net_is_server(s->host.user) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.net_is_client() -> bool
 * True when the live role is a connected client; false (the safe default) with
 * no host / no net_is_client callback. */
static int l_jce_net_is_client(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    bool v = (s->have_host && s->host.net_is_client)
                 ? s->host.net_is_client(s->host.user) : false;
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

/* jce.net_spawn(prefab_path, x, y, z) -> entity
 * Server-authoritative networked spawn of a prefab at world (x,y,z); returns the
 * spawned NetworkObject's backing entity id, or 0 on a client / failure / no
 * host. */
static int l_jce_net_spawn(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    const char *path = luaL_checkstring(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    JceScriptEntity e = (s->have_host && s->host.net_spawn)
                            ? s->host.net_spawn(s->host.user, path, x, y, z) : 0;
    lua_pushinteger(L, (lua_Integer)e);
    return 1;
}

static int l_jce_rpc_send(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    const char *event = luaL_checkstring(L, 2);
    int target = (int)luaL_optinteger(L, 3, 0);              /* default TO_SERVER */
    const char *payload = lua_isstring(L, 4) ? lua_tostring(L, 4) : NULL;
    bool ok = (s->have_host && s->host.rpc_send)
                  ? s->host.rpc_send(s->host.user, e, event, target, payload)
                  : false;
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

/* jce.particle_burst(entity, count)
 * Fire a one-shot burst of `count` particles from the entity's emitter; a no-op
 * with no host / no callback / no particle emitter on the entity. */
static int l_jce_particle_burst(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    int count = (int)luaL_checkinteger(L, 2);
    if (s->have_host && s->host.particle_burst)
        s->host.particle_burst(s->host.user, e, count);
    return 0;
}

/* jce.particle_set_emitting(entity, on)
 * Start (`on`) / stop (!on) the entity emitter's continuous emission; a no-op
 * with no host / no callback / no particle emitter on the entity. */
static int l_jce_particle_set_emitting(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    bool on = lua_toboolean(L, 2) ? true : false;
    if (s->have_host && s->host.particle_set_emitting)
        s->host.particle_set_emitting(s->host.user, e, on);
    return 0;
}

static int l_jce_get_rotation(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float r[3];
    if (s->have_host && s->host.get_rotation &&
        s->host.get_rotation(s->host.user, e, r)) {
        lua_pushnumber(L, (lua_Number)r[0]);
        lua_pushnumber(L, (lua_Number)r[1]);
        lua_pushnumber(L, (lua_Number)r[2]);
        return 3;
    }
    lua_pushnil(L);
    return 1;
}

static int l_jce_set_rotation(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    if (s->have_host && s->host.set_rotation)
        s->host.set_rotation(s->host.user, e, x, y, z);
    return 0;
}

static int l_jce_get_scale(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float v[3];
    if (s->have_host && s->host.get_scale &&
        s->host.get_scale(s->host.user, e, v)) {
        lua_pushnumber(L, (lua_Number)v[0]);
        lua_pushnumber(L, (lua_Number)v[1]);
        lua_pushnumber(L, (lua_Number)v[2]);
        return 3;
    }
    lua_pushnil(L);
    return 1;
}

static int l_jce_set_scale(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    if (s->have_host && s->host.set_scale)
        s->host.set_scale(s->host.user, e, x, y, z);
    return 0;
}

static int l_jce_find_with_tag(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    const char *tag = luaL_checkstring(L, 1);
    JceScriptEntity e = (s->have_host && s->host.find_with_tag)
                            ? s->host.find_with_tag(s->host.user, tag) : 0;
    lua_pushinteger(L, (lua_Integer)e);
    return 1;
}

static int l_jce_destroy(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);
    if (s->have_host && s->host.destroy_entity)
        s->host.destroy_entity(s->host.user, e);
    return 0;
}

static int l_jce_spawn(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    const char *path = luaL_checkstring(L, 1);
    float x = (float)luaL_optnumber(L, 2, 0.0);
    float y = (float)luaL_optnumber(L, 3, 0.0);
    float z = (float)luaL_optnumber(L, 4, 0.0);
    JceScriptEntity e = (s->have_host && s->host.spawn)
                            ? s->host.spawn(s->host.user, path, x, y, z) : 0;
    lua_pushinteger(L, (lua_Integer)e);
    return 1;
}

static int l_jce_move_axis(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    float xz[2] = { 0.0f, 0.0f };
    if (s->have_host && s->host.move_axis)
        s->host.move_axis(s->host.user, xz);
    lua_pushnumber(L, (lua_Number)xz[0]);
    lua_pushnumber(L, (lua_Number)xz[1]);
    return 2;
}

static int l_jce_input_button(lua_State *L, int which)
{
    JceScript *s = self_from_upvalue(L);
    bool down = (s->have_host && s->host.input_button)
                    ? s->host.input_button(s->host.user, which) : false;
    lua_pushboolean(L, down ? 1 : 0);
    return 1;
}
static int l_jce_jump_pressed(lua_State *L) { return l_jce_input_button(L, 0); }
static int l_jce_sprint(lua_State *L)       { return l_jce_input_button(L, 1); }
static int l_jce_attack_pressed(lua_State *L){ return l_jce_input_button(L, 2); }

/* ── Coroutine timer scheduler (jce.start_coroutine / wait_seconds) ────────
 *
 * Unity-style cooperative coroutines: jce.start_coroutine(fn) runs `fn` as a
 * Lua thread immediately up to its first jce.wait_seconds(t) (so synchronous
 * setup before the first yield happens at once), then the runtime resumes it t
 * seconds later via jce_script_update_coroutines().  Pure Lua-thread state — no
 * host callback needed.  Returns an integer handle for jce.stop_coroutine. */

/* Find a free scheduler slot, growing the high-water bound; -1 when full. */
static int coro_alloc_slot(JceScript *s)
{
    for (int i = 0; i < JCE_SCRIPT_MAX_COROUTINES; ++i) {
        if (s->coros[i].thread_ref == LUA_NOREF) {
            if (i >= s->coro_count) s->coro_count = i + 1;
            return i;
        }
    }
    return -1;
}

static int l_jce_start_coroutine(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);

    /* New thread on the MAIN state so the registry ref keeps it alive even when
     * start_coroutine is itself called from inside another coroutine. */
    lua_State *co = lua_newthread(s->L);        /* [main: .., co] */
    lua_pushvalue(L, 1);                        /* [L: .., fn] (copy of arg) */
    lua_xmove(L, co, 1);                        /* move fn onto co: [co: fn] */
    int ref = luaL_ref(s->L, LUA_REGISTRYINDEX);/* pops co (top of main), refs it */

    int nres = 0;
    script_arm_watchdog(co);                     /* hook on the coroutine thread */
    int st = lua_resume(co, L, 0, &nres);       /* run up to first yield/return */
    if (st == LUA_YIELD) {
        float wait = (nres >= 1) ? (float)lua_tonumber(co, -1) : 0.0f;
        lua_pop(co, nres);                      /* clear yielded results */
        int slot = coro_alloc_slot(s);
        if (slot < 0) {                         /* scheduler full → drop it */
            luaL_unref(s->L, LUA_REGISTRYINDEX, ref);
            lua_pushinteger(L, 0);
            return 1;
        }
        s->coros[slot].thread_ref = ref;
        s->coros[slot].remaining  = wait < 0.0f ? 0.0f : wait;
        lua_pushinteger(L, ref);                /* handle for stop_coroutine */
        return 1;
    }
    if (st != LUA_OK) {                          /* errored before first yield */
        const char *err = lua_tostring(co, -1);
        if (s->have_host && s->host.log) {
            char buf[512];
            snprintf(buf, sizeof(buf), "coroutine error: %s", err ? err : "?");
            s->host.log(s->host.user, buf);
        }
        LOG_ERROR(LOG_TAG, "coroutine error: %s", err ? err : "?");
    }
    /* Completed (or errored) without ever waiting — nothing to schedule. */
    luaL_unref(s->L, LUA_REGISTRYINDEX, ref);
    lua_pushinteger(L, 0);
    return 1;
}

static int l_jce_wait_seconds(lua_State *L)
{
    double t = luaL_optnumber(L, 1, 0.0);
    if (!lua_isyieldable(L))
        return luaL_error(L, "jce.wait_seconds must be called inside jce.start_coroutine");
    lua_pushnumber(L, t);
    return lua_yield(L, 1);                      /* yields t to the resumer */
}

static int l_jce_stop_coroutine(lua_State *L)
{
    JceScript *s = self_from_upvalue(L);
    int handle = (int)luaL_optinteger(L, 1, 0);
    if (handle != 0 && handle != LUA_NOREF) {
        for (int i = 0; i < s->coro_count; ++i) {
            if (s->coros[i].thread_ref == handle) {
                luaL_unref(s->L, LUA_REGISTRYINDEX, handle);
                s->coros[i].thread_ref = LUA_NOREF;
                break;
            }
        }
    }
    return 0;
}

/* Register one closure carrying `s` as upvalue into the table on top. */
static void register_binding(lua_State *L, JceScript *s,
                             const char *name, lua_CFunction fn)
{
    lua_pushlightuserdata(L, s);
    lua_pushcclosure(L, fn, 1);
    lua_setfield(L, -2, name);
}

static void install_bindings(JceScript *s)
{
    lua_State *L = s->L;
    lua_newtable(L);                         /* the `jce` table */
    register_binding(L, s, "log",            l_jce_log);
    register_binding(L, s, "get_position",   l_jce_get_position);
    register_binding(L, s, "set_position",   l_jce_set_position);
    register_binding(L, s, "get_rotation",   l_jce_get_rotation);
    register_binding(L, s, "set_rotation",   l_jce_set_rotation);
    register_binding(L, s, "get_scale",      l_jce_get_scale);
    register_binding(L, s, "set_scale",      l_jce_set_scale);
    register_binding(L, s, "is_key_down",    l_jce_is_key_down);
    register_binding(L, s, "find_with_tag",  l_jce_find_with_tag);
    register_binding(L, s, "destroy",        l_jce_destroy);
    register_binding(L, s, "spawn",          l_jce_spawn);
    register_binding(L, s, "move_axis",      l_jce_move_axis);
    register_binding(L, s, "jump_pressed",   l_jce_jump_pressed);
    register_binding(L, s, "sprint",         l_jce_sprint);
    register_binding(L, s, "attack_pressed", l_jce_attack_pressed);
    register_binding(L, s, "set_time_scale", l_jce_set_time_scale);
    register_binding(L, s, "pause",          l_jce_pause);
    register_binding(L, s, "shake_camera",   l_jce_shake_camera);
    register_binding(L, s, "music_set_intensity",      l_jce_music_set_intensity);
    register_binding(L, s, "music_get_intensity",      l_jce_music_get_intensity);
    register_binding(L, s, "music_request_transition", l_jce_music_request_transition);
    register_binding(L, s, "gas_activate",   l_jce_gas_activate);
    register_binding(L, s, "gas_get",        l_jce_gas_get);
    register_binding(L, s, "gas_apply",      l_jce_gas_apply);
    register_binding(L, s, "raycast",        l_jce_raycast);
    register_binding(L, s, "apply_impulse",  l_jce_apply_impulse);
    register_binding(L, s, "set_velocity",   l_jce_set_velocity);
    register_binding(L, s, "anim_set_float",   l_jce_anim_set_float);
    register_binding(L, s, "anim_set_int",     l_jce_anim_set_int);
    register_binding(L, s, "anim_set_bool",    l_jce_anim_set_bool);
    register_binding(L, s, "anim_set_trigger", l_jce_anim_set_trigger);
    register_binding(L, s, "is_action_down",    l_jce_is_action_down);
    register_binding(L, s, "is_action_pressed", l_jce_is_action_pressed);
    register_binding(L, s, "get_axis",          l_jce_get_axis);
    register_binding(L, s, "get_velocity",   l_jce_get_velocity);
    register_binding(L, s, "vehicle_set_input", l_jce_vehicle_set_input);
    register_binding(L, s, "vehicle_get_speed", l_jce_vehicle_get_speed);
    register_binding(L, s, "get_move",          l_jce_get_move);
    register_binding(L, s, "play_sound",     l_jce_play_sound);
    register_binding(L, s, "ui_get_slider",  l_jce_ui_get_slider);
    register_binding(L, s, "ui_set_slider",  l_jce_ui_set_slider);
    register_binding(L, s, "ui_get_toggle",  l_jce_ui_get_toggle);
    register_binding(L, s, "ui_set_toggle",  l_jce_ui_set_toggle);
    register_binding(L, s, "ui_set_text",    l_jce_ui_set_text);
    register_binding(L, s, "send_message",   l_jce_send_message);
    register_binding(L, s, "broadcast",      l_jce_broadcast);
    register_binding(L, s, "has_component",        l_jce_has_component);
    register_binding(L, s, "is_component_enabled", l_jce_is_component_enabled);
    register_binding(L, s, "set_component_enabled", l_jce_set_component_enabled);
    register_binding(L, s, "start_coroutine", l_jce_start_coroutine);
    register_binding(L, s, "wait_seconds",    l_jce_wait_seconds);
    register_binding(L, s, "stop_coroutine",  l_jce_stop_coroutine);
    register_binding(L, s, "net_is_server",  l_jce_net_is_server);
    register_binding(L, s, "net_is_client",  l_jce_net_is_client);
    register_binding(L, s, "net_spawn",      l_jce_net_spawn);
    register_binding(L, s, "rpc_send",       l_jce_rpc_send);
    register_binding(L, s, "particle_burst", l_jce_particle_burst);
    register_binding(L, s, "particle_set_emitting", l_jce_particle_set_emitting);
    lua_setglobal(L, "jce");
}

/* ── Sandboxed standard libs ────────────────────────────────────────────── */
/* Open base/table/string/math only — NOT io/os/package/debug, so scripts
 * can't touch the filesystem, processes, or load native code. */
static void open_sandboxed_libs(lua_State *L)
{
    static const luaL_Reg libs[] = {
        { LUA_GNAME,      luaopen_base },
        { LUA_TABLIBNAME, luaopen_table },
        { LUA_STRLIBNAME, luaopen_string },
        { LUA_MATHLIBNAME, luaopen_math },
        { NULL, NULL },
    };
    for (const luaL_Reg *lib = libs; lib->func; lib++) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

JceScript *jce_script_create(const JceScriptHost *host)
{
    JceScript *s = (JceScript *)jce_malloc(sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));

    s->L = luaL_newstate();
    if (!s->L) {
        jce_free(s);
        LOG_ERROR(LOG_TAG, "luaL_newstate failed (OOM)");
        return NULL;
    }
    if (host) { s->host = *host; s->have_host = true; }

    /* Coroutine slots start free (memset 0 would read as a valid ref). */
    for (int i = 0; i < JCE_SCRIPT_MAX_COROUTINES; ++i)
        s->coros[i].thread_ref = LUA_NOREF;
    s->coro_count = 0;

    open_sandboxed_libs(s->L);
    install_bindings(s);

    LOG_SUCCESS(LOG_TAG, "Lua %s VM created%s",
                LUA_VERSION_MAJOR "." LUA_VERSION_MINOR,
                s->have_host ? " (host bridged)" : "");
    return s;
}

void jce_script_destroy(JceScript *s)
{
    if (!s) return;
    if (s->L) lua_close(s->L);
    jce_free(s);
}

/* ── Instantiation ──────────────────────────────────────────────────────── */

/* Stack on entry: [ module ] (the value the chunk returned).
 * Builds an instance table { entity=owner, <meta __index=module> }, refs it,
 * and returns the ref. Pops the module. Returns 0 on bad module. */
static JceScriptInstance build_instance(lua_State *L, JceScriptEntity owner)
{
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);                       /* drop bad return */
        return 0;
    }
    /* instance = {} */
    lua_newtable(L);                         /* [ module, inst ] */
    lua_pushinteger(L, (lua_Integer)owner);
    lua_setfield(L, -2, "entity");           /* inst.entity = owner */
    /* meta = { __index = module } */
    lua_newtable(L);                         /* [ module, inst, meta ] */
    lua_pushvalue(L, -3);                    /* push module */
    lua_setfield(L, -2, "__index");          /* meta.__index = module */
    lua_setmetatable(L, -2);                 /* setmetatable(inst, meta) -> [ module, inst ] */
    /* ref the instance, pop it, then drop the module */
    int ref = luaL_ref(L, LUA_REGISTRYINDEX); /* pops inst -> [ module ] */
    lua_pop(L, 1);                           /* drop module */
    return (JceScriptInstance)ref;
}

static JceScriptInstance run_chunk(JceScript *s, JceScriptEntity owner,
                                   const char *what)
{
    lua_State *L = s->L;
    /* chunk is on stack top (loaded by caller). Run it expecting 1 return. */
    script_arm_watchdog(L);
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        if (s->have_host && s->host.log) {
            char buf[512];
            snprintf(buf, sizeof(buf), "script run error (%s): %s", what,
                     err ? err : "?");
            s->host.log(s->host.user, buf);
        }
        LOG_ERROR(LOG_TAG, "run error (%s): %s", what, err ? err : "?");
        lua_pop(L, 1);
        return 0;
    }
    JceScriptInstance inst = build_instance(L, owner);
    if (inst == 0)
        LOG_ERROR(LOG_TAG, "script '%s' did not return a table", what);
    else
        s->instance_count++;
    return inst;
}

JceScriptInstance jce_script_instantiate_source(JceScript *s, const char *name,
                                                const char *source,
                                                JceScriptEntity owner)
{
    if (!s || !source) return 0;
    const char *chunkname = name ? name : "=chunk";
    if (luaL_loadbuffer(s->L, source, strlen(source), chunkname) != LUA_OK) {
        const char *err = lua_tostring(s->L, -1);
        LOG_ERROR(LOG_TAG, "compile error (%s): %s", chunkname, err ? err : "?");
        lua_pop(s->L, 1);
        return 0;
    }
    return run_chunk(s, owner, chunkname);
}

JceScriptInstance jce_script_instantiate(JceScript *s, const char *path,
                                         JceScriptEntity owner)
{
    if (!s || !path) return 0;
    if (!s->have_host || !s->host.read_file) {
        LOG_ERROR(LOG_TAG, "no read_file host callback; cannot load '%s'", path);
        return 0;
    }
    uint64_t size = 0;
    void *buf = s->host.read_file(s->host.user, path, &size);
    if (!buf || size == 0) {
        if (buf) jce_free(buf);
        LOG_ERROR(LOG_TAG, "cannot read script '%s'", path);
        return 0;
    }
    char chunkname[256];
    snprintf(chunkname, sizeof(chunkname), "@%s", path);
    int load = luaL_loadbuffer(s->L, (const char *)buf, (size_t)size, chunkname);
    jce_free(buf);
    if (load != LUA_OK) {
        const char *err = lua_tostring(s->L, -1);
        LOG_ERROR(LOG_TAG, "compile error (%s): %s", path, err ? err : "?");
        lua_pop(s->L, 1);
        return 0;
    }
    return run_chunk(s, owner, path);
}

/* ── Script watchdog: abort a runaway dispatch (infinite loop) ─────────────
 * A LUA_MASKCOUNT hook, RE-ARMED before every pcall/resume so each dispatch
 * gets a fresh instruction budget.  If one call burns past the budget it
 * luaL_error()s out of the pcall instead of hard-hanging the single-threaded
 * runtime (editor AND shipped).  Legit per-frame script work is orders of
 * magnitude under the budget, so the hook never fires in normal use. */
#define JCE_SCRIPT_WATCHDOG_INSTR 40000000
static void script_watchdog_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    luaL_error(L, "script watchdog: call exceeded %d instructions (infinite loop?)",
               JCE_SCRIPT_WATCHDOG_INSTR);
}
static void script_arm_watchdog(lua_State *L)
{
    lua_sethook(L, script_watchdog_hook, LUA_MASKCOUNT, JCE_SCRIPT_WATCHDOG_INSTR);
}

/* ── Lifecycle dispatch ─────────────────────────────────────────────────── */

/* Push instance[fn]; if it's a function, push the instance as `self` and any
 * extra args pushed by the caller before this call are NOT supported here —
 * we handle the fixed arities inline below. */
static void call_method(JceScript *s, JceScriptInstance inst,
                        const char *method, bool has_dt, float dt)
{
    if (!s || inst == 0) return;
    lua_State *L = s->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, (lua_Integer)inst);  /* [ inst ] */
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    lua_getfield(L, -1, method);                            /* [ inst, fn ] (via meta) */
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return; }
    lua_pushvalue(L, -2);                                   /* [ inst, fn, self ] */
    int nargs = 1;
    if (has_dt) { lua_pushnumber(L, (lua_Number)dt); nargs = 2; } /* [ ..., dt ] */
    script_arm_watchdog(L);
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {              /* pops fn+args */
        const char *err = lua_tostring(L, -1);
        if (s->have_host && s->host.log) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s error: %s", method, err ? err : "?");
            s->host.log(s->host.user, buf);
        }
        LOG_ERROR(LOG_TAG, "%s error: %s", method, err ? err : "?");
        lua_pop(L, 1);                                      /* drop error */
    }
    lua_pop(L, 1);                                          /* drop inst */
}

void jce_script_call_start(JceScript *s, JceScriptInstance inst)
{
    call_method(s, inst, "on_start", false, 0.0f);
}

void jce_script_call_update(JceScript *s, JceScriptInstance inst, float dt)
{
    call_method(s, inst, "on_update", true, dt);
}

void jce_script_release(JceScript *s, JceScriptInstance inst)
{
    if (!s || inst == 0) return;
    call_method(s, inst, "on_destroy", false, 0.0f);
    luaL_unref(s->L, LUA_REGISTRYINDEX, (int)inst);
    if (s->instance_count > 0) s->instance_count--;
}

/* on_collision(self, other_entity) — like call_method but with a second
 * integer argument (the other body's entity id).  No-op if the script defines
 * no on_collision (so scripts opt in by simply declaring the method). */
void jce_script_call_collision(JceScript *s, JceScriptInstance inst,
                               JceScriptEntity other_entity)
{
    if (!s || inst == 0) return;
    lua_State *L = s->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, (lua_Integer)inst);   /* [ inst ] */
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    lua_getfield(L, -1, "on_collision");                    /* [ inst, fn ] */
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return; }
    lua_pushvalue(L, -2);                                   /* [ inst, fn, self ] */
    lua_pushinteger(L, (lua_Integer)other_entity);         /* [ inst, fn, self, other ] */
    script_arm_watchdog(L);
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {                 /* pops fn + args */
        const char *err = lua_tostring(L, -1);
        if (s->have_host && s->host.log) {
            char buf[512];
            snprintf(buf, sizeof(buf), "on_collision error: %s", err ? err : "?");
            s->host.log(s->host.user, buf);
        }
        LOG_ERROR(LOG_TAG, "on_collision error: %s", err ? err : "?");
        lua_pop(L, 1);                                      /* drop error */
    }
    lua_pop(L, 1);                                          /* drop inst */
}

/* msg_name(self, number_arg, str_arg) — script-to-script message dispatch.
 * Mirrors jce_script_call_collision's instance-method dispatch, but with a
 * configurable method name and a (number, string|nil) payload.  No-op if the
 * receiver defines no method named msg_name (so receivers opt in by simply
 * declaring the method); str_arg NULL pushes nil. */
void jce_script_call_message(JceScript *s, JceScriptInstance inst,
                             const char *msg_name, double number_arg,
                             const char *str_arg)
{
    if (!s || inst == 0 || !msg_name || !msg_name[0]) return;
    lua_State *L = s->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, (lua_Integer)inst);   /* [ inst ] */
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    lua_getfield(L, -1, msg_name);                          /* [ inst, fn ] (via meta) */
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return; }  /* missing handler: clean no-op */
    lua_pushvalue(L, -2);                                   /* [ inst, fn, self ] */
    lua_pushnumber(L, (lua_Number)number_arg);             /* [ inst, fn, self, num ] */
    if (str_arg) lua_pushstring(L, str_arg);               /* [ ..., str ] */
    else         lua_pushnil(L);                            /* [ ..., nil ] */
    script_arm_watchdog(L);
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {                 /* pops fn + 3 args */
        const char *err = lua_tostring(L, -1);
        if (s->have_host && s->host.log) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s error: %s", msg_name, err ? err : "?");
            s->host.log(s->host.user, buf);
        }
        LOG_ERROR(LOG_TAG, "%s error: %s", msg_name, err ? err : "?");
        lua_pop(L, 1);                                      /* drop error */
    }
    lua_pop(L, 1);                                          /* drop inst */
}

/* on_anim_event(self, id, name|nil, f0, f1, i0) — animation frame-event
 * dispatch.  Mirrors jce_script_call_message's instance-method dispatch
 * exactly, but with the fixed method name "on_anim_event" and the 5-scalar
 * (id, name, f0, f1, i0) animation payload.  No-op if the receiver defines no
 * on_anim_event (so receivers opt in by simply declaring the method); a NULL /
 * empty name pushes nil for that parameter. */
void jce_script_call_anim_event(JceScript *s, JceScriptInstance inst,
                                uint32_t id, const char *name,
                                float f0, float f1, int i0)
{
    if (!s || inst == 0) return;
    lua_State *L = s->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, (lua_Integer)inst);   /* [ inst ] */
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    lua_getfield(L, -1, "on_anim_event");                   /* [ inst, fn ] (via meta) */
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return; }  /* missing handler: clean no-op */
    lua_pushvalue(L, -2);                                   /* [ inst, fn, self ] */
    lua_pushinteger(L, (lua_Integer)id);                   /* [ ..., id ] */
    if (name && name[0]) lua_pushstring(L, name);          /* [ ..., name ] */
    else                 lua_pushnil(L);                    /* [ ..., nil ] */
    lua_pushnumber(L, (lua_Number)f0);                     /* [ ..., f0 ] */
    lua_pushnumber(L, (lua_Number)f1);                     /* [ ..., f1 ] */
    lua_pushinteger(L, (lua_Integer)i0);                   /* [ ..., i0 ] */
    if (lua_pcall(L, 6, 0, 0) != LUA_OK) {                 /* pops fn + 6 args */
        const char *err = lua_tostring(L, -1);
        if (s->have_host && s->host.log) {
            char buf[512];
            snprintf(buf, sizeof(buf), "on_anim_event error: %s", err ? err : "?");
            s->host.log(s->host.user, buf);
        }
        LOG_ERROR(LOG_TAG, "on_anim_event error: %s", err ? err : "?");
        lua_pop(L, 1);                                      /* drop error */
    }
    lua_pop(L, 1);                                          /* drop inst */
}

bool jce_script_call_named(JceScript *s, const char *fn_name,
                           JceScriptEntity arg_entity)
{
    if (!s || !s->L || !fn_name || !fn_name[0]) return false;
    lua_State *L = s->L;
    lua_getglobal(L, fn_name);                              /* [ fn|nil ] */
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return false; }
    lua_pushinteger(L, (lua_Integer)arg_entity);           /* [ fn, arg ] */
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {                 /* pops fn + arg */
        const char *err = lua_tostring(L, -1);
        if (s->have_host && s->host.log) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s error: %s", fn_name, err ? err : "?");
            s->host.log(s->host.user, buf);
        }
        LOG_ERROR(LOG_TAG, "%s error: %s", fn_name, err ? err : "?");
        lua_pop(L, 1);                                      /* drop error */
    }
    return true;   /* a function existed and was invoked (error caught above) */
}

bool jce_script_call_named_num(JceScript *s, const char *fn_name,
                               JceScriptEntity arg_entity, double value)
{
    if (!s || !s->L || !fn_name || !fn_name[0]) return false;
    lua_State *L = s->L;
    lua_getglobal(L, fn_name);                              /* [ fn|nil ] */
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return false; }
    lua_pushinteger(L, (lua_Integer)arg_entity);           /* [ fn, ent ] */
    lua_pushnumber(L, (lua_Number)value);                  /* [ fn, ent, v ] */
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {                 /* pops fn + 2 args */
        const char *err = lua_tostring(L, -1);
        if (s->have_host && s->host.log) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s error: %s", fn_name, err ? err : "?");
            s->host.log(s->host.user, buf);
        }
        LOG_ERROR(LOG_TAG, "%s error: %s", fn_name, err ? err : "?");
        lua_pop(L, 1);                                      /* drop error */
    }
    return true;
}

bool jce_script_call_named_str(JceScript *s, const char *fn_name,
                               JceScriptEntity arg_entity, const char *str)
{
    if (!s || !s->L || !fn_name || !fn_name[0]) return false;
    lua_State *L = s->L;
    lua_getglobal(L, fn_name);                              /* [ fn|nil ] */
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return false; }
    lua_pushinteger(L, (lua_Integer)arg_entity);           /* [ fn, ent ] */
    if (str) lua_pushstring(L, str);                       /* [ fn, ent, s ] */
    else     lua_pushnil(L);                               /* [ fn, ent, nil ] */
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {                 /* pops fn + 2 args */
        const char *err = lua_tostring(L, -1);
        if (s->have_host && s->host.log) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s error: %s", fn_name, err ? err : "?");
            s->host.log(s->host.user, buf);
        }
        LOG_ERROR(LOG_TAG, "%s error: %s", fn_name, err ? err : "?");
        lua_pop(L, 1);                                      /* drop error */
    }
    return true;
}

int jce_script_instance_count(const JceScript *s)
{
    return s ? s->instance_count : 0;
}

void jce_script_update_coroutines(JceScript *s, float dt)
{
    if (!s || !s->L) return;
    /* Snapshot the bound so coroutines started DURING a resume this tick (which
     * already ran to their first wait) aren't double-advanced this same tick. */
    int n = s->coro_count;
    for (int i = 0; i < n; ++i) {
        int ref = s->coros[i].thread_ref;
        if (ref == LUA_NOREF) continue;
        s->coros[i].remaining -= dt;
        if (s->coros[i].remaining > 0.0f) continue;

        lua_rawgeti(s->L, LUA_REGISTRYINDEX, ref);   /* [ thread ] */
        lua_State *co = lua_tothread(s->L, -1);
        lua_pop(s->L, 1);
        if (!co) { luaL_unref(s->L, LUA_REGISTRYINDEX, ref); s->coros[i].thread_ref = LUA_NOREF; continue; }

        int nres = 0;
        script_arm_watchdog(co);                     /* hook on the coroutine thread */
        int st = lua_resume(co, s->L, 0, &nres);     /* run to next yield/return */
        if (st == LUA_YIELD) {
            float wait = (nres >= 1) ? (float)lua_tonumber(co, -1) : 0.0f;
            lua_pop(co, nres);
            s->coros[i].remaining = wait < 0.0f ? 0.0f : wait;
        } else {
            if (st != LUA_OK) {
                const char *err = lua_tostring(co, -1);
                if (s->have_host && s->host.log) {
                    char buf[512];
                    snprintf(buf, sizeof(buf), "coroutine error: %s", err ? err : "?");
                    s->host.log(s->host.user, buf);
                }
                LOG_ERROR(LOG_TAG, "coroutine error: %s", err ? err : "?");
            }
            luaL_unref(s->L, LUA_REGISTRYINDEX, ref);
            s->coros[i].thread_ref = LUA_NOREF;       /* completed/errored */
        }
    }
}

/* ── Hot-reload ───────────────────────────────────────────────────────────── */

JceScriptModule jce_script_compile_module(JceScript *s, const char *name,
                                          const char *source, size_t len)
{
    if (!s || !source) return 0;
    lua_State *L = s->L;
    const char *chunkname = name ? name : "=reload";
    if (luaL_loadbuffer(L, source, len, chunkname) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        if (s->have_host && s->host.log) {
            char buf[512];
            snprintf(buf, sizeof(buf), "reload compile error (%s): %s",
                     chunkname, err ? err : "?");
            s->host.log(s->host.user, buf);
        }
        LOG_ERROR(LOG_TAG, "reload compile error (%s): %s", chunkname, err ? err : "?");
        lua_pop(L, 1);
        return 0;
    }
    script_arm_watchdog(L);
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        LOG_ERROR(LOG_TAG, "reload run error (%s): %s", chunkname, err ? err : "?");
        lua_pop(L, 1);
        return 0;
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        LOG_ERROR(LOG_TAG, "reload: '%s' did not return a table", chunkname);
        return 0;
    }
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);   /* pops module, refs it */
    return (JceScriptModule)ref;
}

void jce_script_rebind_instance(JceScript *s, JceScriptInstance inst,
                                JceScriptModule mod)
{
    if (!s || inst == 0 || mod == 0) return;
    lua_State *L = s->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, (lua_Integer)inst);   /* [ inst ] */
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    if (!lua_getmetatable(L, -1)) { lua_pop(L, 1); return; } /* [ inst, meta ] */
    lua_rawgeti(L, LUA_REGISTRYINDEX, (lua_Integer)mod);     /* [ inst, meta, module ] */
    lua_setfield(L, -2, "__index");                          /* meta.__index = module */
    lua_pop(L, 2);                                           /* [] */
}

void jce_script_release_module(JceScript *s, JceScriptModule mod)
{
    if (!s || mod == 0) return;
    luaL_unref(s->L, LUA_REGISTRYINDEX, (int)mod);
}
