/* jce_script_api.gen.c -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_c_abi.py --write
 *
 * One forwarder per manifest `expose` entry.  Every one of them guards
 * the host member before calling it, for the reason
 * tests/middleware/script/test_jce_script_host_abi.c exists:
 * jce_script_api_open copies min(host_size, sizeof) over a zeroed table,
 * so a member the caller's shorter header did not have stays NULL, and
 * calling it unguarded jumps through whatever followed the caller's
 * object.
 *
 * The guard has exactly ONE spelling here -- `!api || !api->host.<member>`
 * -- and the gate's unit tests assert it appears in every emitted
 * function, naming the member the manifest names.
 */

#include "jce_script_api_internal.h"

#include <string.h>

/* jce_script_api_get_position -> host.get_position (fallible_out) */
JCE_SCRIPT_API bool jce_script_api_get_position(JceScriptApi *api,
                                                JceScriptEntity e,
                                                float out_xyz[3])
{
    if (!api || !api->host.get_position) {
        if (out_xyz) memset(out_xyz, 0, 3 * sizeof out_xyz[0]);
        return false;
    }
    return api->host.get_position(api->host.user, e, out_xyz);
}

/* jce_script_api_set_position -> host.set_position (void_call) */
JCE_SCRIPT_API void jce_script_api_set_position(JceScriptApi *api,
                                                JceScriptEntity e, float x,
                                                float y, float z)
{
    if (!api || !api->host.set_position) {
        return;
    }
    api->host.set_position(api->host.user, e, x, y, z);
}

/* jce_script_api_get_rotation -> host.get_rotation (fallible_out) */
JCE_SCRIPT_API bool jce_script_api_get_rotation(JceScriptApi *api,
                                                JceScriptEntity e,
                                                float out_euler_deg[3])
{
    if (!api || !api->host.get_rotation) {
        if (out_euler_deg) memset(out_euler_deg, 0, 3 * sizeof out_euler_deg[0]);
        return false;
    }
    return api->host.get_rotation(api->host.user, e, out_euler_deg);
}

/* jce_script_api_set_rotation -> host.set_rotation (void_call) */
JCE_SCRIPT_API void jce_script_api_set_rotation(JceScriptApi *api,
                                                JceScriptEntity e, float x,
                                                float y, float z)
{
    if (!api || !api->host.set_rotation) {
        return;
    }
    api->host.set_rotation(api->host.user, e, x, y, z);
}

/* jce_script_api_get_scale -> host.get_scale (fallible_out) */
JCE_SCRIPT_API bool jce_script_api_get_scale(JceScriptApi *api,
                                             JceScriptEntity e,
                                             float out_xyz[3])
{
    if (!api || !api->host.get_scale) {
        if (out_xyz) memset(out_xyz, 0, 3 * sizeof out_xyz[0]);
        return false;
    }
    return api->host.get_scale(api->host.user, e, out_xyz);
}

/* jce_script_api_get_world_position -> host.get_world_position (fallible_out) */
JCE_SCRIPT_API bool jce_script_api_get_world_position(JceScriptApi *api,
                                                      JceScriptEntity e,
                                                      float out_xyz[3])
{
    if (!api || !api->host.get_world_position) {
        if (out_xyz) memset(out_xyz, 0, 3 * sizeof out_xyz[0]);
        return false;
    }
    return api->host.get_world_position(api->host.user, e, out_xyz);
}

/* jce_script_api_set_scale -> host.set_scale (void_call) */
JCE_SCRIPT_API void jce_script_api_set_scale(JceScriptApi *api,
                                             JceScriptEntity e, float x,
                                             float y, float z)
{
    if (!api || !api->host.set_scale) {
        return;
    }
    api->host.set_scale(api->host.user, e, x, y, z);
}

/* jce_script_api_set_parent -> host.set_parent (value_return) */
JCE_SCRIPT_API bool jce_script_api_set_parent(JceScriptApi *api,
                                              JceScriptEntity child,
                                              JceScriptEntity parent,
                                              bool preserve_world)
{
    if (!api || !api->host.set_parent) {
        return false;
    }
    return api->host.set_parent(api->host.user, child, parent, preserve_world);
}

/* jce_script_api_get_parent -> host.get_parent (value_return) */
JCE_SCRIPT_API JceScriptEntity jce_script_api_get_parent(JceScriptApi *api,
                                                         JceScriptEntity child)
{
    if (!api || !api->host.get_parent) {
        return 0;
    }
    return api->host.get_parent(api->host.user, child);
}

/* jce_script_api_is_key_down -> host.is_key_down (value_return) */
JCE_SCRIPT_API bool jce_script_api_is_key_down(JceScriptApi *api, int keycode)
{
    if (!api || !api->host.is_key_down) {
        return false;
    }
    return api->host.is_key_down(api->host.user, keycode);
}

/* jce_script_api_find_with_tag -> host.find_with_tag (value_return) */
JCE_SCRIPT_API JceScriptEntity jce_script_api_find_with_tag(JceScriptApi *api,
                                                            const char * tag)
{
    if (!api || !api->host.find_with_tag) {
        return 0;
    }
    return api->host.find_with_tag(api->host.user, tag);
}

