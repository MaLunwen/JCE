// Jce.g.cs - GENERATED. DO NOT EDIT.
//
//   python tools/scriptgen/gen_script_bindings.py --write
//
// Source of truth: struct JceScriptHost in
// engine/include/jce/middleware/script/jce_script.h (C types, arity, parameter
// names) joined with the exposure decisions in
// engine/src/middleware/script/script_exposure.json (name, shape, modifiers).
//
// NO SHIM.  Java carries a hand-written JNI C file because JNI is Java's only
// FFI; C# calls the c_abi shared library directly, so this generated file IS
// the whole engine-facing surface of the C# backend and there is no second,
// hand-written copy of it to drift.
//
// A `fallible_out` is rendered as the Try pattern, which is what the C ABI
// actually is - a bool return with out parameters - rather than as a nullable
// value.  The manifest's `miss_value` is therefore a doc note here and not a
// sentinel: `false` already distinguishes a miss, and a sentinel beside it
// would be a second way to ask the same question.

#nullable enable
using System.Runtime.InteropServices;

namespace JceScript;

    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct JceScriptRaycastHit
    {
        public uint entity;
        public fixed float point[3];
        public fixed float normal[3];
        public float distance;
    }

internal static unsafe partial class Interop
{
    private const string Lib = "jce_script_api";

    /// <summary>The ABI version this binding was generated
    /// against.  open() REFUSES a library older than it rather
    /// than degrading, because a newer binding would call entries
    /// that do not exist.  From the manifest, so it cannot drift.
    /// </summary>
    internal const uint ScriptApiMin = 1u;

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr jce_script_api_open(
        IntPtr host, nuint hostSize, uint scriptApiMin);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void jce_script_api_close(IntPtr api);

