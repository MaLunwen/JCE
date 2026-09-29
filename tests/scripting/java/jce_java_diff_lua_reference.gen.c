/* jce_java_diff_lua_reference.gen.c -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * THE REFERENCE SIDE of the cross-language differential. Batch 1 proved the
 * generated Lua bindings equivalent to the hand-written originals over 87
 * cases and then deleted the originals, so Lua -- not the manifest, and not
 * this backend's own emitter -- is what a new language is measured against.
 *
 * It prints a canonical stream to stdout: per case, the full result (arity,
 * then TYPE and VALUE per slot, so nil / 0 / false cannot pass for each
 * other) and the host-call trace. The Java driver prints the same stream from
 * the same mock host; run_differential.py compares them byte for byte.
 *
 * Numbers are canonicalised as IEEE-754 BIT PATTERNS, not as formatted
 * decimals. C's %g strips trailing zeros and Java's does not, so -1.0 is "-1"
 * on one side and "-1.0" on the other -- a difference that is about printf
 * and about nothing else. The bits are exact in both languages and the
 * comparator decodes them when it reports a diff.
 */

#include "jce_java_diff_host.gen.h"

#include <jce/middleware/script/jce_script.h>

#include <stdio.h>
#include <string.h>

static const char kPrelude[] =

    "local function num(v)\n"
    "  if math.type(v) == 'integer' then return 'i:' .. string.format('%d', v) end\n"
    "  return 'f:' .. string.format('%d', (string.unpack('<i8', string.pack('<d', v))))\n"
    "end\n"
    "local function canon(v)\n"
    "  local t = type(v)\n"
    "  if t == 'nil' then return 'nil' end\n"
    "  if t == 'boolean' then return 'b:' .. tostring(v) end\n"
    "  if t == 'number' then return num(v) end\n"
    "  if t == 'string' then return 's:' .. v end\n"
    "  if t == 'table' then\n"
    "    local p = {}\n"
    "    for i = 1, #v do p[i] = num(v[i]) end\n"
    "    return 't:[' .. table.concat(p, ',') .. ']'\n"
    "  end\n"
    "  return 'other:' .. t\n"
    "end\n"
    "local function probe(...)\n"
    "  local r = table.pack(...)\n"
    "  local p = {}\n"
    "  for i = 1, r.n do p[i] = canon(r[i]) end\n"
    "  return r.n .. '|' .. table.concat(p, '|')\n"
    "end\n"
    ;