/* jce_script_api_destroy -> host.destroy_entity (void_call) */
JCE_SCRIPT_API void jce_script_api_destroy(JceScriptApi *api,
                                           JceScriptEntity e)
{
    if (!api || !api->host.destroy_entity) {
        return;
    }
    api->host.destroy_entity(api->host.user, e);
}

/* jce_script_api_spawn -> host.spawn (value_return) */
JCE_SCRIPT_API JceScriptEntity jce_script_api_spawn(JceScriptApi *api,
                                                    const char * prefab_path,
                                                    float x, float y, float z)
{
    if (!api || !api->host.spawn) {
        return 0;
    }
    return api->host.spawn(api->host.user, prefab_path, x, y, z);
}

/* jce_script_api_move_axis -> host.move_axis (void_out_array) */
JCE_SCRIPT_API void jce_script_api_move_axis(JceScriptApi *api,
                                             float out_xz[2])
{
    if (!api || !api->host.move_axis) {
        if (out_xz) memset(out_xz, 0, 2 * sizeof out_xz[0]);
        return;
    }
    api->host.move_axis(api->host.user, out_xz);
}

/* jce_script_api_jump_pressed -> host.input_button (value_return) */
JCE_SCRIPT_API bool jce_script_api_jump_pressed(JceScriptApi *api)
{
    if (!api || !api->host.input_button) {
        return false;
    }
    return api->host.input_button(api->host.user, (int)0);
}

/* jce_script_api_sprint -> host.input_button (value_return) */
JCE_SCRIPT_API bool jce_script_api_sprint(JceScriptApi *api)
{
    if (!api || !api->host.input_button) {
        return false;
    }
    return api->host.input_button(api->host.user, (int)1);
}

/* jce_script_api_attack_pressed -> host.input_button (value_return) */
JCE_SCRIPT_API bool jce_script_api_attack_pressed(JceScriptApi *api)
{
    if (!api || !api->host.input_button) {
        return false;
    }
    return api->host.input_button(api->host.user, (int)2);
}

/* jce_script_api_set_time_scale -> host.set_time_scale (void_call) */
JCE_SCRIPT_API void jce_script_api_set_time_scale(JceScriptApi *api,
                                                  float scale)
{
    if (!api || !api->host.set_time_scale) {
        return;
    }
    api->host.set_time_scale(api->host.user, scale);
}

/* jce_script_api_pause -> host.set_paused (void_call) */
JCE_SCRIPT_API void jce_script_api_pause(JceScriptApi *api, bool paused)
{
    if (!api || !api->host.set_paused) {
        return;
    }
    api->host.set_paused(api->host.user, paused);
}

/* jce_script_api_shake_camera -> host.shake_camera (void_call) */
JCE_SCRIPT_API void jce_script_api_shake_camera(JceScriptApi *api,
                                                float amount)
{
    if (!api || !api->host.shake_camera) {
        return;
    }
    api->host.shake_camera(api->host.user, amount);
}

/* jce_script_api_music_set_intensity -> host.music_set_intensity (void_call) */
JCE_SCRIPT_API void jce_script_api_music_set_intensity(JceScriptApi *api,
                                                       float intensity)
{
    if (!api || !api->host.music_set_intensity) {
        return;
    }
    api->host.music_set_intensity(api->host.user, intensity);
}

/* jce_script_api_music_get_intensity -> host.music_get_intensity (value_return) */
JCE_SCRIPT_API float jce_script_api_music_get_intensity(JceScriptApi *api)
{
    if (!api || !api->host.music_get_intensity) {
        return 0.0f;
    }
    return api->host.music_get_intensity(api->host.user);
}

/* jce_script_api_music_request_transition -> host.music_request_transition (value_return) */
JCE_SCRIPT_API float jce_script_api_music_request_transition(JceScriptApi *api,
                                                             int to_segment)
{
    if (!api || !api->host.music_request_transition) {
        return -1.0f;
    }
    return api->host.music_request_transition(api->host.user, to_segment);
}

/* jce_script_api_gas_activate -> host.gas_activate (value_return) */
JCE_SCRIPT_API bool jce_script_api_gas_activate(JceScriptApi *api,
                                                JceScriptEntity e,
                                                uint32_t ability_id)
{
    if (!api || !api->host.gas_activate) {
        return false;
    }
    return api->host.gas_activate(api->host.user, e, ability_id);
}

/* jce_script_api_gas_get -> host.gas_get (fallible_out) */
JCE_SCRIPT_API bool jce_script_api_gas_get(JceScriptApi *api,
                                           JceScriptEntity e,
                                           const char * attr_name,
                                           float *out_value)
{
    if (!api || !api->host.gas_get) {
        if (out_value) memset(out_value, 0, sizeof *out_value);
        return false;
    }
    return api->host.gas_get(api->host.user, e, attr_name, out_value);
}

/* jce_script_api_gas_apply -> host.gas_apply (value_return) */
JCE_SCRIPT_API bool jce_script_api_gas_apply(JceScriptApi *api,
                                             JceScriptEntity e,
                                             const char * attr_name, int op,
                                             float magnitude,
                                             float duration_seconds)
{
    if (!api || !api->host.gas_apply) {
        return false;
    }
    return api->host.gas_apply(api->host.user, e, attr_name, op, magnitude, duration_seconds);
}