    internal static class Fn
    {
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_get_position(
            IntPtr api,
            uint e,
            float* outXyz);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_set_position(
            IntPtr api,
            uint e,
            float x,
            float y,
            float z);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_get_rotation(
            IntPtr api,
            uint e,
            float* outEulerDeg);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_set_rotation(
            IntPtr api,
            uint e,
            float x,
            float y,
            float z);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_get_scale(
            IntPtr api,
            uint e,
            float* outXyz);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_get_world_position(
            IntPtr api,
            uint e,
            float* outXyz);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_set_scale(
            IntPtr api,
            uint e,
            float x,
            float y,
            float z);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_set_parent(
            IntPtr api,
            uint child,
            uint parent,
            byte preserveWorld);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint jce_script_api_get_parent(
            IntPtr api,
            uint child);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_is_key_down(
            IntPtr api,
            int keycode);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint jce_script_api_find_with_tag(
            IntPtr api,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? tag);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_destroy(
            IntPtr api,
            uint e);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint jce_script_api_spawn(
            IntPtr api,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? prefabPath,
            float x,
            float y,
            float z);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_move_axis(
            IntPtr api,
            float* outXz);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_jump_pressed(
            IntPtr api,
            int button);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_sprint(
            IntPtr api,
            int button);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_attack_pressed(
            IntPtr api,
            int button);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_set_time_scale(
            IntPtr api,
            float scale);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_pause(
            IntPtr api,
            byte paused);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_shake_camera(
            IntPtr api,
            float amount);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_music_set_intensity(
            IntPtr api,
            float intensity);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern float jce_script_api_music_get_intensity(
            IntPtr api);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern float jce_script_api_music_request_transition(
            IntPtr api,
            int toSegment);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_gas_activate(
            IntPtr api,
            uint e,
            uint abilityId);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_gas_get(
            IntPtr api,
            uint e,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? attrName,
            float* outValue);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_gas_apply(
            IntPtr api,
            uint e,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? attrName,
            int op,
            float magnitude,
            float durationSeconds);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_raycast(
            IntPtr api,
            float* origin,
            float* dir,
            float maxDist,
            JceScriptRaycastHit* @out);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_raycast_filtered(
            IntPtr api,
            float* origin,
            float* dir,
            float maxDist,
            uint layerMask,
            byte hitTriggers,
            JceScriptRaycastHit* @out);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int jce_script_api_raycast_all(
            IntPtr api,
            float* origin,
            float* dir,
            float maxDist,
            uint layerMask,
            byte hitTriggers,
            uint* @out,
            int max);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_apply_impulse(
            IntPtr api,
            uint e,
            float x,
            float y,
            float z);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_set_velocity(
            IntPtr api,
            uint e,
            float x,
            float y,
            float z);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_anim_set_float(
            IntPtr api,
            uint e,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? name,
            float v);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_anim_set_int(
            IntPtr api,
            uint e,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? name,
            int v);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_anim_set_bool(
            IntPtr api,
            uint e,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? name,
            byte v);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_anim_set_trigger(
            IntPtr api,
            uint e,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? name);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_is_action_down(
            IntPtr api,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? name);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_is_action_pressed(
            IntPtr api,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? name);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern float jce_script_api_get_axis(
            IntPtr api,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? name);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_get_pointer_delta(
            IntPtr api,
            float* outXy);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern float jce_script_api_get_pointer_wheel(
            IntPtr api);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_is_pointer_down(
            IntPtr api,
            int button);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int jce_script_api_get_touch_count(
            IntPtr api);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_get_touch(
            IntPtr api,
            int index,
            ulong* id,
            float* x,
            float* y,
            float* pressure);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr jce_script_api_tr(
            IntPtr api,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? key);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr jce_script_api_get_locale(
            IntPtr api);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_set_locale(
            IntPtr api,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? locale);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_get_velocity(
            IntPtr api,
            uint e,
            float* @out);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_vehicle_set_input(
            IntPtr api,
            uint e,
            float throttle,
            float brake,
            float steer);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern float jce_script_api_vehicle_get_speed(
            IntPtr api,
            uint e);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_get_move(
            IntPtr api,
            float* @out);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_ui_get_slider(
            IntPtr api,
            uint e,
            float* @out);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_ui_set_slider(
            IntPtr api,
            uint e,
            float v);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_ui_get_progress(
            IntPtr api,
            uint e,
            float* @out);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_ui_set_progress(
            IntPtr api,
            uint e,
            float v);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_ui_get_toggle(
            IntPtr api,
            uint e,
            byte* @out);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_ui_set_toggle(
            IntPtr api,
            uint e,
            byte v);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_ui_set_text(
            IntPtr api,
            uint e,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? txt);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_send_message(
            IntPtr api,
            uint target,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? msg,
            double numberArg,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? strArg);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_broadcast(
            IntPtr api,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? msg,
            double numberArg,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? strArg);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_has_component(
            IntPtr api,
            uint e,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? compName);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_is_component_enabled(
            IntPtr api,
            uint e,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? compName);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_set_component_enabled(
            IntPtr api,
            uint e,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? compName,
            byte on);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_net_is_server(
            IntPtr api);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_net_is_client(
            IntPtr api);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern uint jce_script_api_net_spawn(
            IntPtr api,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? prefabPath,
            float x,
            float y,
            float z);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_rpc_send(
            IntPtr api,
            uint e,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? @event,
            int target,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? payload);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_particle_burst(
            IntPtr api,
            uint e,
            int count);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_particle_set_emitting(
            IntPtr api,
            uint e,
            byte on);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_particle_set_color(
            IntPtr api,
            uint e,
            float r,
            float g,
            float b);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int jce_script_api_find_by_name(
            IntPtr api,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? name,
            uint* @out,
            int max);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int jce_script_api_find_by_prefix(
            IntPtr api,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? prefix,
            uint* @out,
            int max);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr jce_script_api_comp_get(
            IntPtr api,
            uint e,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? type);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_comp_set(
            IntPtr api,
            uint e,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? type,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? json);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr jce_script_api_render_get(
            IntPtr api);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_render_set(
            IntPtr api,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? json);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_audio_set_volume(
            IntPtr api,
            uint e,
            float volume);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_ui_get_dropdown(
            IntPtr api,
            uint e,
            int* @out);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_ui_set_dropdown(
            IntPtr api,
            uint e,
            int index);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr jce_script_api_ui_get_input_text(
            IntPtr api,
            uint e);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_ui_set_input_text(
            IntPtr api,
            uint e,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? text);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_ui_get_scroll(
            IntPtr api,
            uint e,
            float* outXy);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_ui_set_scroll(
            IntPtr api,
            uint e,
            float x,
            float y);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern float jce_script_api_world_get_hour(
            IntPtr api);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_world_set_hour(
            IntPtr api,
            float hour);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_world_is_daytime(
            IntPtr api);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int jce_script_api_world_get_weather(
            IntPtr api);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern float jce_script_api_world_get_weather_intensity(
            IntPtr api);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern float jce_script_api_world_get_wind_speed(
            IntPtr api);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_request_scene(
            IntPtr api,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? scenePath);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_is_transitioning(
            IntPtr api);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_audio_play(
            IntPtr api,
            uint e);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_audio_stop(
            IntPtr api,
            uint e);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_audio_is_playing(
            IntPtr api,
            uint e);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_save_game(
            IntPtr api,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? path);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_load_game(
            IntPtr api,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? path);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int jce_script_api_overlap_sphere(
            IntPtr api,
            float x,
            float y,
            float z,
            float radius,
            uint layerMask,
            uint* @out,
            int max);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int jce_script_api_overlap_box(
            IntPtr api,
            float x,
            float y,
            float z,
            float hx,
            float hy,
            float hz,
            uint layerMask,
            uint* @out,
            int max);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_get_param(
            IntPtr api,
            uint e,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? name,
            int* outKind,
            double* outNumber,
            uint* outEntity);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr jce_script_api_get_param_text(
            IntPtr api,
            uint e,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? name);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern byte jce_script_api_curve_eval(
            IntPtr api,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? path,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? channel,
            double t,
            double* outValue);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int jce_script_api_vcam_activate(
            IntPtr api,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? name);
        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void jce_script_api_json_free(IntPtr api, IntPtr owned);
    }
}

/// <summary>The engine, as a C# script sees it. 101 calls, generated from the same manifest the other five languages read.</summary>
public static unsafe class Jce
{
    private static IntPtr _api;

    /// <summary>Point the surface at a VM's C ABI handle.  Called by
    /// JceEntityScript.Bind, never by a script.  A process hosts one
    /// engine, so one handle is enough — and a per-instance handle
    /// would be one more thing to get wrong at reload.</summary>
    internal static void Use(IntPtr api) { if (api != IntPtr.Zero) _api = api; }