static const char *const g_calls[] = {
    "jce.get_position(78)",
    "jce.set_position(80, 106.375, 1674.125, 2181.125)",
    "jce.get_rotation(89)",
    "jce.set_rotation(91, 761.75, 2123.25, 578.125)",
    "jce.get_scale(29)",
    "jce.get_world_position(48)",
    "jce.set_scale(12, 598.0, 706.5, 313.625)",
    "jce.set_parent(45, 48, true)",
    NULL,
    "jce.get_parent(6)",
    "jce.is_key_down(48)",
    "jce.find_with_tag('find_with_tag_tag')",
    "jce.destroy(77)",
    "jce.spawn('spawn_prefab_path', 1196.375, 1714.625, 601.25)",
    "jce.spawn('spawn_prefab_path', nil, nil, nil)",
    "jce.move_axis()",
    "jce.jump_pressed()",
    "jce.sprint()",
    "jce.attack_pressed()",
    "jce.set_time_scale(254.5)",
    "jce.pause(true)",
    "jce.pause(nil)",
    "jce.shake_camera(1814.625)",
    "jce.shake_camera(nil)",
    "jce.music_set_intensity(2273.5)",
    "jce.music_get_intensity()",
    "jce.music_request_transition(13)",
    "jce.gas_activate(64, 81)",
    "jce.gas_get(24, 'gas_get_attr_name')",
    "jce.gas_apply(2, 'gas_apply_attr_name', 52, 2178.75, 833.75)",
    "jce.gas_apply(2, 'gas_apply_attr_name', nil, 2178.75, nil)",
    "jce.raycast(2443.375, 2492.875, 311.5, 1408.625, 2363.5, 295.375, 1438.5)",
    "jce.raycast_filtered(2080.5, 1822.5, 1165.25, 2419.875, 1239.875, 2342.375, 245.25, 71, false)",
    "jce.raycast_filtered(2080.5, 1822.5, 1165.25, 2419.875, 1239.875, 2342.375, 245.25, nil, nil)",
    "jce.raycast_all(724.875, 90.5, 1886.25, 2205.0, 1525.625, 1440.625, 2482.0, 31, false)",
    "jce.raycast_all(724.875, 90.5, 1886.25, 2205.0, 1525.625, 1440.625, 2482.0, nil, nil)",
    "jce.apply_impulse(88, 583.25, 1706.75, 1721.25)",
    "jce.set_velocity(46, 2132.75, 881.375, 1655.125)",
    "jce.anim_set_float(79, 'anim_set_float_name', 2114.75)",
    "jce.anim_set_int(49, 'anim_set_int_name', 78)",
    "jce.anim_set_bool(47, 'anim_set_bool_name', false)",
    "jce.anim_set_trigger(23, 'anim_set_trigger_name')",
    "jce.is_action_down('is_action_down_name')",
    "jce.is_action_pressed('is_action_pressed_name')",
    "jce.get_axis('get_axis_name')",
    "jce.get_pointer_delta()",
    "jce.get_pointer_wheel()",
    "jce.is_pointer_down(1)",
    "jce.get_touch_count()",
    "jce.get_touch(1)",
    "jce.get_touch(0)",
    "jce.tr('tr_key')",
    "jce.get_locale()",
    "jce.set_locale('set_locale_locale')",
    "jce.get_velocity(44)",
    "jce.vehicle_set_input(13, 2206.0, 2181.875, 914.5)",
    "jce.vehicle_get_speed(51)",
    "jce.get_move()",
    "jce.ui_get_slider(74)",
    "jce.ui_set_slider(16, 2225.5)",
    "jce.ui_get_progress(9)",
    "jce.ui_set_progress(10, 198.5)",
    "jce.ui_get_toggle(75)",
    "jce.ui_set_toggle(17, false)",
    "jce.ui_set_text(84, 'ui_set_text_txt')",
    "jce.send_message(8, 'send_message_msg', 776.875, 'send_message_str_arg')",
    "jce.send_message(8, 'send_message_msg', nil, nil)",
    "jce.broadcast('broadcast_msg', 1055.625, 'broadcast_str_arg')",
    "jce.broadcast('broadcast_msg', nil, nil)",
    "jce.has_component(40, 'has_component_comp_name')",
    "jce.is_component_enabled(41, 'is_component_enabled_comp_name')",
    "jce.set_component_enabled(48, 'set_component_enabled_comp_name', true)",
    "jce.net_is_server()",
    "jce.net_is_client()",
    "jce.net_spawn('net_spawn_prefab_path', 301.375, 1501.625, 1861.125)",
    "jce.rpc_send(23, 'rpc_send_event', 39, 'rpc_send_payload')",
    "jce.rpc_send(23, 'rpc_send_event', nil, nil)",
    "jce.particle_burst(36, 53)",
    "jce.particle_set_emitting(41, true)",
    "jce.particle_set_color(76, 1588.75, 2102.125, 1560.25)",
    "jce.find_by_name('find_by_name_name')",
    "jce.find_by_prefix('find_by_prefix_prefix')",
    "jce.comp_get(26, 'comp_get_type')",
    "jce.comp_set(97, 'comp_set_type', 'comp_set_json')",
    "jce.render_get()",
    "jce.render_set('render_set_json')",
    "jce.audio_set_volume(55, 966.25)",
    "jce.ui_get_dropdown(37)",
    "jce.ui_set_dropdown(39, 22)",
    "jce.ui_get_input_text(47)",
    "jce.ui_set_input_text(41, 'ui_set_input_text_text')",
    "jce.ui_get_scroll(70)",
    "jce.ui_set_scroll(18, 728.375, 1564.75)",
    "jce.world_get_hour()",
    "jce.world_set_hour(1754.875)",
    "jce.world_is_daytime()",
    "jce.world_get_weather()",
    "jce.world_get_weather_intensity()",
    "jce.world_get_wind_speed()",
    "jce.request_scene('request_scene_scene_path')",
    "jce.is_transitioning()",
    "jce.audio_play(72)",
    "jce.audio_stop(79)",
    "jce.audio_is_playing(86)",
    "jce.save_game('save_game_path')",
    "jce.load_game('load_game_path')",
    "jce.overlap_sphere(1035.5, 207.625, 1063.125, 2067.5, 32)",
    "jce.overlap_sphere(1035.5, 207.625, 1063.125, 2067.5, nil)",
    "jce.overlap_box(1952.625, 352.875, 352.875, 2421.625, 209.5, 1736.125, 38)",
    "jce.overlap_box(1952.625, 352.875, 352.875, 2421.625, 209.5, 1736.125, nil)",
    "jce.get_param(85, 'get_param_name')",
    "jce.get_param_text(86, 'get_param_text_name')",
    "jce.curve_eval('curve_eval_path', 'curve_eval_channel', 2253.625)",
    "jce.vcam_activate('vcam_activate_name')",
    "jce.get_position(78)",
    "jce.set_position(80, 106.375, 1674.125, 2181.125)",
    "jce.get_rotation(89)",
    "jce.set_rotation(91, 761.75, 2123.25, 578.125)",
    "jce.get_scale(29)",
    "jce.get_world_position(48)",
    "jce.set_scale(12, 598.0, 706.5, 313.625)",
    "jce.set_parent(45, 48, true)",
    "jce.get_parent(6)",
    "jce.is_key_down(48)",
    "jce.find_with_tag('find_with_tag_tag')",
    "jce.destroy(77)",
    "jce.spawn('spawn_prefab_path', 1196.375, 1714.625, 601.25)",
    "jce.move_axis()",
    "jce.jump_pressed()",
    "jce.sprint()",
    "jce.attack_pressed()",
    "jce.set_time_scale(254.5)",
    "jce.pause(true)",
    "jce.shake_camera(1814.625)",
    "jce.music_set_intensity(2273.5)",
    "jce.music_get_intensity()",
    "jce.music_request_transition(13)",
    "jce.gas_activate(64, 81)",
    "jce.gas_get(24, 'gas_get_attr_name')",
    "jce.gas_apply(2, 'gas_apply_attr_name', 52, 2178.75, 833.75)",
    "jce.raycast(2443.375, 2492.875, 311.5, 1408.625, 2363.5, 295.375, 1438.5)",
    "jce.raycast_filtered(2080.5, 1822.5, 1165.25, 2419.875, 1239.875, 2342.375, 245.25, 71, false)",
    "jce.raycast_all(724.875, 90.5, 1886.25, 2205.0, 1525.625, 1440.625, 2482.0, 31, false)",
    "jce.apply_impulse(88, 583.25, 1706.75, 1721.25)",
    "jce.set_velocity(46, 2132.75, 881.375, 1655.125)",
    "jce.anim_set_float(79, 'anim_set_float_name', 2114.75)",
    "jce.anim_set_int(49, 'anim_set_int_name', 78)",
    "jce.anim_set_bool(47, 'anim_set_bool_name', false)",
    "jce.anim_set_trigger(23, 'anim_set_trigger_name')",
    "jce.is_action_down('is_action_down_name')",
    "jce.is_action_pressed('is_action_pressed_name')",
    "jce.get_axis('get_axis_name')",
    "jce.get_pointer_delta()",
    "jce.get_pointer_wheel()",
    "jce.is_pointer_down(1)",
    "jce.get_touch_count()",
    "jce.get_touch(1)",
    "jce.tr('tr_key')",
    "jce.get_locale()",
    "jce.set_locale('set_locale_locale')",
    "jce.get_velocity(44)",
    "jce.vehicle_set_input(13, 2206.0, 2181.875, 914.5)",
    "jce.vehicle_get_speed(51)",
    "jce.get_move()",
    "jce.ui_get_slider(74)",
    "jce.ui_set_slider(16, 2225.5)",
    "jce.ui_get_progress(9)",
    "jce.ui_set_progress(10, 198.5)",
    "jce.ui_get_toggle(75)",
    "jce.ui_set_toggle(17, false)",
    "jce.ui_set_text(84, 'ui_set_text_txt')",
    "jce.send_message(8, 'send_message_msg', 776.875, 'send_message_str_arg')",
    "jce.broadcast('broadcast_msg', 1055.625, 'broadcast_str_arg')",
    "jce.has_component(40, 'has_component_comp_name')",
    "jce.is_component_enabled(41, 'is_component_enabled_comp_name')",
    "jce.set_component_enabled(48, 'set_component_enabled_comp_name', true)",
    "jce.net_is_server()",
    "jce.net_is_client()",
    "jce.net_spawn('net_spawn_prefab_path', 301.375, 1501.625, 1861.125)",
    "jce.rpc_send(23, 'rpc_send_event', 39, 'rpc_send_payload')",
    "jce.particle_burst(36, 53)",
    "jce.particle_set_emitting(41, true)",
    "jce.particle_set_color(76, 1588.75, 2102.125, 1560.25)",
    "jce.find_by_name('find_by_name_name')",
    "jce.find_by_prefix('find_by_prefix_prefix')",
    "jce.comp_get(26, 'comp_get_type')",
    "jce.comp_set(97, 'comp_set_type', 'comp_set_json')",
    "jce.render_get()",
    "jce.render_set('render_set_json')",
    "jce.audio_set_volume(55, 966.25)",
    "jce.ui_get_dropdown(37)",
    "jce.ui_set_dropdown(39, 22)",
    "jce.ui_get_input_text(47)",
    "jce.ui_set_input_text(41, 'ui_set_input_text_text')",
    "jce.ui_get_scroll(70)",
    "jce.ui_set_scroll(18, 728.375, 1564.75)",
    "jce.world_get_hour()",
    "jce.world_set_hour(1754.875)",
    "jce.world_is_daytime()",
    "jce.world_get_weather()",
    "jce.world_get_weather_intensity()",
    "jce.world_get_wind_speed()",
    "jce.request_scene('request_scene_scene_path')",
    "jce.is_transitioning()",
    "jce.audio_play(72)",
    "jce.audio_stop(79)",
    "jce.audio_is_playing(86)",
    "jce.save_game('save_game_path')",
    "jce.load_game('load_game_path')",
    "jce.overlap_sphere(1035.5, 207.625, 1063.125, 2067.5, 32)",
    "jce.overlap_box(1952.625, 352.875, 352.875, 2421.625, 209.5, 1736.125, 38)",
    "jce.get_param(85, 'get_param_name')",
    "jce.get_param_text(86, 'get_param_text_name')",
    "jce.curve_eval('curve_eval_path', 'curve_eval_channel', 2253.625)",
    "jce.vcam_activate('vcam_activate_name')",
};