/* jce_script_api_raycast -> host.raycast (fallible_out) */
JCE_SCRIPT_API bool jce_script_api_raycast(JceScriptApi *api,
                                           const float origin[3],
                                           const float dir[3], float max_dist,
                                           JceScriptRaycastHit *out)
{
    if (!api || !api->host.raycast) {
        if (out) memset(out, 0, sizeof *out);
        return false;
    }
    return api->host.raycast(api->host.user, origin, dir, max_dist, out);
}

/* jce_script_api_raycast_filtered -> host.raycast_filtered (fallible_out) */
JCE_SCRIPT_API bool jce_script_api_raycast_filtered(JceScriptApi *api,
                                                    const float origin[3],
                                                    const float dir[3],
                                                    float max_dist,
                                                    uint32_t layer_mask,
                                                    bool hit_triggers,
                                                    JceScriptRaycastHit *out)
{
    if (!api || !api->host.raycast_filtered) {
        if (out) memset(out, 0, sizeof *out);
        return false;
    }
    return api->host.raycast_filtered(api->host.user, origin, dir, max_dist, layer_mask, hit_triggers, out);
}

/* jce_script_api_raycast_all -> host.raycast_all (entity_table) */
JCE_SCRIPT_API int jce_script_api_raycast_all(JceScriptApi *api,
                                              const float origin[3],
                                              const float dir[3],
                                              float max_dist,
                                              uint32_t layer_mask,
                                              bool hit_triggers,
                                              JceScriptEntity *out, int max)
{
    if (!api || !api->host.raycast_all) {
        if (out) memset(out, 0, sizeof *out);
        return 0;
    }
    return api->host.raycast_all(api->host.user, origin, dir, max_dist, layer_mask, hit_triggers, out, max);
}

/* jce_script_api_apply_impulse -> host.apply_impulse (void_call) */
JCE_SCRIPT_API void jce_script_api_apply_impulse(JceScriptApi *api,
                                                 JceScriptEntity e, float x,
                                                 float y, float z)
{
    if (!api || !api->host.apply_impulse) {
        return;
    }
    api->host.apply_impulse(api->host.user, e, x, y, z);
}

/* jce_script_api_set_velocity -> host.set_velocity (void_call) */
JCE_SCRIPT_API void jce_script_api_set_velocity(JceScriptApi *api,
                                                JceScriptEntity e, float x,
                                                float y, float z)
{
    if (!api || !api->host.set_velocity) {
        return;
    }
    api->host.set_velocity(api->host.user, e, x, y, z);
}

/* jce_script_api_anim_set_float -> host.anim_set_float (void_call) */
JCE_SCRIPT_API void jce_script_api_anim_set_float(JceScriptApi *api,
                                                  JceScriptEntity e,
                                                  const char * name, float v)
{
    if (!api || !api->host.anim_set_float) {
        return;
    }
    api->host.anim_set_float(api->host.user, e, name, v);
}

/* jce_script_api_anim_set_int -> host.anim_set_int (void_call) */
JCE_SCRIPT_API void jce_script_api_anim_set_int(JceScriptApi *api,
                                                JceScriptEntity e,
                                                const char * name, int v)
{
    if (!api || !api->host.anim_set_int) {
        return;
    }
    api->host.anim_set_int(api->host.user, e, name, v);
}

/* jce_script_api_anim_set_bool -> host.anim_set_bool (void_call) */
JCE_SCRIPT_API void jce_script_api_anim_set_bool(JceScriptApi *api,
                                                 JceScriptEntity e,
                                                 const char * name, bool v)
{
    if (!api || !api->host.anim_set_bool) {
        return;
    }
    api->host.anim_set_bool(api->host.user, e, name, v);
}

/* jce_script_api_anim_set_trigger -> host.anim_set_trigger (void_call) */
JCE_SCRIPT_API void jce_script_api_anim_set_trigger(JceScriptApi *api,
                                                    JceScriptEntity e,
                                                    const char * name)
{
    if (!api || !api->host.anim_set_trigger) {
        return;
    }
    api->host.anim_set_trigger(api->host.user, e, name);
}

/* jce_script_api_is_action_down -> host.action_down (value_return) */
JCE_SCRIPT_API bool jce_script_api_is_action_down(JceScriptApi *api,
                                                  const char * name)
{
    if (!api || !api->host.action_down) {
        return false;
    }
    return api->host.action_down(api->host.user, name);
}

/* jce_script_api_is_action_pressed -> host.action_pressed (value_return) */
JCE_SCRIPT_API bool jce_script_api_is_action_pressed(JceScriptApi *api,
                                                     const char * name)
{
    if (!api || !api->host.action_pressed) {
        return false;
    }
    return api->host.action_pressed(api->host.user, name);
}

/* jce_script_api_get_axis -> host.action_axis (value_return) */
JCE_SCRIPT_API float jce_script_api_get_axis(JceScriptApi *api,
                                             const char * name)
{
    if (!api || !api->host.action_axis) {
        return 0.0f;
    }
    return api->host.action_axis(api->host.user, name);
}