    /// <summary>True once a VM has opened the C ABI.  Every call
    /// below is a no-op answering its zero value until then, which is
    /// the same thing a NULL host member does.</summary>
    public static bool IsOpen => _api != IntPtr.Zero;

        /// <summary>
        /// LOCAL position of `entity` -- its own translation, not composed up the parent chain. Absent when the entity has no transform. Use get_world_position for the composed pose. This doc said 'World-space' until 2026-08-26; the implementation always returned the local TRS (jce_scene_get_transform), and the wrong word was generated into all five language SDKs.
        /// </summary>

        public static bool TryGetPosition(uint e, out (float, float, float) value)
        {
            float* outXyz = stackalloc float[3];
            if (Interop.Fn.jce_script_api_get_position(_api, e, outXyz) == 0)
            { value = default; return false; }
            value = (outXyz[0], outXyz[1], outXyz[2]);
            return true;
        }


        public static void SetPosition(uint e, float x, float y, float z)
        {
            Interop.Fn.jce_script_api_set_position(_api, e, x, y, z);
        }


        public static bool TryGetRotation(uint e, out (float, float, float) value)
        {
            float* outEulerDeg = stackalloc float[3];
            if (Interop.Fn.jce_script_api_get_rotation(_api, e, outEulerDeg) == 0)
            { value = default; return false; }
            value = (outEulerDeg[0], outEulerDeg[1], outEulerDeg[2]);
            return true;
        }


        public static void SetRotation(uint e, float x, float y, float z)
        {
            Interop.Fn.jce_script_api_set_rotation(_api, e, x, y, z);
        }


        public static bool TryGetScale(uint e, out (float, float, float) value)
        {
            float* outXyz = stackalloc float[3];
            if (Interop.Fn.jce_script_api_get_scale(_api, e, outXyz) == 0)
            { value = default; return false; }
            value = (outXyz[0], outXyz[1], outXyz[2]);
            return true;
        }

        /// <summary>
        /// WORLD position of `entity`: its local TRS composed up the parent chain (jce_scene_get_world_matrix). Absent when the entity has no transform. Every parented rig -- arms, jaws, fingers, pads -- needs this rather than get_position.
        /// </summary>

        public static bool TryGetWorldPosition(uint e, out (float, float, float) value)
        {
            float* outXyz = stackalloc float[3];
            if (Interop.Fn.jce_script_api_get_world_position(_api, e, outXyz) == 0)
            { value = default; return false; }
            value = (outXyz[0], outXyz[1], outXyz[2]);
            return true;
        }


        public static void SetScale(uint e, float x, float y, float z)
        {
            Interop.Fn.jce_script_api_set_scale(_api, e, x, y, z);
        }

        /// <summary>
        /// The ONLY boolean argument on this surface that is type-checked; the other five accept any truthy value. The luaL_checktype is emitted from this entry's `strict` modifier -- no line citation, because the hand-written body that carried it is gone. test_strict_emits_a_type_check_only_for_the_named_parameter is what fails if the emitter drops it.
        /// </summary>

        public static bool SetParent(uint child, uint parent, bool preserveWorld)
        {
            return Interop.Fn.jce_script_api_set_parent(_api, child, parent, (byte)(preserveWorld ? 1 : 0)) != 0;
        }


        public static uint GetParent(uint child)
        {
            return Interop.Fn.jce_script_api_get_parent(_api, child);
        }


        public static bool IsKeyDown(int keycode)
        {
            return Interop.Fn.jce_script_api_is_key_down(_api, keycode) != 0;
        }


        public static uint FindWithTag(string? tag)
        {
            return Interop.Fn.jce_script_api_find_with_tag(_api, tag);
        }


        public static void Destroy(uint e)
        {
            Interop.Fn.jce_script_api_destroy(_api, e);
        }


        public static uint Spawn(string? prefabPath, float x, float y, float z)
        {
            return Interop.Fn.jce_script_api_spawn(_api, prefabPath, x, y, z);
        }


        public static (float, float) MoveAxis()
        {
            float* o = stackalloc float[2];
            Interop.Fn.jce_script_api_move_axis(_api, o);
            return (o[0], o[1]);
        }

        /// <summary>
        /// <para>The C ABI supplies button=0 (manifest bind_args), so it is not an argument.</para>
        /// </summary>

        public static bool JumpPressed(int button)
        {
            return Interop.Fn.jce_script_api_jump_pressed(_api, 0) != 0;
        }

        /// <summary>
        /// <para>The C ABI supplies button=1 (manifest bind_args), so it is not an argument.</para>
        /// </summary>

        public static bool Sprint(int button)
        {
            return Interop.Fn.jce_script_api_sprint(_api, 1) != 0;
        }

        /// <summary>
        /// <para>The C ABI supplies button=2 (manifest bind_args), so it is not an argument.</para>
        /// </summary>

        public static bool AttackPressed(int button)
        {
            return Interop.Fn.jce_script_api_attack_pressed(_api, 2) != 0;
        }


        public static void SetTimeScale(float scale)
        {
            Interop.Fn.jce_script_api_set_time_scale(_api, scale);
        }

