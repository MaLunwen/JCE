/* jce_java_diff_host.gen.c -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * See jce_java_diff_host.gen.h. Return values are canned from each member's
 * own NAME, so both processes see identical answers and any difference in the
 * compared stream is the binding's fault and not the mock's.
 *
 * INPUTS are traced; out parameters are not. An out array is uninitialised on
 * entry on the Lua side, and tracing it would hash uninitialised stack -- a
 * harness that fails for a reason that is not a defect gets muted, which is
 * worse than one that passes.
 */

#include "jce_java_diff_host.gen.h"

#include <jce/os/core/jce_alloc.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_jce_java_diff_trace[65536];
static int  g_jce_java_diff_len;
static char g_jce_java_diff_out[16384];

void jce_java_diff_tracef(const char *fmt, ...)
{
    va_list ap;
    int n;

    if (g_jce_java_diff_len < 0 ||
        g_jce_java_diff_len >= (int)sizeof g_jce_java_diff_trace - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(g_jce_java_diff_trace + g_jce_java_diff_len,
                  sizeof g_jce_java_diff_trace -
                      (size_t)g_jce_java_diff_len, fmt, ap);
    va_end(ap);
    if (n > 0 && g_jce_java_diff_len + n < (int)sizeof g_jce_java_diff_trace)
        g_jce_java_diff_len += n;
}

const char *jce_java_diff_trace(void) { return g_jce_java_diff_trace; }
const char *jce_java_diff_out(void)   { return g_jce_java_diff_out; }

void jce_java_diff_reset(void)
{
    g_jce_java_diff_trace[0] = '\0';
    g_jce_java_diff_len = 0;
    g_jce_java_diff_out[0] = '\0';
}

static char *mk_strdup(const char *s)
{
    size_t n = strlen(s) + 1u;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* NOT traced: this is the wire the Lua probe string comes back on, and it is
 * called on both host modes. Tracing it would put a line in every Lua case
 * that the Java side, which never calls log, could not produce. */
static void mk_log(void *user, const char *msg)
{
    size_t n;
    (void)user;
    n = strlen(g_jce_java_diff_out);
    snprintf(g_jce_java_diff_out + n, sizeof g_jce_java_diff_out - n,
             "%s\n", msg ? msg : "(null)");
}

static bool mk_get_position(void *user, JceScriptEntity e, float out_xyz[3])
{
    (void)user;
    jce_java_diff_tracef("get_position");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("\n");
    out_xyz[0] = 9.5f;
    out_xyz[1] = 10.5f;
    out_xyz[2] = 11.5f;
    return true;
}

static void mk_set_position(void *user, JceScriptEntity e, float x, float y, float z)
{
    (void)user;
    jce_java_diff_tracef("set_position");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%g", (double)x);
    jce_java_diff_tracef("|%g", (double)y);
    jce_java_diff_tracef("|%g", (double)z);
    jce_java_diff_tracef("\n");
}

static bool mk_get_rotation(void *user, JceScriptEntity e, float out_euler_deg[3])
{
    (void)user;
    jce_java_diff_tracef("get_rotation");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("\n");
    out_euler_deg[0] = 35.5f;
    out_euler_deg[1] = 36.5f;
    out_euler_deg[2] = 37.5f;
    return true;
}

static void mk_set_rotation(void *user, JceScriptEntity e, float x, float y, float z)
{
    (void)user;
    jce_java_diff_tracef("set_rotation");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%g", (double)x);
    jce_java_diff_tracef("|%g", (double)y);
    jce_java_diff_tracef("|%g", (double)z);
    jce_java_diff_tracef("\n");
}

static bool mk_get_scale(void *user, JceScriptEntity e, float out_xyz[3])
{
    (void)user;
    jce_java_diff_tracef("get_scale");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("\n");
    out_xyz[0] = 9.5f;
    out_xyz[1] = 10.5f;
    out_xyz[2] = 11.5f;
    return true;
}

static void mk_set_scale(void *user, JceScriptEntity e, float x, float y, float z)
{
    (void)user;
    jce_java_diff_tracef("set_scale");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%g", (double)x);
    jce_java_diff_tracef("|%g", (double)y);
    jce_java_diff_tracef("|%g", (double)z);
    jce_java_diff_tracef("\n");
}

static bool mk_is_key_down(void *user, int keycode)
{
    (void)user;
    jce_java_diff_tracef("is_key_down");
    jce_java_diff_tracef("|%lld", (long long)keycode);
    jce_java_diff_tracef("\n");
    return true;
}

static JceScriptEntity mk_find_with_tag(void *user, const char *tag)
{
    (void)user;
    jce_java_diff_tracef("find_with_tag");
    jce_java_diff_tracef("|%s", tag ? tag : "(null)");
    jce_java_diff_tracef("\n");
    return 5;
}

static void mk_destroy_entity(void *user, JceScriptEntity e)
{
    (void)user;
    jce_java_diff_tracef("destroy_entity");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("\n");
}

static JceScriptEntity mk_spawn(void *user, const char *prefab_path, float x, float y, float z)
{
    (void)user;
    jce_java_diff_tracef("spawn");
    jce_java_diff_tracef("|%s", prefab_path ? prefab_path : "(null)");
    jce_java_diff_tracef("|%g", (double)x);
    jce_java_diff_tracef("|%g", (double)y);
    jce_java_diff_tracef("|%g", (double)z);
    jce_java_diff_tracef("\n");
    return 18;
}

static void mk_move_axis(void *user, float out_xz[2])
{
    (void)user;
    jce_java_diff_tracef("move_axis");
    jce_java_diff_tracef("\n");
    out_xz[0] = 22.5f;
    out_xz[1] = 23.5f;
}

static bool mk_input_button(void *user, int button)
{
    (void)user;
    jce_java_diff_tracef("input_button");
    jce_java_diff_tracef("|%lld", (long long)button);
    jce_java_diff_tracef("\n");
    return true;
}

static bool mk_action_down(void *user, const char *name)
{
    (void)user;
    jce_java_diff_tracef("action_down");
    jce_java_diff_tracef("|%s", name ? name : "(null)");
    jce_java_diff_tracef("\n");
    return true;
}

static bool mk_action_pressed(void *user, const char *name)
{
    (void)user;
    jce_java_diff_tracef("action_pressed");
    jce_java_diff_tracef("|%s", name ? name : "(null)");
    jce_java_diff_tracef("\n");
    return true;
}

static float mk_action_axis(void *user, const char *name)
{
    (void)user;
    jce_java_diff_tracef("action_axis");
    jce_java_diff_tracef("|%s", name ? name : "(null)");
    jce_java_diff_tracef("\n");
    return 16.5f;
}

static void mk_pointer_delta(void *user, float out_xy[2])
{
    (void)user;
    jce_java_diff_tracef("pointer_delta");
    jce_java_diff_tracef("\n");
    out_xy[0] = 4.5f;
    out_xy[1] = 5.5f;
}

static float mk_pointer_wheel(void *user)
{
    (void)user;
    jce_java_diff_tracef("pointer_wheel");
    jce_java_diff_tracef("\n");
    return 9.5f;
}

static bool mk_pointer_button(void *user, int button)
{
    (void)user;
    jce_java_diff_tracef("pointer_button");
    jce_java_diff_tracef("|%lld", (long long)button);
    jce_java_diff_tracef("\n");
    return true;
}

/* NEGATIVE on purpose. clamp_min exists because a host may answer with a
 * negative count, and a mock that never returns one leaves the modifier
 * unexercised: both surfaces would agree on 12 whether or not either clamped.
 * With -3 they agree on 0 only if BOTH clamp. */
static int mk_touch_count(void *user)
{
    (void)user;
    jce_java_diff_tracef("touch_count\n");
    return -3;
}

static bool mk_touch_get(void *user, int index, uint64_t *id, float *x, float *y, float *pressure)
{
    (void)user;
    jce_java_diff_tracef("touch_get");
    jce_java_diff_tracef("|%lld", (long long)index);
    jce_java_diff_tracef("\n");
    *id = 26;
    *x = 27.5f;
    *y = 28.5f;
    *pressure = 29.5f;
    return true;
}

static void mk_set_time_scale(void *user, float scale)
{
    (void)user;
    jce_java_diff_tracef("set_time_scale");
    jce_java_diff_tracef("|%g", (double)scale);
    jce_java_diff_tracef("\n");
}

static void mk_set_paused(void *user, bool paused)
{
    (void)user;
    jce_java_diff_tracef("set_paused");
    jce_java_diff_tracef("|%d", paused ? 1 : 0);
    jce_java_diff_tracef("\n");
}

static void mk_shake_camera(void *user, float amount)
{
    (void)user;
    jce_java_diff_tracef("shake_camera");
    jce_java_diff_tracef("|%g", (double)amount);
    jce_java_diff_tracef("\n");
}

static bool mk_gas_activate(void *user, JceScriptEntity e, uint32_t ability_id)
{
    (void)user;
    jce_java_diff_tracef("gas_activate");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%lld", (long long)ability_id);
    jce_java_diff_tracef("\n");
    return true;
}

static bool mk_gas_get(void *user, JceScriptEntity e, const char *attr_name, float *out_value)
{
    (void)user;
    jce_java_diff_tracef("gas_get");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%s", attr_name ? attr_name : "(null)");
    jce_java_diff_tracef("\n");
    *out_value = 32.5f;
    return true;
}

static bool mk_gas_apply(void *user, JceScriptEntity e, const char *attr_name, int op, float magnitude, float duration_seconds)
{
    (void)user;
    jce_java_diff_tracef("gas_apply");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%s", attr_name ? attr_name : "(null)");
    jce_java_diff_tracef("|%lld", (long long)op);
    jce_java_diff_tracef("|%g", (double)magnitude);
    jce_java_diff_tracef("|%g", (double)duration_seconds);
    jce_java_diff_tracef("\n");
    return true;
}

/* Reached only by the hand-written asset_read_* bindings, which are not on
 * the Java surface at all. It exists so the `full` host really is full: a
 * NULL here would make the Lua VM take a different path from the one the
 * generated bindings see. */
static void *mk_read_file(void *user, const char *path, uint64_t *out_size)
{
    static const char kBody[] = "[1,2,3]";
    size_t n = sizeof kBody - 1u;
    char *buf;

    (void)user;
    (void)path;
    buf = (char *)jce_malloc(n);
    if (!buf) {
        if (out_size) *out_size = 0u;
        return NULL;
    }
    memcpy(buf, kBody, n);
    if (out_size) *out_size = (uint64_t)n;
    return buf;
}

static bool mk_raycast(void *user, const float origin[3], const float dir[3], float max_dist, JceScriptRaycastHit *out)
{
    (void)user;
    jce_java_diff_tracef("raycast");
    jce_java_diff_tracef("|%g", origin ? (double)origin[0] : 0.0);
    jce_java_diff_tracef("|%g", origin ? (double)origin[1] : 0.0);
    jce_java_diff_tracef("|%g", origin ? (double)origin[2] : 0.0);
    jce_java_diff_tracef("|%g", dir ? (double)dir[0] : 0.0);
    jce_java_diff_tracef("|%g", dir ? (double)dir[1] : 0.0);
    jce_java_diff_tracef("|%g", dir ? (double)dir[2] : 0.0);
    jce_java_diff_tracef("|%g", (double)max_dist);
    jce_java_diff_tracef("\n");
    memset(out, 0, sizeof *out);
    out->entity = 31;
    out->point[0] = 32.5f;
    out->point[1] = 33.5f;
    out->point[2] = 34.5f;
    out->normal[0] = 35.5f;
    out->normal[1] = 36.5f;
    out->normal[2] = 37.5f;
    out->distance = 38.5f;
    return true;
}

static void mk_apply_impulse(void *user, JceScriptEntity e, float x, float y, float z)
{
    (void)user;
    jce_java_diff_tracef("apply_impulse");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%g", (double)x);
    jce_java_diff_tracef("|%g", (double)y);
    jce_java_diff_tracef("|%g", (double)z);
    jce_java_diff_tracef("\n");
}

static void mk_set_velocity(void *user, JceScriptEntity e, float x, float y, float z)
{
    (void)user;
    jce_java_diff_tracef("set_velocity");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%g", (double)x);
    jce_java_diff_tracef("|%g", (double)y);
    jce_java_diff_tracef("|%g", (double)z);
    jce_java_diff_tracef("\n");
}

static bool mk_get_velocity(void *user, JceScriptEntity e, float out[3])
{
    (void)user;
    jce_java_diff_tracef("get_velocity");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("\n");
    out[0] = 37.5f;
    out[1] = 38.5f;
    out[2] = 39.5f;
    return true;
}

static void mk_anim_set_float(void *user, JceScriptEntity e, const char *name, float v)
{
    (void)user;
    jce_java_diff_tracef("anim_set_float");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%s", name ? name : "(null)");
    jce_java_diff_tracef("|%g", (double)v);
    jce_java_diff_tracef("\n");
}

static void mk_anim_set_int(void *user, JceScriptEntity e, const char *name, int v)
{
    (void)user;
    jce_java_diff_tracef("anim_set_int");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%s", name ? name : "(null)");
    jce_java_diff_tracef("|%lld", (long long)v);
    jce_java_diff_tracef("\n");
}

static void mk_anim_set_bool(void *user, JceScriptEntity e, const char *name, bool v)
{
    (void)user;
    jce_java_diff_tracef("anim_set_bool");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%s", name ? name : "(null)");
    jce_java_diff_tracef("|%d", v ? 1 : 0);
    jce_java_diff_tracef("\n");
}

static void mk_anim_set_trigger(void *user, JceScriptEntity e, const char *name)
{
    (void)user;
    jce_java_diff_tracef("anim_set_trigger");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%s", name ? name : "(null)");
    jce_java_diff_tracef("\n");
}

static void mk_play_sound(void *user, const char *path, const float pos[3], float volume)
{
    (void)user;
    jce_java_diff_tracef("play_sound");
    jce_java_diff_tracef("|%s", path ? path : "(null)");
    jce_java_diff_tracef("|%g", pos ? (double)pos[0] : 0.0);
    jce_java_diff_tracef("|%g", pos ? (double)pos[1] : 0.0);
    jce_java_diff_tracef("|%g", pos ? (double)pos[2] : 0.0);
    jce_java_diff_tracef("|%g", (double)volume);
    jce_java_diff_tracef("\n");
}

static bool mk_ui_get_slider(void *user, JceScriptEntity e, float *out)
{
    (void)user;
    jce_java_diff_tracef("ui_get_slider");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("\n");
    *out = 7.5f;
    return true;
}

static void mk_ui_set_slider(void *user, JceScriptEntity e, float v)
{
    (void)user;
    jce_java_diff_tracef("ui_set_slider");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%g", (double)v);
    jce_java_diff_tracef("\n");
}

static bool mk_ui_get_toggle(void *user, JceScriptEntity e, bool *out)
{
    (void)user;
    jce_java_diff_tracef("ui_get_toggle");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("\n");
    *out = false;
    return true;
}

static void mk_ui_set_toggle(void *user, JceScriptEntity e, bool v)
{
    (void)user;
    jce_java_diff_tracef("ui_set_toggle");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%d", v ? 1 : 0);
    jce_java_diff_tracef("\n");
}

static void mk_ui_set_text(void *user, JceScriptEntity e, const char *txt)
{
    (void)user;
    jce_java_diff_tracef("ui_set_text");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%s", txt ? txt : "(null)");
    jce_java_diff_tracef("\n");
}

static void mk_send_message(void *user, JceScriptEntity target, const char *msg, double number_arg, const char *str_arg)
{
    (void)user;
    jce_java_diff_tracef("send_message");
    jce_java_diff_tracef("|%lld", (long long)target);
    jce_java_diff_tracef("|%s", msg ? msg : "(null)");
    jce_java_diff_tracef("|%g", (double)number_arg);
    jce_java_diff_tracef("|%s", str_arg ? str_arg : "(null)");
    jce_java_diff_tracef("\n");
}

static void mk_broadcast(void *user, const char *msg, double number_arg, const char *str_arg)
{
    (void)user;
    jce_java_diff_tracef("broadcast");
    jce_java_diff_tracef("|%s", msg ? msg : "(null)");
    jce_java_diff_tracef("|%g", (double)number_arg);
    jce_java_diff_tracef("|%s", str_arg ? str_arg : "(null)");
    jce_java_diff_tracef("\n");
}

static bool mk_net_is_server(void *user)
{
    (void)user;
    jce_java_diff_tracef("net_is_server");
    jce_java_diff_tracef("\n");
    return true;
}

static bool mk_net_is_client(void *user)
{
    (void)user;
    jce_java_diff_tracef("net_is_client");
    jce_java_diff_tracef("\n");
    return true;
}

static JceScriptEntity mk_net_spawn(void *user, const char *prefab_path, float x, float y, float z)
{
    (void)user;
    jce_java_diff_tracef("net_spawn");
    jce_java_diff_tracef("|%s", prefab_path ? prefab_path : "(null)");
    jce_java_diff_tracef("|%g", (double)x);
    jce_java_diff_tracef("|%g", (double)y);
    jce_java_diff_tracef("|%g", (double)z);
    jce_java_diff_tracef("\n");
    return 35;
}

static bool mk_rpc_send(void *user, JceScriptEntity e, const char *event, int target, const char *payload)
{
    (void)user;
    jce_java_diff_tracef("rpc_send");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%s", event ? event : "(null)");
    jce_java_diff_tracef("|%lld", (long long)target);
    jce_java_diff_tracef("|%s", payload ? payload : "(null)");
    jce_java_diff_tracef("\n");
    return true;
}

static void mk_particle_burst(void *user, JceScriptEntity e, int count)
{
    (void)user;
    jce_java_diff_tracef("particle_burst");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%lld", (long long)count);
    jce_java_diff_tracef("\n");
}

static void mk_particle_set_emitting(void *user, JceScriptEntity e, bool on)
{
    (void)user;
    jce_java_diff_tracef("particle_set_emitting");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%d", on ? 1 : 0);
    jce_java_diff_tracef("\n");
}

static void mk_particle_set_color(void *user, JceScriptEntity e, float r, float g, float b)
{
    (void)user;
    jce_java_diff_tracef("particle_set_color");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%g", (double)r);
    jce_java_diff_tracef("|%g", (double)g);
    jce_java_diff_tracef("|%g", (double)b);
    jce_java_diff_tracef("\n");
}

static bool mk_has_component(void *user, JceScriptEntity e, const char *comp_name)
{
    (void)user;
    jce_java_diff_tracef("has_component");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%s", comp_name ? comp_name : "(null)");
    jce_java_diff_tracef("\n");
    return true;
}

static bool mk_is_component_enabled(void *user, JceScriptEntity e, const char *comp_name)
{
    (void)user;
    jce_java_diff_tracef("is_component_enabled");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%s", comp_name ? comp_name : "(null)");
    jce_java_diff_tracef("\n");
    return true;
}

static void mk_set_component_enabled(void *user, JceScriptEntity e, const char *comp_name, bool on)
{
    (void)user;
    jce_java_diff_tracef("set_component_enabled");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%s", comp_name ? comp_name : "(null)");
    jce_java_diff_tracef("|%d", on ? 1 : 0);
    jce_java_diff_tracef("\n");
}

static void mk_vehicle_set_input(void *user, JceScriptEntity e, float throttle, float brake, float steer)
{
    (void)user;
    jce_java_diff_tracef("vehicle_set_input");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%g", (double)throttle);
    jce_java_diff_tracef("|%g", (double)brake);
    jce_java_diff_tracef("|%g", (double)steer);
    jce_java_diff_tracef("\n");
}

static float mk_vehicle_get_speed(void *user, JceScriptEntity e)
{
    (void)user;
    jce_java_diff_tracef("vehicle_get_speed");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("\n");
    return 4.5f;
}

static void mk_get_move(void *user, float out[3])
{
    (void)user;
    jce_java_diff_tracef("get_move");
    jce_java_diff_tracef("\n");
    out[0] = 29.5f;
    out[1] = 30.5f;
    out[2] = 31.5f;
}

static void mk_music_set_intensity(void *user, float intensity)
{
    (void)user;
    jce_java_diff_tracef("music_set_intensity");
    jce_java_diff_tracef("|%g", (double)intensity);
    jce_java_diff_tracef("\n");
}

static float mk_music_get_intensity(void *user)
{
    (void)user;
    jce_java_diff_tracef("music_get_intensity");
    jce_java_diff_tracef("\n");
    return 14.5f;
}

static float mk_music_request_transition(void *user, int to_segment)
{
    (void)user;
    jce_java_diff_tracef("music_request_transition");
    jce_java_diff_tracef("|%lld", (long long)to_segment);
    jce_java_diff_tracef("\n");
    return 39.5f;
}

static int mk_find_by_name(void *user, const char *name, JceScriptEntity *out, int max)
{
    (void)user;
    jce_java_diff_tracef("find_by_name");
    jce_java_diff_tracef("|%s", name ? name : "(null)");
    jce_java_diff_tracef("|%lld", (long long)max);
    jce_java_diff_tracef("\n");
    for (int i = 0; i < 3 && i < max; i++)
        out[i] = (JceScriptEntity)(100 + i);
    return max < 3 ? max : 3;
}

static int mk_find_by_prefix(void *user, const char *prefix, JceScriptEntity *out, int max)
{
    (void)user;
    jce_java_diff_tracef("find_by_prefix");
    jce_java_diff_tracef("|%s", prefix ? prefix : "(null)");
    jce_java_diff_tracef("|%lld", (long long)max);
    jce_java_diff_tracef("\n");
    for (int i = 0; i < 3 && i < max; i++)
        out[i] = (JceScriptEntity)(100 + i);
    return max < 3 ? max : 3;
}

static char * mk_comp_get_json(void *user, JceScriptEntity e, const char *type)
{
    (void)user;
    jce_java_diff_tracef("comp_get_json");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%s", type ? type : "(null)");
    jce_java_diff_tracef("\n");
    return mk_strdup("comp_get_json_json");
}

static bool mk_comp_set_json(void *user, JceScriptEntity e, const char *type, const char *json)
{
    (void)user;
    jce_java_diff_tracef("comp_set_json");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%s", type ? type : "(null)");
    jce_java_diff_tracef("|%s", json ? json : "(null)");
    jce_java_diff_tracef("\n");
    return true;
}

static char * mk_render_get_json(void *user)
{
    (void)user;
    jce_java_diff_tracef("render_get_json");
    jce_java_diff_tracef("\n");
    return mk_strdup("render_get_json_json");
}

static bool mk_render_set_json(void *user, const char *json)
{
    (void)user;
    jce_java_diff_tracef("render_set_json");
    jce_java_diff_tracef("|%s", json ? json : "(null)");
    jce_java_diff_tracef("\n");
    return true;
}

/* Traced BY CONTENT. The pointer differs between the two processes and means
 * nothing; "was the host's string released at all" is exactly what the
 * owned_string_release shape has to get right, and it is the only evidence
 * either side releases. free(), matching the malloc in mk_strdup. */
static void mk_json_free(void *user, char *s)
{
    (void)user;
    jce_java_diff_tracef("json_free|%s\n", s ? s : "(null)");
    free(s);
}

static void mk_audio_set_volume(void *user, JceScriptEntity e, float volume)
{
    (void)user;
    jce_java_diff_tracef("audio_set_volume");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%g", (double)volume);
    jce_java_diff_tracef("\n");
}

static bool mk_set_parent(void *user, JceScriptEntity child, JceScriptEntity parent, bool preserve_world)
{
    (void)user;
    jce_java_diff_tracef("set_parent");
    jce_java_diff_tracef("|%lld", (long long)child);
    jce_java_diff_tracef("|%lld", (long long)parent);
    jce_java_diff_tracef("|%d", preserve_world ? 1 : 0);
    jce_java_diff_tracef("\n");
    return true;
}

static JceScriptEntity mk_get_parent(void *user, JceScriptEntity child)
{
    (void)user;
    jce_java_diff_tracef("get_parent");
    jce_java_diff_tracef("|%lld", (long long)child);
    jce_java_diff_tracef("\n");
    return 34;
}

static void mk_play_sound_spatial(void *user, const char *path, const float pos[3], float volume, float min_distance, float max_distance, float rolloff)
{
    (void)user;
    jce_java_diff_tracef("play_sound_spatial");
    jce_java_diff_tracef("|%s", path ? path : "(null)");
    jce_java_diff_tracef("|%g", pos ? (double)pos[0] : 0.0);
    jce_java_diff_tracef("|%g", pos ? (double)pos[1] : 0.0);
    jce_java_diff_tracef("|%g", pos ? (double)pos[2] : 0.0);
    jce_java_diff_tracef("|%g", (double)volume);
    jce_java_diff_tracef("|%g", (double)min_distance);
    jce_java_diff_tracef("|%g", (double)max_distance);
    jce_java_diff_tracef("|%g", (double)rolloff);
    jce_java_diff_tracef("\n");
}

static const char * mk_loc_translate(void *user, const char *key)
{
    (void)user;
    jce_java_diff_tracef("loc_translate");
    jce_java_diff_tracef("|%s", key ? key : "(null)");
    jce_java_diff_tracef("\n");
    return "loc_translate_ret";
}

static const char * mk_loc_get_locale(void *user)
{
    (void)user;
    jce_java_diff_tracef("loc_get_locale");
    jce_java_diff_tracef("\n");
    return "loc_get_locale_ret";
}

static void mk_loc_set_locale(void *user, const char *locale)
{
    (void)user;
    jce_java_diff_tracef("loc_set_locale");
    jce_java_diff_tracef("|%s", locale ? locale : "(null)");
    jce_java_diff_tracef("\n");
}

static bool mk_get_world_position(void *user, JceScriptEntity e, float out_xyz[3])
{
    (void)user;
    jce_java_diff_tracef("get_world_position");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("\n");
    out_xyz[0] = 27.5f;
    out_xyz[1] = 28.5f;
    out_xyz[2] = 29.5f;
    return true;
}

static int mk_line_set_points(void *user, JceScriptEntity e, const float *xyz, int count)
{
    (void)user;
    jce_java_diff_tracef("line_set_points");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%lld", (long long)xyz);
    jce_java_diff_tracef("|%lld", (long long)count);
    jce_java_diff_tracef("\n");
    return 5;
}

static bool mk_ui_get_progress(void *user, JceScriptEntity e, float *out)
{
    (void)user;
    jce_java_diff_tracef("ui_get_progress");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("\n");
    *out = 41.5f;
    return true;
}

static void mk_ui_set_progress(void *user, JceScriptEntity e, float v)
{
    (void)user;
    jce_java_diff_tracef("ui_set_progress");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%g", (double)v);
    jce_java_diff_tracef("\n");
}

static bool mk_ui_get_dropdown(void *user, JceScriptEntity e, int *out)
{
    (void)user;
    jce_java_diff_tracef("ui_get_dropdown");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("\n");
    *out = 22;
    return true;
}

static void mk_ui_set_dropdown(void *user, JceScriptEntity e, int index)
{
    (void)user;
    jce_java_diff_tracef("ui_set_dropdown");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%lld", (long long)index);
    jce_java_diff_tracef("\n");
}

static const char * mk_ui_get_input_text(void *user, JceScriptEntity e)
{
    (void)user;
    jce_java_diff_tracef("ui_get_input_text");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("\n");
    return "ui_get_input_text_ret";
}

static void mk_ui_set_input_text(void *user, JceScriptEntity e, const char *text)
{
    (void)user;
    jce_java_diff_tracef("ui_set_input_text");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%s", text ? text : "(null)");
    jce_java_diff_tracef("\n");
}

static bool mk_ui_get_scroll(void *user, JceScriptEntity e, float out_xy[2])
{
    (void)user;
    jce_java_diff_tracef("ui_get_scroll");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("\n");
    out_xy[0] = 30.5f;
    out_xy[1] = 31.5f;
    return true;
}

static void mk_ui_set_scroll(void *user, JceScriptEntity e, float x, float y)
{
    (void)user;
    jce_java_diff_tracef("ui_set_scroll");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%g", (double)x);
    jce_java_diff_tracef("|%g", (double)y);
    jce_java_diff_tracef("\n");
}

static float mk_world_get_hour(void *user)
{
    (void)user;
    jce_java_diff_tracef("world_get_hour");
    jce_java_diff_tracef("\n");
    return 7.5f;
}

static void mk_world_set_hour(void *user, float hour)
{
    (void)user;
    jce_java_diff_tracef("world_set_hour");
    jce_java_diff_tracef("|%g", (double)hour);
    jce_java_diff_tracef("\n");
}

static bool mk_world_is_daytime(void *user)
{
    (void)user;
    jce_java_diff_tracef("world_is_daytime");
    jce_java_diff_tracef("\n");
    return true;
}

static int mk_world_get_weather(void *user)
{
    (void)user;
    jce_java_diff_tracef("world_get_weather");
    jce_java_diff_tracef("\n");
    return 10;
}

static float mk_world_get_weather_intensity(void *user)
{
    (void)user;
    jce_java_diff_tracef("world_get_weather_intensity");
    jce_java_diff_tracef("\n");
    return 31.5f;
}

static float mk_world_get_wind_speed(void *user)
{
    (void)user;
    jce_java_diff_tracef("world_get_wind_speed");
    jce_java_diff_tracef("\n");
    return 23.5f;
}

static bool mk_request_scene(void *user, const char *scene_path)
{
    (void)user;
    jce_java_diff_tracef("request_scene");
    jce_java_diff_tracef("|%s", scene_path ? scene_path : "(null)");
    jce_java_diff_tracef("\n");
    return true;
}

static bool mk_is_transitioning(void *user)
{
    (void)user;
    jce_java_diff_tracef("is_transitioning");
    jce_java_diff_tracef("\n");
    return true;
}

static bool mk_audio_play(void *user, JceScriptEntity e)
{
    (void)user;
    jce_java_diff_tracef("audio_play");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("\n");
    return true;
}

static bool mk_audio_stop(void *user, JceScriptEntity e)
{
    (void)user;
    jce_java_diff_tracef("audio_stop");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("\n");
    return true;
}

static bool mk_audio_is_playing(void *user, JceScriptEntity e)
{
    (void)user;
    jce_java_diff_tracef("audio_is_playing");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("\n");
    return true;
}

static bool mk_save_game(void *user, const char *path)
{
    (void)user;
    jce_java_diff_tracef("save_game");
    jce_java_diff_tracef("|%s", path ? path : "(null)");
    jce_java_diff_tracef("\n");
    return true;
}

static bool mk_load_game(void *user, const char *path)
{
    (void)user;
    jce_java_diff_tracef("load_game");
    jce_java_diff_tracef("|%s", path ? path : "(null)");
    jce_java_diff_tracef("\n");
    return true;
}

static int mk_overlap_sphere(void *user, float x, float y, float z, float radius, uint32_t layer_mask, JceScriptEntity *out, int max)
{
    (void)user;
    jce_java_diff_tracef("overlap_sphere");
    jce_java_diff_tracef("|%g", (double)x);
    jce_java_diff_tracef("|%g", (double)y);
    jce_java_diff_tracef("|%g", (double)z);
    jce_java_diff_tracef("|%g", (double)radius);
    jce_java_diff_tracef("|%lld", (long long)layer_mask);
    jce_java_diff_tracef("|%lld", (long long)max);
    jce_java_diff_tracef("\n");
    for (int i = 0; i < 3 && i < max; i++)
        out[i] = (JceScriptEntity)(100 + i);
    return max < 3 ? max : 3;
}

static int mk_overlap_box(void *user, float x, float y, float z, float hx, float hy, float hz, uint32_t layer_mask, JceScriptEntity *out, int max)
{
    (void)user;
    jce_java_diff_tracef("overlap_box");
    jce_java_diff_tracef("|%g", (double)x);
    jce_java_diff_tracef("|%g", (double)y);
    jce_java_diff_tracef("|%g", (double)z);
    jce_java_diff_tracef("|%g", (double)hx);
    jce_java_diff_tracef("|%g", (double)hy);
    jce_java_diff_tracef("|%g", (double)hz);
    jce_java_diff_tracef("|%lld", (long long)layer_mask);
    jce_java_diff_tracef("|%lld", (long long)max);
    jce_java_diff_tracef("\n");
    for (int i = 0; i < 3 && i < max; i++)
        out[i] = (JceScriptEntity)(100 + i);
    return max < 3 ? max : 3;
}

static int mk_vcam_activate(void *user, const char *name)
{
    (void)user;
    jce_java_diff_tracef("vcam_activate");
    jce_java_diff_tracef("|%s", name ? name : "(null)");
    jce_java_diff_tracef("\n");
    return 37;
}

static bool mk_get_script_param(void *user, JceScriptEntity e, const char *name, int *out_kind, double *out_number, JceScriptEntity *out_entity)
{
    (void)user;
    jce_java_diff_tracef("get_script_param");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%s", name ? name : "(null)");
    jce_java_diff_tracef("\n");
    *out_kind = 39;
    *out_number = 40.5;
    *out_entity = 41;
    return true;
}

static const char * mk_get_script_param_text(void *user, JceScriptEntity e, const char *name)
{
    (void)user;
    jce_java_diff_tracef("get_script_param_text");
    jce_java_diff_tracef("|%lld", (long long)e);
    jce_java_diff_tracef("|%s", name ? name : "(null)");
    jce_java_diff_tracef("\n");
    return "get_script_param_text_ret";
}

static bool mk_curve_eval(void *user, const char *path, const char *channel, double t, double *out_value)
{
    (void)user;
    jce_java_diff_tracef("curve_eval");
    jce_java_diff_tracef("|%s", path ? path : "(null)");
    jce_java_diff_tracef("|%s", channel ? channel : "(null)");
    jce_java_diff_tracef("|%g", (double)t);
    jce_java_diff_tracef("\n");
    *out_value = 35.5;
    return true;
}

static bool mk_raycast_filtered(void *user, const float origin[3], const float dir[3], float max_dist, uint32_t layer_mask, bool hit_triggers, JceScriptRaycastHit *out)
{
    (void)user;
    jce_java_diff_tracef("raycast_filtered");
    jce_java_diff_tracef("|%g", origin ? (double)origin[0] : 0.0);
    jce_java_diff_tracef("|%g", origin ? (double)origin[1] : 0.0);
    jce_java_diff_tracef("|%g", origin ? (double)origin[2] : 0.0);
    jce_java_diff_tracef("|%g", dir ? (double)dir[0] : 0.0);
    jce_java_diff_tracef("|%g", dir ? (double)dir[1] : 0.0);
    jce_java_diff_tracef("|%g", dir ? (double)dir[2] : 0.0);
    jce_java_diff_tracef("|%g", (double)max_dist);
    jce_java_diff_tracef("|%lld", (long long)layer_mask);
    jce_java_diff_tracef("|%d", hit_triggers ? 1 : 0);
    jce_java_diff_tracef("\n");
    memset(out, 0, sizeof *out);
    out->entity = 23;
    out->point[0] = 24.5f;
    out->point[1] = 25.5f;
    out->point[2] = 26.5f;
    out->normal[0] = 27.5f;
    out->normal[1] = 28.5f;
    out->normal[2] = 29.5f;
    out->distance = 30.5f;
    return true;
}

static int mk_raycast_all(void *user, const float origin[3], const float dir[3], float max_dist, uint32_t layer_mask, bool hit_triggers, JceScriptEntity *out, int max)
{
    (void)user;
    jce_java_diff_tracef("raycast_all");
    jce_java_diff_tracef("|%g", origin ? (double)origin[0] : 0.0);
    jce_java_diff_tracef("|%g", origin ? (double)origin[1] : 0.0);
    jce_java_diff_tracef("|%g", origin ? (double)origin[2] : 0.0);
    jce_java_diff_tracef("|%g", dir ? (double)dir[0] : 0.0);
    jce_java_diff_tracef("|%g", dir ? (double)dir[1] : 0.0);
    jce_java_diff_tracef("|%g", dir ? (double)dir[2] : 0.0);
    jce_java_diff_tracef("|%g", (double)max_dist);
    jce_java_diff_tracef("|%lld", (long long)layer_mask);
    jce_java_diff_tracef("|%d", hit_triggers ? 1 : 0);
    jce_java_diff_tracef("|%lld", (long long)max);
    jce_java_diff_tracef("\n");
    for (int i = 0; i < 3 && i < max; i++)
        out[i] = (JceScriptEntity)(100 + i);
    return max < 3 ? max : 3;
}

static JceScriptHost g_full;
static JceScriptHost g_partial;
static int g_ready;

static void build(void)
{
    if (g_ready) return;
    g_ready = 1;
    memset(&g_full, 0, sizeof g_full);
    memset(&g_partial, 0, sizeof g_partial);
    g_full.user = (void *)&g_ready;
    g_partial.user = (void *)&g_ready;
    g_partial.log = mk_log;
    g_full.log = mk_log;
    g_full.get_position = mk_get_position;
    g_full.set_position = mk_set_position;
    g_full.get_rotation = mk_get_rotation;
    g_full.set_rotation = mk_set_rotation;
    g_full.get_scale = mk_get_scale;
    g_full.set_scale = mk_set_scale;
    g_full.is_key_down = mk_is_key_down;
    g_full.find_with_tag = mk_find_with_tag;
    g_full.destroy_entity = mk_destroy_entity;
    g_full.spawn = mk_spawn;
    g_full.move_axis = mk_move_axis;
    g_full.input_button = mk_input_button;
    g_full.action_down = mk_action_down;
    g_full.action_pressed = mk_action_pressed;
    g_full.action_axis = mk_action_axis;
    g_full.pointer_delta = mk_pointer_delta;
    g_full.pointer_wheel = mk_pointer_wheel;
    g_full.pointer_button = mk_pointer_button;
    g_full.touch_count = mk_touch_count;
    g_full.touch_get = mk_touch_get;
    g_full.set_time_scale = mk_set_time_scale;
    g_full.set_paused = mk_set_paused;
    g_full.shake_camera = mk_shake_camera;
    g_full.gas_activate = mk_gas_activate;
    g_full.gas_get = mk_gas_get;
    g_full.gas_apply = mk_gas_apply;
    g_full.read_file = mk_read_file;
    g_full.raycast = mk_raycast;
    g_full.apply_impulse = mk_apply_impulse;
    g_full.set_velocity = mk_set_velocity;
    g_full.get_velocity = mk_get_velocity;
    g_full.anim_set_float = mk_anim_set_float;
    g_full.anim_set_int = mk_anim_set_int;
    g_full.anim_set_bool = mk_anim_set_bool;
    g_full.anim_set_trigger = mk_anim_set_trigger;
    g_full.play_sound = mk_play_sound;
    g_full.ui_get_slider = mk_ui_get_slider;
    g_full.ui_set_slider = mk_ui_set_slider;
    g_full.ui_get_toggle = mk_ui_get_toggle;
    g_full.ui_set_toggle = mk_ui_set_toggle;
    g_full.ui_set_text = mk_ui_set_text;
    g_full.send_message = mk_send_message;
    g_full.broadcast = mk_broadcast;
    g_full.net_is_server = mk_net_is_server;
    g_full.net_is_client = mk_net_is_client;
    g_full.net_spawn = mk_net_spawn;
    g_full.rpc_send = mk_rpc_send;
    g_full.particle_burst = mk_particle_burst;
    g_full.particle_set_emitting = mk_particle_set_emitting;
    g_full.particle_set_color = mk_particle_set_color;
    g_full.has_component = mk_has_component;
    g_full.is_component_enabled = mk_is_component_enabled;
    g_full.set_component_enabled = mk_set_component_enabled;
    g_full.vehicle_set_input = mk_vehicle_set_input;
    g_full.vehicle_get_speed = mk_vehicle_get_speed;
    g_full.get_move = mk_get_move;
    g_full.music_set_intensity = mk_music_set_intensity;
    g_full.music_get_intensity = mk_music_get_intensity;
    g_full.music_request_transition = mk_music_request_transition;
    g_full.find_by_name = mk_find_by_name;
    g_full.find_by_prefix = mk_find_by_prefix;
    g_full.comp_get_json = mk_comp_get_json;
    g_full.comp_set_json = mk_comp_set_json;
    g_full.render_get_json = mk_render_get_json;
    g_full.render_set_json = mk_render_set_json;
    g_full.json_free = mk_json_free;
    g_full.audio_set_volume = mk_audio_set_volume;
    g_full.set_parent = mk_set_parent;
    g_full.get_parent = mk_get_parent;
    g_full.play_sound_spatial = mk_play_sound_spatial;
    g_full.loc_translate = mk_loc_translate;
    g_full.loc_get_locale = mk_loc_get_locale;
    g_full.loc_set_locale = mk_loc_set_locale;
    g_full.get_world_position = mk_get_world_position;
    g_full.line_set_points = mk_line_set_points;
    g_full.ui_get_progress = mk_ui_get_progress;
    g_full.ui_set_progress = mk_ui_set_progress;
    g_full.ui_get_dropdown = mk_ui_get_dropdown;
    g_full.ui_set_dropdown = mk_ui_set_dropdown;
    g_full.ui_get_input_text = mk_ui_get_input_text;
    g_full.ui_set_input_text = mk_ui_set_input_text;
    g_full.ui_get_scroll = mk_ui_get_scroll;
    g_full.ui_set_scroll = mk_ui_set_scroll;
    g_full.world_get_hour = mk_world_get_hour;
    g_full.world_set_hour = mk_world_set_hour;
    g_full.world_is_daytime = mk_world_is_daytime;
    g_full.world_get_weather = mk_world_get_weather;
    g_full.world_get_weather_intensity = mk_world_get_weather_intensity;
    g_full.world_get_wind_speed = mk_world_get_wind_speed;
    g_full.request_scene = mk_request_scene;
    g_full.is_transitioning = mk_is_transitioning;
    g_full.audio_play = mk_audio_play;
    g_full.audio_stop = mk_audio_stop;
    g_full.audio_is_playing = mk_audio_is_playing;
    g_full.save_game = mk_save_game;
    g_full.load_game = mk_load_game;
    g_full.overlap_sphere = mk_overlap_sphere;
    g_full.overlap_box = mk_overlap_box;
    g_full.vcam_activate = mk_vcam_activate;
    g_full.get_script_param = mk_get_script_param;
    g_full.get_script_param_text = mk_get_script_param_text;
    g_full.curve_eval = mk_curve_eval;
    g_full.raycast_filtered = mk_raycast_filtered;
    g_full.raycast_all = mk_raycast_all;
}

const JceScriptHost *jce_java_diff_host_full(void)
{
    build();
    return &g_full;
}

const JceScriptHost *jce_java_diff_host_partial(void)
{
    build();
    return &g_partial;
}

size_t jce_java_diff_host_size(void)
{
    return sizeof(JceScriptHost);
}

/* The case table. Both drivers walk THIS order. */
static const char *const g_case_labels[] = {
    "get_position",
    "set_position",
    "get_rotation",
    "set_rotation",
    "get_scale",
    "get_world_position",
    "set_scale",
    "set_parent",
    "set_parent [strict preserve_world]",
    "get_parent",
    "is_key_down",
    "find_with_tag",
    "destroy",
    "spawn",
    "spawn [defaults]",
    "move_axis",
    "jump_pressed",
    "sprint",
    "attack_pressed",
    "set_time_scale",
    "pause",
    "pause [defaults]",
    "shake_camera",
    "shake_camera [defaults]",
    "music_set_intensity",
    "music_get_intensity",
    "music_request_transition",
    "gas_activate",
    "gas_get",
    "gas_apply",
    "gas_apply [defaults]",
    "raycast",
    "raycast_filtered",
    "raycast_filtered [defaults]",
    "raycast_all",
    "raycast_all [defaults]",
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
    "get_touch [index below base]",
    "tr",
    "get_locale",
    "set_locale",
    "get_velocity",
    "vehicle_set_input",
    "vehicle_get_speed",
    "get_move",
    "ui_get_slider",
    "ui_set_slider",
    "ui_get_progress",
    "ui_set_progress",
    "ui_get_toggle",
    "ui_set_toggle",
    "ui_set_text",
    "send_message",
    "send_message [defaults]",
    "broadcast",
    "broadcast [defaults]",
    "has_component",
    "is_component_enabled",
    "set_component_enabled",
    "net_is_server",
    "net_is_client",
    "net_spawn",
    "rpc_send",
    "rpc_send [defaults]",
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
    "ui_get_dropdown",
    "ui_set_dropdown",
    "ui_get_input_text",
    "ui_set_input_text",
    "ui_get_scroll",
    "ui_set_scroll",
    "world_get_hour",
    "world_set_hour",
    "world_is_daytime",
    "world_get_weather",
    "world_get_weather_intensity",
    "world_get_wind_speed",
    "request_scene",
    "is_transitioning",
    "audio_play",
    "audio_stop",
    "audio_is_playing",
    "save_game",
    "load_game",
    "overlap_sphere",
    "overlap_sphere [defaults]",
    "overlap_box",
    "overlap_box [defaults]",
    "get_param",
    "get_param_text",
    "curve_eval",
    "vcam_activate",
    "get_position [no host]",
    "set_position [no host]",
    "get_rotation [no host]",
    "set_rotation [no host]",
    "get_scale [no host]",
    "get_world_position [no host]",
    "set_scale [no host]",
    "set_parent [no host]",
    "get_parent [no host]",
    "is_key_down [no host]",
    "find_with_tag [no host]",
    "destroy [no host]",
    "spawn [no host]",
    "move_axis [no host]",
    "jump_pressed [no host]",
    "sprint [no host]",
    "attack_pressed [no host]",
    "set_time_scale [no host]",
    "pause [no host]",
    "shake_camera [no host]",
    "music_set_intensity [no host]",
    "music_get_intensity [no host]",
    "music_request_transition [no host]",
    "gas_activate [no host]",
    "gas_get [no host]",
    "gas_apply [no host]",
    "raycast [no host]",
    "raycast_filtered [no host]",
    "raycast_all [no host]",
    "apply_impulse [no host]",
    "set_velocity [no host]",
    "anim_set_float [no host]",
    "anim_set_int [no host]",
    "anim_set_bool [no host]",
    "anim_set_trigger [no host]",
    "is_action_down [no host]",
    "is_action_pressed [no host]",
    "get_axis [no host]",
    "get_pointer_delta [no host]",
    "get_pointer_wheel [no host]",
    "is_pointer_down [no host]",
    "get_touch_count [no host]",
    "get_touch [no host]",
    "tr [no host]",
    "get_locale [no host]",
    "set_locale [no host]",
    "get_velocity [no host]",
    "vehicle_set_input [no host]",
    "vehicle_get_speed [no host]",
    "get_move [no host]",
    "ui_get_slider [no host]",
    "ui_set_slider [no host]",
    "ui_get_progress [no host]",
    "ui_set_progress [no host]",
    "ui_get_toggle [no host]",
    "ui_set_toggle [no host]",
    "ui_set_text [no host]",
    "send_message [no host]",
    "broadcast [no host]",
    "has_component [no host]",
    "is_component_enabled [no host]",
    "set_component_enabled [no host]",
    "net_is_server [no host]",
    "net_is_client [no host]",
    "net_spawn [no host]",
    "rpc_send [no host]",
    "particle_burst [no host]",
    "particle_set_emitting [no host]",
    "particle_set_color [no host]",
    "find_by_name [no host]",
    "find_by_prefix [no host]",
    "comp_get [no host]",
    "comp_set [no host]",
    "render_get [no host]",
    "render_set [no host]",
    "audio_set_volume [no host]",
    "ui_get_dropdown [no host]",
    "ui_set_dropdown [no host]",
    "ui_get_input_text [no host]",
    "ui_set_input_text [no host]",
    "ui_get_scroll [no host]",
    "ui_set_scroll [no host]",
    "world_get_hour [no host]",
    "world_set_hour [no host]",
    "world_is_daytime [no host]",
    "world_get_weather [no host]",
    "world_get_weather_intensity [no host]",
    "world_get_wind_speed [no host]",
    "request_scene [no host]",
    "is_transitioning [no host]",
    "audio_play [no host]",
    "audio_stop [no host]",
    "audio_is_playing [no host]",
    "save_game [no host]",
    "load_game [no host]",
    "overlap_sphere [no host]",
    "overlap_box [no host]",
    "get_param [no host]",
    "get_param_text [no host]",
    "curve_eval [no host]",
    "vcam_activate [no host]",
};

int jce_java_diff_case_count(void)
{
    return (int)(sizeof g_case_labels / sizeof g_case_labels[0]);
}

const char *jce_java_diff_case_label(int i)
{
    if (i < 0 || i >= jce_java_diff_case_count())
        return "";
    return g_case_labels[i];
}