/* jce_script_api_get_pointer_delta -> host.pointer_delta (void_out_array) */
JCE_SCRIPT_API void jce_script_api_get_pointer_delta(JceScriptApi *api,
                                                     float out_xy[2])
{
    if (!api || !api->host.pointer_delta) {
        if (out_xy) memset(out_xy, 0, 2 * sizeof out_xy[0]);
        return;
    }
    api->host.pointer_delta(api->host.user, out_xy);
}

/* jce_script_api_get_pointer_wheel -> host.pointer_wheel (value_return) */
JCE_SCRIPT_API float jce_script_api_get_pointer_wheel(JceScriptApi *api)
{
    if (!api || !api->host.pointer_wheel) {
        return 0.0f;
    }
    return api->host.pointer_wheel(api->host.user);
}

/* jce_script_api_is_pointer_down -> host.pointer_button (value_return) */
JCE_SCRIPT_API bool jce_script_api_is_pointer_down(JceScriptApi *api,
                                                   int button)
{
    if (!api || !api->host.pointer_button) {
        return false;
    }
    return api->host.pointer_button(api->host.user, button);
}

/* jce_script_api_get_touch_count -> host.touch_count (value_return) */
JCE_SCRIPT_API int jce_script_api_get_touch_count(JceScriptApi *api)
{
    int v;

    if (!api || !api->host.touch_count) {
        return 0;
    }
    v = api->host.touch_count(api->host.user);
    if (v < 0)
        v = 0;
    return v;
}

/* jce_script_api_get_touch -> host.touch_get (fallible_out) */
JCE_SCRIPT_API bool jce_script_api_get_touch(JceScriptApi *api, int index,
                                             uint64_t *id, float *x, float *y,
                                             float *pressure)
{
    if (!api || !api->host.touch_get) {
        if (id) memset(id, 0, sizeof *id);
        if (x) memset(x, 0, sizeof *x);
        if (y) memset(y, 0, sizeof *y);
        if (pressure) memset(pressure, 0, sizeof *pressure);
        return false;
    }
    return api->host.touch_get(api->host.user, index, id, x, y, pressure);
}

/* jce_script_api_tr -> host.loc_translate (value_return) */
JCE_SCRIPT_API const char *jce_script_api_tr(JceScriptApi *api,
                                             const char * key)
{
    if (!api || !api->host.loc_translate) {
        return key;
    }
    return api->host.loc_translate(api->host.user, key);
}

/* jce_script_api_get_locale -> host.loc_get_locale (value_return) */
JCE_SCRIPT_API const char *jce_script_api_get_locale(JceScriptApi *api)
{
    if (!api || !api->host.loc_get_locale) {
        return NULL;
    }
    return api->host.loc_get_locale(api->host.user);
}

/* jce_script_api_set_locale -> host.loc_set_locale (void_call) */
JCE_SCRIPT_API void jce_script_api_set_locale(JceScriptApi *api,
                                              const char * locale)
{
    if (!api || !api->host.loc_set_locale) {
        return;
    }
    api->host.loc_set_locale(api->host.user, locale);
}

/* jce_script_api_get_velocity -> host.get_velocity (fallible_out) */
JCE_SCRIPT_API bool jce_script_api_get_velocity(JceScriptApi *api,
                                                JceScriptEntity e,
                                                float out[3])
{
    if (!api || !api->host.get_velocity) {
        if (out) memset(out, 0, 3 * sizeof out[0]);
        return false;
    }
    return api->host.get_velocity(api->host.user, e, out);
}

/* jce_script_api_vehicle_set_input -> host.vehicle_set_input (void_call) */
JCE_SCRIPT_API void jce_script_api_vehicle_set_input(JceScriptApi *api,
                                                     JceScriptEntity e,
                                                     float throttle,
                                                     float brake, float steer)
{
    if (!api || !api->host.vehicle_set_input) {
        return;
    }
    api->host.vehicle_set_input(api->host.user, e, throttle, brake, steer);
}

/* jce_script_api_vehicle_get_speed -> host.vehicle_get_speed (value_return) */
JCE_SCRIPT_API float jce_script_api_vehicle_get_speed(JceScriptApi *api,
                                                      JceScriptEntity e)
{
    if (!api || !api->host.vehicle_get_speed) {
        return 0.0f;
    }
    return api->host.vehicle_get_speed(api->host.user, e);
}

/* jce_script_api_get_move -> host.get_move (void_out_array) */
JCE_SCRIPT_API void jce_script_api_get_move(JceScriptApi *api, float out[3])
{
    if (!api || !api->host.get_move) {
        if (out) memset(out, 0, 3 * sizeof out[0]);
        return;
    }
    api->host.get_move(api->host.user, out);
}

/* jce_script_api_ui_get_slider -> host.ui_get_slider (fallible_out) */
JCE_SCRIPT_API bool jce_script_api_ui_get_slider(JceScriptApi *api,
                                                 JceScriptEntity e, float *out)
{
    if (!api || !api->host.ui_get_slider) {
        if (out) memset(out, 0, sizeof *out);
        return false;
    }
    return api->host.ui_get_slider(api->host.user, e, out);
}