        /// <summary>
        /// jce.pause() with no argument pauses; jce.pause(false) resumes.
        /// </summary>

        public static void Pause(bool paused)
        {
            Interop.Fn.jce_script_api_pause(_api, (byte)(paused ? 1 : 0));
        }


        public static void ShakeCamera(float amount)
        {
            Interop.Fn.jce_script_api_shake_camera(_api, amount);
        }


        public static void MusicSetIntensity(float intensity)
        {
            Interop.Fn.jce_script_api_music_set_intensity(_api, intensity);
        }


        public static float MusicGetIntensity()
        {
            return Interop.Fn.jce_script_api_music_get_intensity(_api);
        }

        /// <summary>
        /// Absolute playhead time of the quantized switch; negative on miss or no track, which is why the no-host value is -1 and not 0.
        /// <para>With the host member absent the C ABI answers the manifest's absent_value, not zero.</para>
        /// </summary>

        public static float MusicRequestTransition(int toSegment)
        {
            return Interop.Fn.jce_script_api_music_request_transition(_api, toSegment);
        }


        public static bool GasActivate(uint e, uint abilityId)
        {
            return Interop.Fn.jce_script_api_gas_activate(_api, e, abilityId) != 0;
        }


        public static bool TryGasGet(uint e, string? attrName, out float value)
        {
            float outValue = default;
            if (Interop.Fn.jce_script_api_gas_get(_api, e, attrName, &outValue) == 0)
            { value = default; return false; }
            value = outValue;
            return true;
        }


        public static bool GasApply(uint e, string? attrName, int op, float magnitude, float durationSeconds)
        {
            return Interop.Fn.jce_script_api_gas_apply(_api, e, attrName, op, magnitude, durationSeconds) != 0;
        }

        /// <summary>
        /// 8 values on a hit; a MISS pushes integer 0, not nil -- scripts branch on `e == 0`.
        /// <para>Lua answers 0 on a miss because it must push something; this returns false and leaves the out value zeroed. Same information, one way to test it.</para>
        /// </summary>

        public static bool TryRaycast(ReadOnlySpan<float> origin, ReadOnlySpan<float> dir, float maxDist, out (uint, float, float, float, float, float, float, float) value)
        {
            if (origin.Length < 3)
                throw new ArgumentException("needs 3 values", nameof(origin));
            if (dir.Length < 3)
                throw new ArgumentException("needs 3 values", nameof(dir));
            fixed (float* p_origin = origin)
            fixed (float* p_dir = dir)
            {
                JceScriptRaycastHit @out = default;
                if (Interop.Fn.jce_script_api_raycast(_api, p_origin, p_dir, maxDist, &@out) == 0)
                { value = default; return false; }
                value = (@out.entity, @out.point[0], @out.point[1], @out.point[2], @out.normal[0], @out.normal[1], @out.normal[2], @out.distance);
                return true;
            }
        }

        /// <summary>
        /// Closest hit along the ray, honouring a layer mask and the trigger skip -- 8 values on a hit; a MISS pushes integer 0, not nil, so scripts branch on `e == 0`, the same as jce.raycast. layer_mask 0 means every layer and hit_triggers defaults to false, so the common call stays origin/dir/distance and the filter is what you add when you need it. hit_triggers is separate from the mask because a trigger volume is not a layer: collapsing them would make 'ignore triggers on layer 3' inexpressible.
        /// <para>Lua answers 0 on a miss because it must push something; this returns false and leaves the out value zeroed. Same information, one way to test it.</para>
        /// </summary>

        public static bool TryRaycastFiltered(ReadOnlySpan<float> origin, ReadOnlySpan<float> dir, float maxDist, uint layerMask, bool hitTriggers, out (uint, float, float, float, float, float, float, float) value)
        {
            if (origin.Length < 3)
                throw new ArgumentException("needs 3 values", nameof(origin));
            if (dir.Length < 3)
                throw new ArgumentException("needs 3 values", nameof(dir));
            fixed (float* p_origin = origin)
            fixed (float* p_dir = dir)
            {
                JceScriptRaycastHit @out = default;
                if (Interop.Fn.jce_script_api_raycast_filtered(_api, p_origin, p_dir, maxDist, layerMask, (byte)(hitTriggers ? 1 : 0), &@out) == 0)
                { value = default; return false; }
                value = (@out.entity, @out.point[0], @out.point[1], @out.point[2], @out.normal[0], @out.normal[1], @out.normal[2], @out.distance);
                return true;
            }
        }

        /// <summary>
        /// Every entity the ray passes through, as one array sorted near to far. layer_mask 0 means every layer; hit_triggers defaults to false. Returns ENTITIES rather than full hit records because the eight-value hit does not survive as an array shape across seven languages without inventing a per-language container -- re-query a specific one with jce.raycast_filtered when you need its point and normal.
        /// </summary>