static const char *const g_skips[] = {
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    "strict-preserve_world-is-a-compile-error-in-java",
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

static const int g_partial[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};


static void print_trace(FILE *f)
{
    const char *t = jce_java_diff_trace();
    const char *p = t;

    while (*p) {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        fprintf(f, "T %.*s\n", n, p);
        if (!nl)
            break;
        p = nl + 1;
    }
}

/* argv[1] is the stream file.  A FILE and not stdout, in BINARY mode: on
 * Windows a text-mode stdout turns every \n into \r\n, and the JVM's own
 * -Xcheck:jni diagnostics land on the Java side's stdout.  Neither belongs in
 * a stream that is compared byte for byte. */
int main(int argc, char **argv)
{
    int i;
    int total = jce_java_diff_case_count();
    FILE *f;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <stream-file>\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", argv[1]);
        return 2;
    }

    for (i = 0; i < total; i++) {
        JceScript *s;
        char src[8192];
        JceScriptInstance inst;
        const char *out;

        fprintf(f, "CASE %d %s\n", i, jce_java_diff_case_label(i));
        if (g_skips[i]) {
            fprintf(f, "R SKIPPED %s\n", g_skips[i]);
            continue;
        }
        jce_java_diff_reset();
        s = jce_script_create_sized(g_partial[i]
                                        ? jce_java_diff_host_partial()
                                        : jce_java_diff_host_full(),
                                    jce_java_diff_host_size());
        if (!s) {
            fprintf(f, "R VMFAIL\n");
            continue;
        }
        /* kPrelude carries string.format directives; it is an ARGUMENT here
         * and never part of the format, or snprintf would eat the call
         * expression as one of them.
         *
         * `return {}` IS LOAD-BEARING, not decoration. run_chunk() calls
         * build_instance() on whatever the chunk returns and refuses anything
         * that is not a table, so a chunk ending at the jce.log() line reports
         * R CHUNKFAIL for EVERY case while the call itself has already run --
         * the probe output is produced and then thrown away. Measured: without
         * it, 150 of the 151 cases print R CHUNKFAIL, i.e. the reference side
         * of this differential never ran once. run_differential.py's
         * "the chunk must RUN" check is what fails if this line is lost. */
        snprintf(src, sizeof src, "%s\njce.log(probe(%s))\nreturn {}\n",
                 kPrelude, g_calls[i]);
        jce_java_diff_reset();
        inst = jce_script_instantiate_source(s, "diff", src, 0);
        if (inst == 0) {
            fprintf(f, "R CHUNKFAIL\n");
            jce_script_destroy(s);
            continue;
        }
        out = jce_java_diff_out();
        fprintf(f, "R %s", out);
        if (out[0] == '\0' || out[strlen(out) - 1] != '\n')
            fprintf(f, "\n");
        print_trace(f);
        jce_script_release(s, inst);
        jce_script_destroy(s);
    }
    fprintf(f, "DONE %d\n", total);
    fclose(f);
    return 0;
}