/* jce_script_api_ui_set_slider -> host.ui_set_slider (void_call) */
JCE_SCRIPT_API void jce_script_api_ui_set_slider(JceScriptApi *api,
                                                 JceScriptEntity e, float v)
{
    if (!api || !api->host.ui_set_slider) {
        return;
    }
    api->host.ui_set_slider(api->host.user, e, v);
}

/* jce_script_api_ui_get_progress -> host.ui_get_progress (fallible_out) */
JCE_SCRIPT_API bool jce_script_api_ui_get_progress(JceScriptApi *api,
                                                   JceScriptEntity e,
                                                   float *out)
{
    if (!api || !api->host.ui_get_progress) {
        if (out) memset(out, 0, sizeof *out);
        return false;
    }
    return api->host.ui_get_progress(api->host.user, e, out);
}

/* jce_script_api_ui_set_progress -> host.ui_set_progress (void_call) */
JCE_SCRIPT_API void jce_script_api_ui_set_progress(JceScriptApi *api,
                                                   JceScriptEntity e, float v)
{
    if (!api || !api->host.ui_set_progress) {
        return;
    }
    api->host.ui_set_progress(api->host.user, e, v);
}

/* jce_script_api_ui_get_toggle -> host.ui_get_toggle (fallible_out) */
JCE_SCRIPT_API bool jce_script_api_ui_get_toggle(JceScriptApi *api,
                                                 JceScriptEntity e, bool *out)
{
    if (!api || !api->host.ui_get_toggle) {
        if (out) memset(out, 0, sizeof *out);
        return false;
    }
    return api->host.ui_get_toggle(api->host.user, e, out);
}

/* jce_script_api_ui_set_toggle -> host.ui_set_toggle (void_call) */
JCE_SCRIPT_API void jce_script_api_ui_set_toggle(JceScriptApi *api,
                                                 JceScriptEntity e, bool v)
{
    if (!api || !api->host.ui_set_toggle) {
        return;
    }
    api->host.ui_set_toggle(api->host.user, e, v);
}

/* jce_script_api_ui_set_text -> host.ui_set_text (void_call) */
JCE_SCRIPT_API void jce_script_api_ui_set_text(JceScriptApi *api,
                                               JceScriptEntity e,
                                               const char * txt)
{
    if (!api || !api->host.ui_set_text) {
        return;
    }
    api->host.ui_set_text(api->host.user, e, txt);
}

/* jce_script_api_send_message -> host.send_message (void_call) */
JCE_SCRIPT_API void jce_script_api_send_message(JceScriptApi *api,
                                                JceScriptEntity target,
                                                const char * msg,
                                                double number_arg,
                                                const char * str_arg)
{
    if (!api || !api->host.send_message) {
        return;
    }
    api->host.send_message(api->host.user, target, msg, number_arg, str_arg);
}

/* jce_script_api_broadcast -> host.broadcast (void_call) */
JCE_SCRIPT_API void jce_script_api_broadcast(JceScriptApi *api,
                                             const char * msg,
                                             double number_arg,
                                             const char * str_arg)
{
    if (!api || !api->host.broadcast) {
        return;
    }
    api->host.broadcast(api->host.user, msg, number_arg, str_arg);
}

/* jce_script_api_has_component -> host.has_component (value_return) */
JCE_SCRIPT_API bool jce_script_api_has_component(JceScriptApi *api,
                                                 JceScriptEntity e,
                                                 const char * comp_name)
{
    if (!api || !api->host.has_component) {
        return false;
    }
    return api->host.has_component(api->host.user, e, comp_name);
}

/* jce_script_api_is_component_enabled -> host.is_component_enabled (value_return) */
JCE_SCRIPT_API bool jce_script_api_is_component_enabled(JceScriptApi *api,
                                                        JceScriptEntity e,
                                                        const char * comp_name)
{
    if (!api || !api->host.is_component_enabled) {
        return false;
    }
    return api->host.is_component_enabled(api->host.user, e, comp_name);
}

/* jce_script_api_set_component_enabled -> host.set_component_enabled (void_call) */
JCE_SCRIPT_API void jce_script_api_set_component_enabled(JceScriptApi *api,
                                                         JceScriptEntity e,
                                                         const char * comp_name,
                                                         bool on)
{
    if (!api || !api->host.set_component_enabled) {
        return;
    }
    api->host.set_component_enabled(api->host.user, e, comp_name, on);
}

/* jce_script_api_net_is_server -> host.net_is_server (value_return) */
JCE_SCRIPT_API bool jce_script_api_net_is_server(JceScriptApi *api)
{
    if (!api || !api->host.net_is_server) {
        return false;
    }
    return api->host.net_is_server(api->host.user);
}

/* jce_script_api_net_is_client -> host.net_is_client (value_return) */
JCE_SCRIPT_API bool jce_script_api_net_is_client(JceScriptApi *api)
{
    if (!api || !api->host.net_is_client) {
        return false;
    }
    return api->host.net_is_client(api->host.user);
}