        public static uint[] RaycastAll(ReadOnlySpan<float> origin, ReadOnlySpan<float> dir, float maxDist, uint layerMask, bool hitTriggers)
        {
            if (origin.Length < 3)
                throw new ArgumentException("needs 3 values", nameof(origin));
            if (dir.Length < 3)
                throw new ArgumentException("needs 3 values", nameof(dir));
            fixed (float* p_origin = origin)
            fixed (float* p_dir = dir)
            {
                uint* found = stackalloc uint[256];
                int n = Interop.Fn.jce_script_api_raycast_all(_api, p_origin, p_dir, maxDist, layerMask, (byte)(hitTriggers ? 1 : 0), found, 256);
                if (n <= 0) return Array.Empty<uint>();
                var r = new uint[n];
                for (int i = 0; i < n; ++i) r[i] = found[i];
                return r;
            }
        }


        public static void ApplyImpulse(uint e, float x, float y, float z)
        {
            Interop.Fn.jce_script_api_apply_impulse(_api, e, x, y, z);
        }


        public static void SetVelocity(uint e, float x, float y, float z)
        {
            Interop.Fn.jce_script_api_set_velocity(_api, e, x, y, z);
        }


        public static void AnimSetFloat(uint e, string? name, float v)
        {
            Interop.Fn.jce_script_api_anim_set_float(_api, e, name, v);
        }


        public static void AnimSetInt(uint e, string? name, int v)
        {
            Interop.Fn.jce_script_api_anim_set_int(_api, e, name, v);
        }


        public static void AnimSetBool(uint e, string? name, bool v)
        {
            Interop.Fn.jce_script_api_anim_set_bool(_api, e, name, (byte)(v ? 1 : 0));
        }


        public static void AnimSetTrigger(uint e, string? name)
        {
            Interop.Fn.jce_script_api_anim_set_trigger(_api, e, name);
        }


        public static bool IsActionDown(string? name)
        {
            return Interop.Fn.jce_script_api_is_action_down(_api, name) != 0;
        }


        public static bool IsActionPressed(string? name)
        {
            return Interop.Fn.jce_script_api_is_action_pressed(_api, name) != 0;
        }


        public static float GetAxis(string? name)
        {
            return Interop.Fn.jce_script_api_get_axis(_api, name);
        }


        public static (float, float) GetPointerDelta()
        {
            float* o = stackalloc float[2];
            Interop.Fn.jce_script_api_get_pointer_delta(_api, o);
            return (o[0], o[1]);
        }


        public static float GetPointerWheel()
        {
            return Interop.Fn.jce_script_api_get_pointer_wheel(_api);
        }


        public static bool IsPointerDown(int button)
        {
            return Interop.Fn.jce_script_api_is_pointer_down(_api, button) != 0;
        }

        /// <summary>
        /// A host returning a negative count is clamped to 0 so `for i = 1, jce.get_touch_count()` cannot underflow.
        /// <para>The C ABI clamps the result to a minimum of 0.</para>
        /// </summary>

        public static int GetTouchCount()
        {
            return Interop.Fn.jce_script_api_get_touch_count(_api);
        }

        /// <summary>
        /// 1-based Lua index mapped to 0-based C; an index below 1 returns nil without calling the host.
        /// <para>index is 1-based, as in Lua and Python; below 1 answers without calling the host. Kept 1-based ON PURPOSE: it is a contract value, not a rendering.</para>
        /// </summary>

        public static bool TryGetTouch(int index, out (ulong, float, float, float) value)
        {
            if (index < 1) { value = default; return false; }
            ulong id = default;
            float x = default;
            float y = default;
            float pressure = default;
            if (Interop.Fn.jce_script_api_get_touch(_api, (int)(index - 1), &id, &x, &y, &pressure) == 0)
            { value = default; return false; }
            value = (id, x, y, pressure);
            return true;
        }

        /// <summary>
        /// Passthrough is the contract, not a fallback: an unlocalized build shows readable keys instead of blank UI.
        /// <para>With the host member absent the C ABI answers the manifest's absent_value, not zero.</para>
        /// </summary>

        public static string Tr(string? key)
        {
            IntPtr s = Interop.Fn.jce_script_api_tr(_api, key);
            return s == IntPtr.Zero ? string.Empty
                : (Marshal.PtrToStringUTF8(s) ?? string.Empty);
        }


        public static string GetLocale()
        {
            IntPtr s = Interop.Fn.jce_script_api_get_locale(_api);
            return s == IntPtr.Zero ? string.Empty
                : (Marshal.PtrToStringUTF8(s) ?? string.Empty);
        }


        public static void SetLocale(string? locale)
        {
            Interop.Fn.jce_script_api_set_locale(_api, locale);
        }


        public static bool TryGetVelocity(uint e, out (float, float, float) value)
        {
            float* @out = stackalloc float[3];
            if (Interop.Fn.jce_script_api_get_velocity(_api, e, @out) == 0)
            { value = default; return false; }
            value = (@out[0], @out[1], @out[2]);
            return true;
        }


        public static void VehicleSetInput(uint e, float throttle, float brake, float steer)
        {
            Interop.Fn.jce_script_api_vehicle_set_input(_api, e, throttle, brake, steer);
        }


        public static float VehicleGetSpeed(uint e)
        {
            return Interop.Fn.jce_script_api_vehicle_get_speed(_api, e);
        }


        public static (float, float, float) GetMove()
        {
            float* o = stackalloc float[3];
            Interop.Fn.jce_script_api_get_move(_api, o);
            return (o[0], o[1], o[2]);
        }


