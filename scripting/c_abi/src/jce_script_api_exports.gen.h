/* jce_script_api_exports.gen.h -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_c_abi.py --write
 *
 * The export set, for the symbol-table test.  Not installed and not
 * part of the ABI: it exists so that the test compares the DLL's real
 * export table against the manifest instead of against a list someone
 * maintains by hand.
 */
#ifndef JCE_SCRIPT_API_EXPORTS_GEN_H
#define JCE_SCRIPT_API_EXPORTS_GEN_H

#define JCE_SCRIPT_API_EXPORT_COUNT 74

/* The host member with the highest vtable_index, and the entry point
 * that reaches it.  The short-host test truncates AT this member's
 * offset, so it keeps withholding the real tail as the struct grows
 * instead of testing a member that stopped being last. */
#define JCE_SCRIPT_API_TAIL_MEMBER loc_set_locale
#define JCE_SCRIPT_API_TAIL_ENTRY  "jce_script_api_set_locale"

static const char *const
kJceScriptApiExports[JCE_SCRIPT_API_EXPORT_COUNT] = {
    "jce_script_api_anim_set_bool",
    "jce_script_api_anim_set_float",
    "jce_script_api_anim_set_int",
    "jce_script_api_anim_set_trigger",
    "jce_script_api_apply_impulse",
    "jce_script_api_attack_pressed",
    "jce_script_api_audio_set_volume",
    "jce_script_api_broadcast",
    "jce_script_api_close",
    "jce_script_api_comp_get",
    "jce_script_api_comp_set",
    "jce_script_api_destroy",
    "jce_script_api_find_by_name",
    "jce_script_api_find_by_prefix",
    "jce_script_api_find_with_tag",
    "jce_script_api_gas_activate",
    "jce_script_api_gas_apply",
    "jce_script_api_gas_get",
    "jce_script_api_get_axis",
    "jce_script_api_get_locale",
    "jce_script_api_get_move",
    "jce_script_api_get_parent",
    "jce_script_api_get_pointer_delta",
    "jce_script_api_get_pointer_wheel",
    "jce_script_api_get_position",
    "jce_script_api_get_rotation",
    "jce_script_api_get_scale",
    "jce_script_api_get_touch",
    "jce_script_api_get_touch_count",
    "jce_script_api_get_velocity",
    "jce_script_api_has_component",
    "jce_script_api_is_action_down",
    "jce_script_api_is_action_pressed",
    "jce_script_api_is_component_enabled",
    "jce_script_api_is_key_down",
    "jce_script_api_is_pointer_down",
    "jce_script_api_jump_pressed",
    "jce_script_api_move_axis",
    "jce_script_api_music_get_intensity",
    "jce_script_api_music_request_transition",
    "jce_script_api_music_set_intensity",
    "jce_script_api_net_is_client",
    "jce_script_api_net_is_server",
    "jce_script_api_net_spawn",
    "jce_script_api_open",
    "jce_script_api_particle_burst",
    "jce_script_api_particle_set_color",
    "jce_script_api_particle_set_emitting",
    "jce_script_api_pause",
    "jce_script_api_raycast",
    "jce_script_api_render_get",
    "jce_script_api_render_set",
    "jce_script_api_rpc_send",
    "jce_script_api_send_message",
    "jce_script_api_set_component_enabled",
    "jce_script_api_set_locale",
    "jce_script_api_set_parent",
    "jce_script_api_set_position",
    "jce_script_api_set_rotation",
    "jce_script_api_set_scale",
    "jce_script_api_set_time_scale",
    "jce_script_api_set_velocity",
    "jce_script_api_shake_camera",
    "jce_script_api_spawn",
    "jce_script_api_sprint",
    "jce_script_api_tr",
    "jce_script_api_ui_get_slider",
    "jce_script_api_ui_get_toggle",
    "jce_script_api_ui_set_slider",
    "jce_script_api_ui_set_text",
    "jce_script_api_ui_set_toggle",
    "jce_script_api_vehicle_get_speed",
    "jce_script_api_vehicle_set_input",
    "jce_script_api_version",
};

#endif /* JCE_SCRIPT_API_EXPORTS_GEN_H */