/* jce_script_api_net_spawn -> host.net_spawn (value_return) */
JCE_SCRIPT_API JceScriptEntity jce_script_api_net_spawn(JceScriptApi *api,
                                                        const char * prefab_path,
                                                        float x, float y,
                                                        float z)
{
    if (!api || !api->host.net_spawn) {
        return 0;
    }
    return api->host.net_spawn(api->host.user, prefab_path, x, y, z);
}

/* jce_script_api_rpc_send -> host.rpc_send (value_return) */
JCE_SCRIPT_API bool jce_script_api_rpc_send(JceScriptApi *api,
                                            JceScriptEntity e,
                                            const char * event, int target,
                                            const char * payload)
{
    if (!api || !api->host.rpc_send) {
        return false;
    }
    return api->host.rpc_send(api->host.user, e, event, target, payload);
}

/* jce_script_api_particle_burst -> host.particle_burst (void_call) */
JCE_SCRIPT_API void jce_script_api_particle_burst(JceScriptApi *api,
                                                  JceScriptEntity e, int count)
{
    if (!api || !api->host.particle_burst) {
        return;
    }
    api->host.particle_burst(api->host.user, e, count);
}

/* jce_script_api_particle_set_emitting -> host.particle_set_emitting (void_call) */
JCE_SCRIPT_API void jce_script_api_particle_set_emitting(JceScriptApi *api,
                                                         JceScriptEntity e,
                                                         bool on)
{
    if (!api || !api->host.particle_set_emitting) {
        return;
    }
    api->host.particle_set_emitting(api->host.user, e, on);
}

/* jce_script_api_particle_set_color -> host.particle_set_color (void_call) */
JCE_SCRIPT_API void jce_script_api_particle_set_color(JceScriptApi *api,
                                                      JceScriptEntity e,
                                                      float r, float g,
                                                      float b)
{
    if (!api || !api->host.particle_set_color) {
        return;
    }
    api->host.particle_set_color(api->host.user, e, r, g, b);
}

/* jce_script_api_find_by_name -> host.find_by_name (first_and_count) */
JCE_SCRIPT_API int jce_script_api_find_by_name(JceScriptApi *api,
                                               const char * name,
                                               JceScriptEntity *out, int max)
{
    if (!api || !api->host.find_by_name) {
        if (out) memset(out, 0, sizeof *out);
        return 0;
    }
    return api->host.find_by_name(api->host.user, name, out, max);
}

/* jce_script_api_find_by_prefix -> host.find_by_prefix (entity_table) */
JCE_SCRIPT_API int jce_script_api_find_by_prefix(JceScriptApi *api,
                                                 const char * prefix,
                                                 JceScriptEntity *out, int max)
{
    if (!api || !api->host.find_by_prefix) {
        if (out) memset(out, 0, sizeof *out);
        return 0;
    }
    return api->host.find_by_prefix(api->host.user, prefix, out, max);
}

/* jce_script_api_comp_get -> host.comp_get_json (owned_string_release) */
JCE_SCRIPT_API int jce_script_api_comp_get(JceScriptApi *api,
                                           JceScriptEntity e,
                                           const char * type, char *out,
                                           int out_cap)
{
    char *s;
    size_t n;

    if (out && out_cap > 0)
        out[0] = '\0';
    if (!api || !api->host.comp_get_json || !api->host.json_free)
        return -1;
    s = api->host.comp_get_json(api->host.user, e, type);
    if (!s)
        return -1;
    n = strlen(s);
    if (out && out_cap > 0) {
        size_t room = (size_t)out_cap - 1u;
        size_t take = (n < room) ? n : room;
        memcpy(out, s, take);
        out[take] = '\0';
    }
    api->host.json_free(api->host.user, s);
    return (int)n;
}

/* jce_script_api_comp_set -> host.comp_set_json (value_return) */
JCE_SCRIPT_API bool jce_script_api_comp_set(JceScriptApi *api,
                                            JceScriptEntity e,
                                            const char * type,
                                            const char * json)
{
    if (!api || !api->host.comp_set_json) {
        return false;
    }
    return api->host.comp_set_json(api->host.user, e, type, json);
}

/* jce_script_api_render_get -> host.render_get_json (owned_string_release) */
JCE_SCRIPT_API int jce_script_api_render_get(JceScriptApi *api, char *out,
                                             int out_cap)
{
    char *s;
    size_t n;

    if (out && out_cap > 0)
        out[0] = '\0';
    if (!api || !api->host.render_get_json || !api->host.json_free)
        return -1;
    s = api->host.render_get_json(api->host.user);
    if (!s)
        return -1;
    n = strlen(s);
    if (out && out_cap > 0) {
        size_t room = (size_t)out_cap - 1u;
        size_t take = (n < room) ? n : room;
        memcpy(out, s, take);
        out[take] = '\0';
    }
    api->host.json_free(api->host.user, s);
    return (int)n;
}

/* jce_script_api_render_set -> host.render_set_json (value_return) */
JCE_SCRIPT_API bool jce_script_api_render_set(JceScriptApi *api,
                                              const char * json)
{
    if (!api || !api->host.render_set_json) {
        return false;
    }
    return api->host.render_set_json(api->host.user, json);
}