        public static bool TryUiGetSlider(uint e, out float value)
        {
            float @out = default;
            if (Interop.Fn.jce_script_api_ui_get_slider(_api, e, &@out) == 0)
            { value = default; return false; }
            value = @out;
            return true;
        }


        public static void UiSetSlider(uint e, float v)
        {
            Interop.Fn.jce_script_api_ui_set_slider(_api, e, v);
        }


        public static bool TryUiGetProgress(uint e, out float value)
        {
            float @out = default;
            if (Interop.Fn.jce_script_api_ui_get_progress(_api, e, &@out) == 0)
            { value = default; return false; }
            value = @out;
            return true;
        }


        public static void UiSetProgress(uint e, float v)
        {
            Interop.Fn.jce_script_api_ui_set_progress(_api, e, v);
        }


        public static bool TryUiGetToggle(uint e, out bool value)
        {
            byte @out = default;
            if (Interop.Fn.jce_script_api_ui_get_toggle(_api, e, &@out) == 0)
            { value = default; return false; }
            value = @out != 0;
            return true;
        }


        public static void UiSetToggle(uint e, bool v)
        {
            Interop.Fn.jce_script_api_ui_set_toggle(_api, e, (byte)(v ? 1 : 0));
        }


        public static void UiSetText(uint e, string? txt)
        {
            Interop.Fn.jce_script_api_ui_set_text(_api, e, txt);
        }


        public static void SendMessage(uint target, string? msg, double numberArg, string? strArg)
        {
            Interop.Fn.jce_script_api_send_message(_api, target, msg, numberArg, strArg);
        }


        public static void Broadcast(string? msg, double numberArg, string? strArg)
        {
            Interop.Fn.jce_script_api_broadcast(_api, msg, numberArg, strArg);
        }


        public static bool HasComponent(uint e, string? compName)
        {
            return Interop.Fn.jce_script_api_has_component(_api, e, compName) != 0;
        }


        public static bool IsComponentEnabled(uint e, string? compName)
        {
            return Interop.Fn.jce_script_api_is_component_enabled(_api, e, compName) != 0;
        }


        public static void SetComponentEnabled(uint e, string? compName, bool on)
        {
            Interop.Fn.jce_script_api_set_component_enabled(_api, e, compName, (byte)(on ? 1 : 0));
        }


        public static bool NetIsServer()
        {
            return Interop.Fn.jce_script_api_net_is_server(_api) != 0;
        }


        public static bool NetIsClient()
        {
            return Interop.Fn.jce_script_api_net_is_client(_api) != 0;
        }


        public static uint NetSpawn(string? prefabPath, float x, float y, float z)
        {
            return Interop.Fn.jce_script_api_net_spawn(_api, prefabPath, x, y, z);
        }


        public static bool RpcSend(uint e, string? @event, int target, string? payload)
        {
            return Interop.Fn.jce_script_api_rpc_send(_api, e, @event, target, payload) != 0;
        }


        public static void ParticleBurst(uint e, int count)
        {
            Interop.Fn.jce_script_api_particle_burst(_api, e, count);
        }


        public static void ParticleSetEmitting(uint e, bool on)
        {
            Interop.Fn.jce_script_api_particle_set_emitting(_api, e, (byte)(on ? 1 : 0));
        }


        public static void ParticleSetColor(uint e, float r, float g, float b)
        {
            Interop.Fn.jce_script_api_particle_set_color(_api, e, r, g, b);
        }

        /// <summary>
        /// first-or-nil AND a match count, so a strict scene director can reject duplicate authored names. IDENTICAL C signature to find_by_prefix and a DIFFERENT contract.
        /// </summary>

        public static (uint? First, int Count) FindByName(string? name)
        {
            uint* found = stackalloc uint[2];
            int n = Interop.Fn.jce_script_api_find_by_name(_api, name, found, 2);
            return (n > 0 ? found[0] : (uint?)null, n);
        }

        /// <summary>
        /// One Lua array. IDENTICAL C signature to find_by_name and a DIFFERENT contract.
        /// </summary>

        public static uint[] FindByPrefix(string? prefix)
        {
            uint* found = stackalloc uint[1024];
            int n = Interop.Fn.jce_script_api_find_by_prefix(_api, prefix, found, 1024);
            if (n <= 0) return Array.Empty<uint>();
            var r = new uint[n];
            for (int i = 0; i < n; ++i) r[i] = found[i];
            return r;
        }


        public static string? CompGet(uint e, string? type)
        {
            IntPtr s = Interop.Fn.jce_script_api_comp_get(_api, e, type);
            if (s == IntPtr.Zero) return null;
            string r = Marshal.PtrToStringUTF8(s) ?? string.Empty;
            Interop.Fn.jce_script_api_json_free(_api, s);
            return r;
        }


        public static bool CompSet(uint e, string? type, string? json)
        {
            return Interop.Fn.jce_script_api_comp_set(_api, e, type, json) != 0;
        }


        public static string? RenderGet()
        {
            IntPtr s = Interop.Fn.jce_script_api_render_get(_api);
            if (s == IntPtr.Zero) return null;
            string r = Marshal.PtrToStringUTF8(s) ?? string.Empty;
            Interop.Fn.jce_script_api_json_free(_api, s);
            return r;
        }