/* jce_script_api_audio_set_volume -> host.audio_set_volume (void_call) */
JCE_SCRIPT_API void jce_script_api_audio_set_volume(JceScriptApi *api,
                                                    JceScriptEntity e,
                                                    float volume)
{
    if (!api || !api->host.audio_set_volume) {
        return;
    }
    api->host.audio_set_volume(api->host.user, e, volume);
}

/* jce_script_api_ui_get_dropdown -> host.ui_get_dropdown (fallible_out) */
JCE_SCRIPT_API bool jce_script_api_ui_get_dropdown(JceScriptApi *api,
                                                   JceScriptEntity e, int *out)
{
    if (!api || !api->host.ui_get_dropdown) {
        if (out) memset(out, 0, sizeof *out);
        return false;
    }
    return api->host.ui_get_dropdown(api->host.user, e, out);
}

/* jce_script_api_ui_set_dropdown -> host.ui_set_dropdown (void_call) */
JCE_SCRIPT_API void jce_script_api_ui_set_dropdown(JceScriptApi *api,
                                                   JceScriptEntity e,
                                                   int index)
{
    if (!api || !api->host.ui_set_dropdown) {
        return;
    }
    api->host.ui_set_dropdown(api->host.user, e, index);
}

/* jce_script_api_ui_get_input_text -> host.ui_get_input_text (value_return) */
JCE_SCRIPT_API const char *jce_script_api_ui_get_input_text(JceScriptApi *api,
                                                            JceScriptEntity e)
{
    if (!api || !api->host.ui_get_input_text) {
        return NULL;
    }
    return api->host.ui_get_input_text(api->host.user, e);
}

/* jce_script_api_ui_set_input_text -> host.ui_set_input_text (void_call) */
JCE_SCRIPT_API void jce_script_api_ui_set_input_text(JceScriptApi *api,
                                                     JceScriptEntity e,
                                                     const char * text)
{
    if (!api || !api->host.ui_set_input_text) {
        return;
    }
    api->host.ui_set_input_text(api->host.user, e, text);
}

/* jce_script_api_ui_get_scroll -> host.ui_get_scroll (fallible_out) */
JCE_SCRIPT_API bool jce_script_api_ui_get_scroll(JceScriptApi *api,
                                                 JceScriptEntity e,
                                                 float out_xy[2])
{
    if (!api || !api->host.ui_get_scroll) {
        if (out_xy) memset(out_xy, 0, 2 * sizeof out_xy[0]);
        return false;
    }
    return api->host.ui_get_scroll(api->host.user, e, out_xy);
}

/* jce_script_api_ui_set_scroll -> host.ui_set_scroll (void_call) */
JCE_SCRIPT_API void jce_script_api_ui_set_scroll(JceScriptApi *api,
                                                 JceScriptEntity e, float x,
                                                 float y)
{
    if (!api || !api->host.ui_set_scroll) {
        return;
    }
    api->host.ui_set_scroll(api->host.user, e, x, y);
}

/* jce_script_api_world_get_hour -> host.world_get_hour (value_return) */
JCE_SCRIPT_API float jce_script_api_world_get_hour(JceScriptApi *api)
{
    if (!api || !api->host.world_get_hour) {
        return 0.0f;
    }
    return api->host.world_get_hour(api->host.user);
}

/* jce_script_api_world_set_hour -> host.world_set_hour (void_call) */
JCE_SCRIPT_API void jce_script_api_world_set_hour(JceScriptApi *api,
                                                  float hour)
{
    if (!api || !api->host.world_set_hour) {
        return;
    }
    api->host.world_set_hour(api->host.user, hour);
}

/* jce_script_api_world_is_daytime -> host.world_is_daytime (value_return) */
JCE_SCRIPT_API bool jce_script_api_world_is_daytime(JceScriptApi *api)
{
    if (!api || !api->host.world_is_daytime) {
        return false;
    }
    return api->host.world_is_daytime(api->host.user);
}

/* jce_script_api_world_get_weather -> host.world_get_weather (value_return) */
JCE_SCRIPT_API int jce_script_api_world_get_weather(JceScriptApi *api)
{
    if (!api || !api->host.world_get_weather) {
        return 0;
    }
    return api->host.world_get_weather(api->host.user);
}

/* jce_script_api_world_get_weather_intensity -> host.world_get_weather_intensity (value_return) */
JCE_SCRIPT_API float jce_script_api_world_get_weather_intensity(JceScriptApi *api)
{
    if (!api || !api->host.world_get_weather_intensity) {
        return 0.0f;
    }
    return api->host.world_get_weather_intensity(api->host.user);
}

/* jce_script_api_world_get_wind_speed -> host.world_get_wind_speed (value_return) */
JCE_SCRIPT_API float jce_script_api_world_get_wind_speed(JceScriptApi *api)
{
    if (!api || !api->host.world_get_wind_speed) {
        return 0.0f;
    }
    return api->host.world_get_wind_speed(api->host.user);
}

/* jce_script_api_request_scene -> host.request_scene (value_return) */
JCE_SCRIPT_API bool jce_script_api_request_scene(JceScriptApi *api,
                                                 const char * scene_path)
{
    if (!api || !api->host.request_scene) {
        return false;
    }
    return api->host.request_scene(api->host.user, scene_path);
}

/* jce_script_api_is_transitioning -> host.is_transitioning (value_return) */
JCE_SCRIPT_API bool jce_script_api_is_transitioning(JceScriptApi *api)
{
    if (!api || !api->host.is_transitioning) {
        return false;
    }
    return api->host.is_transitioning(api->host.user);
}

/* jce_script_api_audio_play -> host.audio_play (value_return) */
JCE_SCRIPT_API bool jce_script_api_audio_play(JceScriptApi *api,
                                              JceScriptEntity e)
{
    if (!api || !api->host.audio_play) {
        return false;
    }
    return api->host.audio_play(api->host.user, e);
}

/* jce_script_api_audio_stop -> host.audio_stop (value_return) */
JCE_SCRIPT_API bool jce_script_api_audio_stop(JceScriptApi *api,
                                              JceScriptEntity e)
{
    if (!api || !api->host.audio_stop) {
        return false;
    }
    return api->host.audio_stop(api->host.user, e);
}

/* jce_script_api_audio_is_playing -> host.audio_is_playing (value_return) */
JCE_SCRIPT_API bool jce_script_api_audio_is_playing(JceScriptApi *api,
                                                    JceScriptEntity e)
{
    if (!api || !api->host.audio_is_playing) {
        return false;
    }
    return api->host.audio_is_playing(api->host.user, e);
}

/* jce_script_api_save_game -> host.save_game (value_return) */
JCE_SCRIPT_API bool jce_script_api_save_game(JceScriptApi *api,
                                             const char * path)
{
    if (!api || !api->host.save_game) {
        return false;
    }
    return api->host.save_game(api->host.user, path);
}

/* jce_script_api_load_game -> host.load_game (value_return) */
JCE_SCRIPT_API bool jce_script_api_load_game(JceScriptApi *api,
                                             const char * path)
{
    if (!api || !api->host.load_game) {
        return false;
    }
    return api->host.load_game(api->host.user, path);
}

/* jce_script_api_overlap_sphere -> host.overlap_sphere (entity_table) */
JCE_SCRIPT_API int jce_script_api_overlap_sphere(JceScriptApi *api, float x,
                                                 float y, float z,
                                                 float radius,
                                                 uint32_t layer_mask,
                                                 JceScriptEntity *out, int max)
{
    if (!api || !api->host.overlap_sphere) {
        if (out) memset(out, 0, sizeof *out);
        return 0;
    }
    return api->host.overlap_sphere(api->host.user, x, y, z, radius, layer_mask, out, max);
}

/* jce_script_api_overlap_box -> host.overlap_box (entity_table) */
JCE_SCRIPT_API int jce_script_api_overlap_box(JceScriptApi *api, float x,
                                              float y, float z, float hx,
                                              float hy, float hz,
                                              uint32_t layer_mask,
                                              JceScriptEntity *out, int max)
{
    if (!api || !api->host.overlap_box) {
        if (out) memset(out, 0, sizeof *out);
        return 0;
    }
    return api->host.overlap_box(api->host.user, x, y, z, hx, hy, hz, layer_mask, out, max);
}

/* jce_script_api_get_param -> host.get_script_param (fallible_out) */
JCE_SCRIPT_API bool jce_script_api_get_param(JceScriptApi *api,
                                             JceScriptEntity e,
                                             const char * name, int *out_kind,
                                             double *out_number,
                                             JceScriptEntity *out_entity)
{
    if (!api || !api->host.get_script_param) {
        if (out_kind) memset(out_kind, 0, sizeof *out_kind);
        if (out_number) memset(out_number, 0, sizeof *out_number);
        if (out_entity) memset(out_entity, 0, sizeof *out_entity);
        return false;
    }
    return api->host.get_script_param(api->host.user, e, name, out_kind, out_number, out_entity);
}

/* jce_script_api_get_param_text -> host.get_script_param_text (value_return) */
JCE_SCRIPT_API const char *jce_script_api_get_param_text(JceScriptApi *api,
                                                         JceScriptEntity e,
                                                         const char * name)
{
    if (!api || !api->host.get_script_param_text) {
        return NULL;
    }
    return api->host.get_script_param_text(api->host.user, e, name);
}

/* jce_script_api_curve_eval -> host.curve_eval (fallible_out) */
JCE_SCRIPT_API bool jce_script_api_curve_eval(JceScriptApi *api,
                                              const char * path,
                                              const char * channel, double t,
                                              double *out_value)
{
    if (!api || !api->host.curve_eval) {
        if (out_value) memset(out_value, 0, sizeof *out_value);
        return false;
    }
    return api->host.curve_eval(api->host.user, path, channel, t, out_value);
}

/* jce_script_api_vcam_activate -> host.vcam_activate (value_return) */
JCE_SCRIPT_API int jce_script_api_vcam_activate(JceScriptApi *api,
                                                const char * name)
{
    if (!api || !api->host.vcam_activate) {
        return 0;
    }
    return api->host.vcam_activate(api->host.user, name);
}