        public static bool RenderSet(string? json)
        {
            return Interop.Fn.jce_script_api_render_set(_api, json) != 0;
        }


        public static void AudioSetVolume(uint e, float volume)
        {
            Interop.Fn.jce_script_api_audio_set_volume(_api, e, volume);
        }

        /// <summary>
        /// Selected option INDEX of `entity`'s UIDropdown. Absent when the entity has no dropdown, so a script can tell 'no dropdown' from 'a dropdown reading 0'. The index and not the label: branching on which option is the common case, and a label would make it a string compare.
        /// </summary>

        public static bool TryUiGetDropdown(uint e, out int value)
        {
            int @out = default;
            if (Interop.Fn.jce_script_api_ui_get_dropdown(_api, e, &@out) == 0)
            { value = default; return false; }
            value = @out;
            return true;
        }

        /// <summary>
        /// Select an option by INDEX. Clamped into [0, option_count-1] rather than refused, the way ui_set_progress clamps and the way the scene loader clamps: the draw already clamps, so storing outside the range would make the component and the picture disagree.
        /// </summary>

        public static void UiSetDropdown(uint e, int index)
        {
            Interop.Fn.jce_script_api_ui_set_dropdown(_api, e, index);
        }

        /// <summary>
        /// Current text of `entity`'s UIInputField, or '' when it has none. The string is the component's own buffer and is valid until the next mutation of that entity -- the same contract tr() and get_locale() carry; every binding copies it and none may store it.
        /// </summary>

        public static string UiGetInputText(uint e)
        {
            IntPtr s = Interop.Fn.jce_script_api_ui_get_input_text(_api, e);
            return s == IntPtr.Zero ? string.Empty
                : (Marshal.PtrToStringUTF8(s) ?? string.Empty);
        }

        /// <summary>
        /// Replace the UIInputField's text. Truncated to the field's capacity and to char_limit when one is set -- the same cap the canvas applies to typed input, so a script write and a keystroke cannot disagree about what the field holds. A truncation is logged rather than silent.
        /// </summary>

        public static void UiSetInputText(uint e, string? text)
        {
            Interop.Fn.jce_script_api_ui_set_input_text(_api, e, text);
        }

        /// <summary>
        /// Scroll offset (x, y) of `entity`'s UIScrollView, in REFERENCE units -- what the component stores and what the wheel path clamps, not device px. Absent when the entity has no scroll view.
        /// </summary>

        public static bool TryUiGetScroll(uint e, out (float, float) value)
        {
            float* outXy = stackalloc float[2];
            if (Interop.Fn.jce_script_api_ui_get_scroll(_api, e, outXy) == 0)
            { value = default; return false; }
            value = (outXy[0], outXy[1]);
            return true;
        }

        /// <summary>
        /// Set the scroll offset in reference units. A disabled axis is pinned to 0 and each axis is clamped the way the wheel path clamps, so a script cannot push the offset somewhere a wheel could not; the canvas re-clamps against the resolved viewport on the next render.
        /// </summary>

        public static void UiSetScroll(uint e, float x, float y)
        {
            Interop.Fn.jce_script_api_ui_set_scroll(_api, e, x, y);
        }

        /// <summary>
        /// Live hour of day in [0, 24) -- what the sky is showing now, NOT the authored tod_hour seed a scene starts from.  Reading the seed would return the level's start-of-day forever while the sky moved.
        /// </summary>

        public static float WorldGetHour()
        {
            return Interop.Fn.jce_script_api_world_get_hour(_api);
        }

        /// <summary>
        /// Move the live clock, wrapping into [0, 24).  For 'sleep until dawn'.  The authored seed is untouched, so reloading the scene still starts where the designer set it.
        /// </summary>

        public static void WorldSetHour(float hour)
        {
            Interop.Fn.jce_script_api_world_set_hour(_api, hour);
        }

        /// <summary>
        /// True while the sun is above the horizon.  THE predicate for 'is it night?' -- every key-light chooser in the engine is required to agree on this one, so a script that rolled its own threshold would disagree with the lighting it can see.
        /// </summary>

        public static bool WorldIsDaytime()
        {
            return Interop.Fn.jce_script_api_world_is_daytime(_api) != 0;
        }

        /// <summary>
        /// Authored weather type: 0 clear, 1 rain, 2 snow.
        /// </summary>

        public static int WorldGetWeather()
        {
            return Interop.Fn.jce_script_api_world_get_weather(_api);
        }

        /// <summary>
        /// Authored weather intensity in [0, 1].
        /// </summary>

        public static float WorldGetWeatherIntensity()
        {
            return Interop.Fn.jce_script_api_world_get_weather_intensity(_api);
        }

        /// <summary>
        /// Instantaneous wind speed in m/s -- the sustained speed plus this moment's gust.  Do NOT key a cache on it: it changes every frame by design.  It is the same number the ocean spectrum and the vegetation shader read, so a script cannot disagree with what is on screen.
        /// </summary>

        public static float WorldGetWindSpeed()
        {
            return Interop.Fn.jce_script_api_world_get_wind_speed(_api);
        }


        public static bool RequestScene(string? scenePath)
        {
            return Interop.Fn.jce_script_api_request_scene(_api, scenePath) != 0;
        }


        public static bool IsTransitioning()
        {
            return Interop.Fn.jce_script_api_is_transitioning(_api) != 0;
        }


        public static bool AudioPlay(uint e)
        {
            return Interop.Fn.jce_script_api_audio_play(_api, e) != 0;
        }


        public static bool AudioStop(uint e)
        {
            return Interop.Fn.jce_script_api_audio_stop(_api, e) != 0;
        }


        public static bool AudioIsPlaying(uint e)
        {
            return Interop.Fn.jce_script_api_audio_is_playing(_api, e) != 0;
        }


        public static bool SaveGame(string? path)
        {
            return Interop.Fn.jce_script_api_save_game(_api, path) != 0;
        }


        public static bool LoadGame(string? path)
        {
            return Interop.Fn.jce_script_api_load_game(_api, path) != 0;
        }

        /// <summary>
        /// Entities whose collider overlaps the sphere, as one array. layer_mask 0 means all layers. Triggers are skipped. layer_mask is OPTIONAL: omitting it means every layer, which is what an explosion or a pickup check wants and keeps the common call to its coordinates and its size.
        /// </summary>

        public static uint[] OverlapSphere(float x, float y, float z, float radius, uint layerMask)
        {
            uint* found = stackalloc uint[256];
            int n = Interop.Fn.jce_script_api_overlap_sphere(_api, x, y, z, radius, layerMask, found, 256);
            if (n <= 0) return Array.Empty<uint>();
            var r = new uint[n];
            for (int i = 0; i < n; ++i) r[i] = found[i];
            return r;
        }

        /// <summary>
        /// Entities whose collider overlaps the axis-aligned box (half-extents), as one array. layer_mask 0 means all layers. layer_mask is OPTIONAL: omitting it means every layer, which is what an explosion or a pickup check wants and keeps the common call to its coordinates and its size.
        /// </summary>

        public static uint[] OverlapBox(float x, float y, float z, float hx, float hy, float hz, uint layerMask)
        {
            uint* found = stackalloc uint[256];
            int n = Interop.Fn.jce_script_api_overlap_box(_api, x, y, z, hx, hy, hz, layerMask, found, 256);
            if (n <= 0) return Array.Empty<uint>();
            var r = new uint[n];
            for (int i = 0; i < n; ++i) r[i] = found[i];
            return r;
        }

        /// <summary>
        /// Returns kind, number, entity for an AUTHORED script parameter -- Unity's [SerializeField], Godot's @export.  Returns nil when the entity has no script component, when no parameter of that name is authored, or when the name is empty: three absences a script cannot act differently on, so `jce.get_param(e, 'speed') or 3.0` reads the way an author expects.
        /// </summary>

        public static bool TryGetParam(uint e, string? name, out (int, double, uint) value)
        {
            int outKind = default;
            double outNumber = default;
            uint outEntity = default;
            if (Interop.Fn.jce_script_api_get_param(_api, e, name, &outKind, &outNumber, &outEntity) == 0)
            { value = default; return false; }
            value = (outKind, outNumber, outEntity);
            return true;
        }

        /// <summary>
        /// The TEXT value of an authored script parameter, or '' when the entity has no script component, no parameter of that name, or one that is not text. Empty rather than nil for the same reason ui_get_input_text is empty: a script comparing strings should not have to test for nil first. The string is the component's own buffer -- copy it if you keep it.
        /// </summary>

        public static string GetParamText(uint e, string? name)
        {
            IntPtr s = Interop.Fn.jce_script_api_get_param_text(_api, e, name);
            return s == IntPtr.Zero ? string.Empty
                : (Marshal.PtrToStringUTF8(s) ?? string.Empty);
        }

        /// <summary>
        /// Sample an AUTHORED curve -- the documents the editor's Curve Editor writes, which nothing could read until this binding existed. Unity's AnimationCurve shape: the curve is a designer-authored function and the script decides what it means, so the engine never has to invent what a curve DRIVES. Returns nil when the path does not resolve, the document does not parse, the named channel is absent, or that channel has no keys -- so a curve that genuinely evaluates to 0 and a curve that is not there are never one reading, and `jce.curve_eval(p, 'kick', t) or 0.0` reads the way an author expects. An empty channel name means the FIRST channel, which is a different request from a name that is not there. The parsed curve is cached per runtime, so a call inside on_update costs a name compare, not a JSON parse.
        /// </summary>

        public static bool TryCurveEval(string? path, string? channel, double t, out double value)
        {
            double outValue = default;
            if (Interop.Fn.jce_script_api_curve_eval(_api, path, channel, t, &outValue) == 0)
            { value = default; return false; }
            value = outValue;
            return true;
        }

        /// <summary>
        /// Cut to the virtual camera with this name, ahead of priority.  Returns 1 when the name resolves to a camera that is active and enabled, 0 otherwise -- the request is recorded either way, so naming a camera in a streaming cell that has not loaded yet does not silently become 'whatever priority says'.  Pass an empty string to clear it and hand the decision back to priority.  It does NOT rewrite the authored components: the override lives in the vcam system, so a cutscene cannot bake its camera choice into the level file.
        /// </summary>

        public static int VcamActivate(string? name)
        {
            return Interop.Fn.jce_script_api_vcam_activate(_api, name);
        }
}
