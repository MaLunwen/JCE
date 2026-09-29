# jce_script/_generated.py — GENERATED. DO NOT EDIT.
#
#   python tools/scriptgen/gen_script_bindings.py --write
#
# Source of truth: struct JceScriptHost in
# engine/include/jce/middleware/script/jce_script.h joined with the exposure
# decisions in engine/src/middleware/script/script_exposure.json, published as
# contracts/script-api.json.  The transport is the C ABI shared library
# in scripting/c_abi/, bound with ctypes — no C compilation, no dependency.
#
# EVERY entry point is resolved BY NAME (_decl below), never by ordinal and
# never by slot.  script-api.json's own _note explains why: the append-only
# rule plus the min(caller, engine) copy over a zeroed host is what makes a
# short host safe, and anything keyed off a position defeats it.  A missing
# export raises at bind time naming the symbol, rather than at first call
# through a NULL — the same reason tests/scripting/c_abi's must_sym() exists.
#
# ABSENT VALUES: `None` is this module's spelling of Lua's `nil` and is used
# for nothing else.  The rule and its derivation are in
# tools/scriptgen/emit_python.py's docstring; the cross-language differential
# in tests/scripting/python/ is what holds it.
#
# NOT EXPOSED — the manifest's seven hand-written entries, as a
# class, with its own reason text, plus json_null:
#   line_set_points
#       marshalling, not glue: the host takes a packed `const float *xyz, int
#       count` and no generated shape reads a Lua array into one. Hand-written
#       so the table walk, the JCE_LINE_MAX_POINTS clamp and the returned
#       stored-count live in one place. Exists because the comp_set/JSON route
#       is O(n^2): cJSON resolves each flat px/py/pz key by walking the
#       object's child list (jce_scene_components_render.c parse_line_renderer)
#   log
#       no-host fallback: the LOG_INFO else-branch at jce_script.c:63 is the
#       only one among the 78
#   asset_read_text
#       policy, not glue: script_virtual_asset_path_valid (:67, called :103),
#       the 1 MiB JCE_SCRIPT_TEXT_ASSET_MAX_BYTES cap (jce_script.h:114),
#       jce_free on every exit path. A generated read_file template is a
#       sandbox escape (P0-2)
#   asset_read_json
#       all of asset_read_text (the same script_virtual_asset_path_valid at
#       :220) plus a depth- and node-capped JSON walk, non-finite rejection,
#       the json_null sentinel and distinct string error codes (P0-2)
#   play_sound
#       arity dispatch across two members: lua_gettop at :264 routes to
#       play_sound_spatial (:287) or play_sound (:290); its own comment at :281
#       concedes top == 3 is ambiguous
#   start_coroutine
#       Lua VM machinery: lua_newthread / lua_xmove / luaL_ref / lua_resume;
#       owns s->coros[]
#   wait_seconds
#       lua_yield into the same scheduler
#   stop_coroutine
#       scans s->coros[]
#   json_null
#       no producer on this side: the only thing that yields the sentinel is
#       asset_read_json, which is hand-written and excluded from the C ABI, and
#       owned_string_release returns raw JSON TEXT. A sentinel no reader can
#       return is a contract nothing enforces.



from __future__ import annotations

import ctypes
from typing import Sequence

__all__ = [
    "Api",
    "ENTRY_NAMES",
    "ENTRY_COUNT",
    "SCRIPT_API_VERSION",
    "NOT_EXPOSED",
    "OWNED_STRING_INITIAL_CAPACITY",
    "OWNED_STRING_MAX_ATTEMPTS",
    "MissingExportError",
]


class MissingExportError(AttributeError):
    """A jce_script_api_* entry point is not in the loaded library.

    Raised at bind time, from _decl, with the symbol named.  A ctypes CDLL
    hands back a callable for any attribute it can resolve and raises
    AttributeError otherwise; catching it here turns "the library is an older
    build" into one legible message instead of an AttributeError from inside a
    generated method."""


def _decl(lib, name: str, restype, argtypes):
    try:
        fn = getattr(lib, name)
    except AttributeError:
        raise MissingExportError(
            f"{name} is not exported by the loaded jce_script_api library; "
            f"it was generated for script_api_version "
            f"{SCRIPT_API_VERSION}") from None
    fn.restype = restype
    fn.argtypes = argtypes
    return fn


def _s(v):
    """A Python str as the C ABI's `const char *`; None stays NULL.

    None is passed through rather than encoded because two manifest entries
    (send_message, broadcast) carry `optional: {"str_arg": null}`, whose Lua
    reading is `lua_isstring(...) ? lua_tostring(...) : NULL` — an explicit
    absent string, not an empty one."""
    if v is None:
        return None
    if isinstance(v, bytes):
        return v
    return str(v).encode("utf-8")


def _f(v, n: int, arr, what: str):
    """A length-checked `const float[n]` input.

    Lua reads these with n separate luaL_checknumber calls and raises when the
    caller passed fewer; a short sequence here would otherwise reach the host
    as zero-filled tail, which is a wrong answer rather than an error."""
    seq = tuple(v)
    if len(seq) != n:
        raise ValueError(f"{what} takes exactly {n} numbers, got {len(seq)}")
    return arr(*seq)


# How many bytes the first owned-string call offers the C ABI.
#
# THE C ABI HAS NO WAY TO ASK FOR A LENGTH WITHOUT PRODUCING THE STRING: its
# owned_string_release forwarder calls the host, copies into the caller's
# buffer, and RELEASES — so a call that finds the buffer too small and retries
# calls the host a SECOND time and frees a SECOND time.  Lua, which receives
# the host's pointer directly, always calls once.  That divergence is real,
# documented, and pinned by differential.py's check_long_string_retry, which
# asserts the two call counts are exactly 1 and 2 and that every copy-out
# released; this constant is set high enough that component and render JSON fit
# in one call.
OWNED_STRING_INITIAL_CAPACITY = 8192

# A host whose string keeps growing between calls must not spin forever.
OWNED_STRING_MAX_ATTEMPTS = 4


def _owned(fn, args, what: str):
    cap = OWNED_STRING_INITIAL_CAPACITY
    for _ in range(OWNED_STRING_MAX_ATTEMPTS):
        buf = ctypes.create_string_buffer(cap)
        n = fn(*args, buf, cap)
        if n < 0:
            return None
        if n < cap:
            return buf.value.decode("utf-8", "replace")
        cap = n + 1
    raise RuntimeError(
        f"{what}: the host's string grew on every one of "
        f"{OWNED_STRING_MAX_ATTEMPTS} attempts")

SCRIPT_API_VERSION = 1
ENTRY_COUNT = 101

ENTRY_NAMES = (
    "get_position",
    "set_position",
    "get_rotation",
    "set_rotation",
    "get_scale",
    "get_world_position",
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
    "raycast_filtered",
    "raycast_all",
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
    "ui_get_progress",
    "ui_set_progress",
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
    "overlap_box",
    "get_param",
    "get_param_text",
    "curve_eval",
    "vcam_activate",
)

# Every Lua table key this module deliberately does not carry, with the
# reason it does not.  A name may not leave the Lua surface and vanish
# from here silently; the differential's key-set case reads BOTH.
NOT_EXPOSED = {
    "line_set_points":
        "marshalling, not glue: the host takes a packed `const float *xyz, int count` and no generated shape reads a Lua array into one. Hand-written so the table walk, the JCE_LINE_MAX_POINTS clamp and the returned stored-count live in one place. Exists because the comp_set/JSON route is O(n^2): cJSON resolves each flat px/py/pz key by walking the object's child list (jce_scene_components_render.c parse_line_renderer)",
    "log":
        'no-host fallback: the LOG_INFO else-branch at jce_script.c:63 is the only one among the 78',
    "asset_read_text":
        'policy, not glue: script_virtual_asset_path_valid (:67, called :103), the 1 MiB JCE_SCRIPT_TEXT_ASSET_MAX_BYTES cap (jce_script.h:114), jce_free on every exit path. A generated read_file template is a sandbox escape (P0-2)',
    "asset_read_json":
        'all of asset_read_text (the same script_virtual_asset_path_valid at :220) plus a depth- and node-capped JSON walk, non-finite rejection, the json_null sentinel and distinct string error codes (P0-2)',
    "play_sound":
        'arity dispatch across two members: lua_gettop at :264 routes to play_sound_spatial (:287) or play_sound (:290); its own comment at :281 concedes top == 3 is ambiguous',
    "start_coroutine":
        'Lua VM machinery: lua_newthread / lua_xmove / luaL_ref / lua_resume; owns s->coros[]',
    "wait_seconds":
        'lua_yield into the same scheduler',
    "stop_coroutine":
        'scans s->coros[]',
    "json_null":
        'no producer on this side: the only thing that yields the sentinel is asset_read_json, which is hand-written and excluded from the C ABI, and owned_string_release returns raw JSON TEXT. A sentinel no reader can return is a contract nothing enforces.',
}

class _RaycastHit(ctypes.Structure):
    """JceScriptRaycastHit, flattened to the scalars Lua pushes.

    The field order IS the push order: emit_lua flattens this POD
    with the same scriptgen_core.flatten_pod call, so slot i here
    is slot i there.  `float point[3]` is emitted as three floats
    because C gives them identical layout, and the differential
    asserts ctypes.sizeof against the C sizeof rather than trusting
    that.
    """

    _fields_ = [
        ("entity", ctypes.c_uint64),
        ("point_0", ctypes.c_float),
        ("point_1", ctypes.c_float),
        ("point_2", ctypes.c_float),
        ("normal_0", ctypes.c_float),
        ("normal_1", ctypes.c_float),
        ("normal_2", ctypes.c_float),
        ("distance", ctypes.c_float),
    ]


class _Entries:
    """Every C ABI entry point, resolved by name once per library."""

    __slots__ = (
        "_meta_version",
        "_meta_open",
        "_meta_close",
        "get_position",
        "set_position",
        "get_rotation",
        "set_rotation",
        "get_scale",
        "get_world_position",
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
        "raycast_filtered",
        "raycast_all",
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
        "ui_get_progress",
        "ui_set_progress",
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
        "overlap_box",
        "get_param",
        "get_param_text",
        "curve_eval",
        "vcam_activate",
    )

    def __init__(self, lib: ctypes.CDLL) -> None:
        self._meta_version = _decl(lib, "jce_script_api_version", ctypes.c_uint32, [])
        self._meta_open = _decl(lib, "jce_script_api_open", ctypes.c_void_p,
            [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32])
        self._meta_close = _decl(lib, "jce_script_api_close", None, [ctypes.c_void_p])
        self.get_position = _decl(
            lib, "jce_script_api_get_position", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_float * 3])
        self.set_position = _decl(
            lib, "jce_script_api_set_position", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_float, ctypes.c_float, ctypes.c_float])
        self.get_rotation = _decl(
            lib, "jce_script_api_get_rotation", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_float * 3])
        self.set_rotation = _decl(
            lib, "jce_script_api_set_rotation", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_float, ctypes.c_float, ctypes.c_float])
        self.get_scale = _decl(
            lib, "jce_script_api_get_scale", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_float * 3])
        self.get_world_position = _decl(
            lib, "jce_script_api_get_world_position", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_float * 3])
        self.set_scale = _decl(
            lib, "jce_script_api_set_scale", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_float, ctypes.c_float, ctypes.c_float])
        self.set_parent = _decl(
            lib, "jce_script_api_set_parent", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_bool])
        self.get_parent = _decl(
            lib, "jce_script_api_get_parent", ctypes.c_uint64,
            [ctypes.c_void_p, ctypes.c_uint64])
        self.is_key_down = _decl(
            lib, "jce_script_api_is_key_down", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_int])
        self.find_with_tag = _decl(
            lib, "jce_script_api_find_with_tag", ctypes.c_uint64,
            [ctypes.c_void_p, ctypes.c_char_p])
        self.destroy = _decl(
            lib, "jce_script_api_destroy", None,
            [ctypes.c_void_p, ctypes.c_uint64])
        self.spawn = _decl(
            lib, "jce_script_api_spawn", ctypes.c_uint64,
            [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_float, ctypes.c_float, ctypes.c_float])
        self.move_axis = _decl(
            lib, "jce_script_api_move_axis", None,
            [ctypes.c_void_p, ctypes.c_float * 2])
        self.jump_pressed = _decl(
            lib, "jce_script_api_jump_pressed", ctypes.c_bool,
            [ctypes.c_void_p])
        self.sprint = _decl(
            lib, "jce_script_api_sprint", ctypes.c_bool,
            [ctypes.c_void_p])
        self.attack_pressed = _decl(
            lib, "jce_script_api_attack_pressed", ctypes.c_bool,
            [ctypes.c_void_p])
        self.set_time_scale = _decl(
            lib, "jce_script_api_set_time_scale", None,
            [ctypes.c_void_p, ctypes.c_float])
        self.pause = _decl(
            lib, "jce_script_api_pause", None,
            [ctypes.c_void_p, ctypes.c_bool])
        self.shake_camera = _decl(
            lib, "jce_script_api_shake_camera", None,
            [ctypes.c_void_p, ctypes.c_float])
        self.music_set_intensity = _decl(
            lib, "jce_script_api_music_set_intensity", None,
            [ctypes.c_void_p, ctypes.c_float])
        self.music_get_intensity = _decl(
            lib, "jce_script_api_music_get_intensity", ctypes.c_float,
            [ctypes.c_void_p])
        self.music_request_transition = _decl(
            lib, "jce_script_api_music_request_transition", ctypes.c_float,
            [ctypes.c_void_p, ctypes.c_int])
        self.gas_activate = _decl(
            lib, "jce_script_api_gas_activate", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint32])
        self.gas_get = _decl(
            lib, "jce_script_api_gas_get", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p, ctypes.POINTER(ctypes.c_float)])
        self.gas_apply = _decl(
            lib, "jce_script_api_gas_apply", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p, ctypes.c_int, ctypes.c_float, ctypes.c_float])
        self.raycast = _decl(
            lib, "jce_script_api_raycast", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_float * 3, ctypes.c_float * 3, ctypes.c_float, ctypes.POINTER(_RaycastHit)])
        self.raycast_filtered = _decl(
            lib, "jce_script_api_raycast_filtered", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_float * 3, ctypes.c_float * 3, ctypes.c_float, ctypes.c_uint32, ctypes.c_bool, ctypes.POINTER(_RaycastHit)])
        self.raycast_all = _decl(
            lib, "jce_script_api_raycast_all", ctypes.c_int,
            [ctypes.c_void_p, ctypes.c_float * 3, ctypes.c_float * 3, ctypes.c_float, ctypes.c_uint32, ctypes.c_bool, ctypes.POINTER(ctypes.c_uint64), ctypes.c_int])
        self.apply_impulse = _decl(
            lib, "jce_script_api_apply_impulse", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_float, ctypes.c_float, ctypes.c_float])
        self.set_velocity = _decl(
            lib, "jce_script_api_set_velocity", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_float, ctypes.c_float, ctypes.c_float])
        self.anim_set_float = _decl(
            lib, "jce_script_api_anim_set_float", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p, ctypes.c_float])
        self.anim_set_int = _decl(
            lib, "jce_script_api_anim_set_int", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p, ctypes.c_int])
        self.anim_set_bool = _decl(
            lib, "jce_script_api_anim_set_bool", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p, ctypes.c_bool])
        self.anim_set_trigger = _decl(
            lib, "jce_script_api_anim_set_trigger", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p])
        self.is_action_down = _decl(
            lib, "jce_script_api_is_action_down", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_char_p])
        self.is_action_pressed = _decl(
            lib, "jce_script_api_is_action_pressed", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_char_p])
        self.get_axis = _decl(
            lib, "jce_script_api_get_axis", ctypes.c_float,
            [ctypes.c_void_p, ctypes.c_char_p])
        self.get_pointer_delta = _decl(
            lib, "jce_script_api_get_pointer_delta", None,
            [ctypes.c_void_p, ctypes.c_float * 2])
        self.get_pointer_wheel = _decl(
            lib, "jce_script_api_get_pointer_wheel", ctypes.c_float,
            [ctypes.c_void_p])
        self.is_pointer_down = _decl(
            lib, "jce_script_api_is_pointer_down", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_int])
        self.get_touch_count = _decl(
            lib, "jce_script_api_get_touch_count", ctypes.c_int,
            [ctypes.c_void_p])
        self.get_touch = _decl(
            lib, "jce_script_api_get_touch", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float)])
        self.tr = _decl(
            lib, "jce_script_api_tr", ctypes.c_char_p,
            [ctypes.c_void_p, ctypes.c_char_p])
        self.get_locale = _decl(
            lib, "jce_script_api_get_locale", ctypes.c_char_p,
            [ctypes.c_void_p])
        self.set_locale = _decl(
            lib, "jce_script_api_set_locale", None,
            [ctypes.c_void_p, ctypes.c_char_p])
        self.get_velocity = _decl(
            lib, "jce_script_api_get_velocity", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_float * 3])
        self.vehicle_set_input = _decl(
            lib, "jce_script_api_vehicle_set_input", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_float, ctypes.c_float, ctypes.c_float])
        self.vehicle_get_speed = _decl(
            lib, "jce_script_api_vehicle_get_speed", ctypes.c_float,
            [ctypes.c_void_p, ctypes.c_uint64])
        self.get_move = _decl(
            lib, "jce_script_api_get_move", None,
            [ctypes.c_void_p, ctypes.c_float * 3])
        self.ui_get_slider = _decl(
            lib, "jce_script_api_ui_get_slider", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_float)])
        self.ui_set_slider = _decl(
            lib, "jce_script_api_ui_set_slider", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_float])
        self.ui_get_progress = _decl(
            lib, "jce_script_api_ui_get_progress", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_float)])
        self.ui_set_progress = _decl(
            lib, "jce_script_api_ui_set_progress", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_float])
        self.ui_get_toggle = _decl(
            lib, "jce_script_api_ui_get_toggle", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_bool)])
        self.ui_set_toggle = _decl(
            lib, "jce_script_api_ui_set_toggle", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_bool])
        self.ui_set_text = _decl(
            lib, "jce_script_api_ui_set_text", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p])
        self.send_message = _decl(
            lib, "jce_script_api_send_message", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p, ctypes.c_double, ctypes.c_char_p])
        self.broadcast = _decl(
            lib, "jce_script_api_broadcast", None,
            [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_double, ctypes.c_char_p])
        self.has_component = _decl(
            lib, "jce_script_api_has_component", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p])
        self.is_component_enabled = _decl(
            lib, "jce_script_api_is_component_enabled", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p])
        self.set_component_enabled = _decl(
            lib, "jce_script_api_set_component_enabled", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p, ctypes.c_bool])
        self.net_is_server = _decl(
            lib, "jce_script_api_net_is_server", ctypes.c_bool,
            [ctypes.c_void_p])
        self.net_is_client = _decl(
            lib, "jce_script_api_net_is_client", ctypes.c_bool,
            [ctypes.c_void_p])
        self.net_spawn = _decl(
            lib, "jce_script_api_net_spawn", ctypes.c_uint64,
            [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_float, ctypes.c_float, ctypes.c_float])
        self.rpc_send = _decl(
            lib, "jce_script_api_rpc_send", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p])
        self.particle_burst = _decl(
            lib, "jce_script_api_particle_burst", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_int])
        self.particle_set_emitting = _decl(
            lib, "jce_script_api_particle_set_emitting", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_bool])
        self.particle_set_color = _decl(
            lib, "jce_script_api_particle_set_color", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_float, ctypes.c_float, ctypes.c_float])
        self.find_by_name = _decl(
            lib, "jce_script_api_find_by_name", ctypes.c_int,
            [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint64), ctypes.c_int])
        self.find_by_prefix = _decl(
            lib, "jce_script_api_find_by_prefix", ctypes.c_int,
            [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint64), ctypes.c_int])
        self.comp_get = _decl(
            lib, "jce_script_api_comp_get", ctypes.c_int,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int])
        self.comp_set = _decl(
            lib, "jce_script_api_comp_set", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p, ctypes.c_char_p])
        self.render_get = _decl(
            lib, "jce_script_api_render_get", ctypes.c_int,
            [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int])
        self.render_set = _decl(
            lib, "jce_script_api_render_set", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_char_p])
        self.audio_set_volume = _decl(
            lib, "jce_script_api_audio_set_volume", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_float])
        self.ui_get_dropdown = _decl(
            lib, "jce_script_api_ui_get_dropdown", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_int)])
        self.ui_set_dropdown = _decl(
            lib, "jce_script_api_ui_set_dropdown", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_int])
        self.ui_get_input_text = _decl(
            lib, "jce_script_api_ui_get_input_text", ctypes.c_char_p,
            [ctypes.c_void_p, ctypes.c_uint64])
        self.ui_set_input_text = _decl(
            lib, "jce_script_api_ui_set_input_text", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p])
        self.ui_get_scroll = _decl(
            lib, "jce_script_api_ui_get_scroll", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_float * 2])
        self.ui_set_scroll = _decl(
            lib, "jce_script_api_ui_set_scroll", None,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_float, ctypes.c_float])
        self.world_get_hour = _decl(
            lib, "jce_script_api_world_get_hour", ctypes.c_float,
            [ctypes.c_void_p])
        self.world_set_hour = _decl(
            lib, "jce_script_api_world_set_hour", None,
            [ctypes.c_void_p, ctypes.c_float])
        self.world_is_daytime = _decl(
            lib, "jce_script_api_world_is_daytime", ctypes.c_bool,
            [ctypes.c_void_p])
        self.world_get_weather = _decl(
            lib, "jce_script_api_world_get_weather", ctypes.c_int,
            [ctypes.c_void_p])
        self.world_get_weather_intensity = _decl(
            lib, "jce_script_api_world_get_weather_intensity", ctypes.c_float,
            [ctypes.c_void_p])
        self.world_get_wind_speed = _decl(
            lib, "jce_script_api_world_get_wind_speed", ctypes.c_float,
            [ctypes.c_void_p])
        self.request_scene = _decl(
            lib, "jce_script_api_request_scene", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_char_p])
        self.is_transitioning = _decl(
            lib, "jce_script_api_is_transitioning", ctypes.c_bool,
            [ctypes.c_void_p])
        self.audio_play = _decl(
            lib, "jce_script_api_audio_play", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64])
        self.audio_stop = _decl(
            lib, "jce_script_api_audio_stop", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64])
        self.audio_is_playing = _decl(
            lib, "jce_script_api_audio_is_playing", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64])
        self.save_game = _decl(
            lib, "jce_script_api_save_game", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_char_p])
        self.load_game = _decl(
            lib, "jce_script_api_load_game", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_char_p])
        self.overlap_sphere = _decl(
            lib, "jce_script_api_overlap_sphere", ctypes.c_int,
            [ctypes.c_void_p, ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint64), ctypes.c_int])
        self.overlap_box = _decl(
            lib, "jce_script_api_overlap_box", ctypes.c_int,
            [ctypes.c_void_p, ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint64), ctypes.c_int])
        self.get_param = _decl(
            lib, "jce_script_api_get_param", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_uint64)])
        self.get_param_text = _decl(
            lib, "jce_script_api_get_param_text", ctypes.c_char_p,
            [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p])
        self.curve_eval = _decl(
            lib, "jce_script_api_curve_eval", ctypes.c_bool,
            [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_double, ctypes.POINTER(ctypes.c_double)])
        self.vcam_activate = _decl(
            lib, "jce_script_api_vcam_activate", ctypes.c_int,
            [ctypes.c_void_p, ctypes.c_char_p])


class Api:
    """The scripting surface, over one open JceScriptApi handle.

    Construct through jce_script.open_host() or jce_script.attach();
    this class never opens or closes the handle it is given, because
    the process that created it owns it — the engine, in-process.
    """

    __slots__ = ("_h", "_f", "_lib")

    def __init__(self, handle: int, entries: _Entries,
                 lib: ctypes.CDLL) -> None:
        self._h = ctypes.c_void_p(handle)
        self._f = entries
        self._lib = lib

    @property
    def handle(self) -> int:
        """The raw JceScriptApi* this Api wraps."""
        return int(self._h.value or 0)

    def get_position(self, e: int) -> tuple[float, float, float] | None:
        """jce.get_position — shape: fallible_out, since 1.

        LOCAL position of `entity` -- its own translation, not composed up the
        parent chain. Absent when the entity has no transform. Use
        get_world_position for the composed pose. This doc said 'World-space'
        until 2026-08-26; the implementation always returned the local TRS
        (jce_scene_get_transform), and the wrong word was generated into all
        five language SDKs.
        """
        out_xyz = (ctypes.c_float * 3)()
        if not self._f.get_position(self._h, e, out_xyz):
            return None
        return (out_xyz[0], out_xyz[1], out_xyz[2],)

    def set_position(self, e: int, x: float, y: float, z: float) -> None:
        """jce.set_position — shape: void_call, since 1.
        """
        self._f.set_position(self._h, e, x, y, z)
        return None

    def get_rotation(self, e: int) -> tuple[float, float, float] | None:
        """jce.get_rotation — shape: fallible_out, since 1.
        """
        out_euler_deg = (ctypes.c_float * 3)()
        if not self._f.get_rotation(self._h, e, out_euler_deg):
            return None
        return (out_euler_deg[0], out_euler_deg[1], out_euler_deg[2],)

    def set_rotation(self, e: int, x: float, y: float, z: float) -> None:
        """jce.set_rotation — shape: void_call, since 1.
        """
        self._f.set_rotation(self._h, e, x, y, z)
        return None

    def get_scale(self, e: int) -> tuple[float, float, float] | None:
        """jce.get_scale — shape: fallible_out, since 1.
        """
        out_xyz = (ctypes.c_float * 3)()
        if not self._f.get_scale(self._h, e, out_xyz):
            return None
        return (out_xyz[0], out_xyz[1], out_xyz[2],)

    def get_world_position(self, e: int) -> tuple[float, float, float] | None:
        """jce.get_world_position — shape: fallible_out, since 1.

        WORLD position of `entity`: its local TRS composed up the parent chain
        (jce_scene_get_world_matrix). Absent when the entity has no transform.
        Every parented rig -- arms, jaws, fingers, pads -- needs this rather
        than get_position.
        """
        out_xyz = (ctypes.c_float * 3)()
        if not self._f.get_world_position(self._h, e, out_xyz):
            return None
        return (out_xyz[0], out_xyz[1], out_xyz[2],)

    def set_scale(self, e: int, x: float, y: float, z: float) -> None:
        """jce.set_scale — shape: void_call, since 1.
        """
        self._f.set_scale(self._h, e, x, y, z)
        return None

    def set_parent(self, child: int, parent: int, preserve_world: bool) -> bool:
        """jce.set_parent — shape: value_return, since 1.

        The ONLY boolean argument on this surface that is type-checked; the
        other five accept any truthy value. The luaL_checktype is emitted from
        this entry's `strict` modifier -- no line citation, because the
        hand-written body that carried it is gone.
        test_strict_emits_a_type_check_only_for_the_named_parameter is what
        fails if the emitter drops it.
        """
        if not isinstance(preserve_world, bool):
            raise TypeError('set_parent' + ' expects a bool for preserve_world, got ' + type(preserve_world).__name__)
        return self._f.set_parent(self._h, child, parent, preserve_world)

    def get_parent(self, child: int) -> int:
        """jce.get_parent — shape: value_return, since 1.
        """
        return self._f.get_parent(self._h, child)

    def is_key_down(self, keycode: int) -> bool:
        """jce.is_key_down — shape: value_return, since 1.
        """
        return self._f.is_key_down(self._h, keycode)

    def find_with_tag(self, tag: str) -> int:
        """jce.find_with_tag — shape: value_return, since 1.
        """
        return self._f.find_with_tag(self._h, _s(tag))

    def destroy(self, e: int) -> None:
        """jce.destroy — shape: void_call, since 1.
        """
        self._f.destroy(self._h, e)
        return None

    def spawn(self, prefab_path: str, x: float | None = 0.0, y: float | None = 0.0, z: float | None = 0.0) -> int:
        """jce.spawn — shape: value_return, since 1.
        """
        if x is None:
            x = 0.0
        if y is None:
            y = 0.0
        if z is None:
            z = 0.0
        return self._f.spawn(self._h, _s(prefab_path), x, y, z)

    def move_axis(self) -> tuple[float, float]:
        """jce.move_axis — shape: void_out_array, since 1.
        """
        out_xz = (ctypes.c_float * 2)()
        self._f.move_axis(self._h, out_xz)
        return (out_xz[0], out_xz[1],)

    def jump_pressed(self) -> bool:
        """jce.jump_pressed — shape: value_return, since 1.

        The C ABI supplies button=0 (manifest bind_args), so it is not an
        argument.
        """
        return self._f.jump_pressed(self._h)

    def sprint(self) -> bool:
        """jce.sprint — shape: value_return, since 1.

        The C ABI supplies button=1 (manifest bind_args), so it is not an
        argument.
        """
        return self._f.sprint(self._h)

    def attack_pressed(self) -> bool:
        """jce.attack_pressed — shape: value_return, since 1.

        The C ABI supplies button=2 (manifest bind_args), so it is not an
        argument.
        """
        return self._f.attack_pressed(self._h)

    def set_time_scale(self, scale: float) -> None:
        """jce.set_time_scale — shape: void_call, since 1.
        """
        self._f.set_time_scale(self._h, scale)
        return None

    def pause(self, paused: bool | None = True) -> None:
        """jce.pause — shape: void_call, since 1.

        jce.pause() with no argument pauses; jce.pause(false) resumes.
        """
        if paused is None:
            paused = True
        self._f.pause(self._h, bool(paused))
        return None

    def shake_camera(self, amount: float | None = 0.5) -> None:
        """jce.shake_camera — shape: void_call, since 1.
        """
        if amount is None:
            amount = 0.5
        self._f.shake_camera(self._h, amount)
        return None

    def music_set_intensity(self, intensity: float) -> None:
        """jce.music_set_intensity — shape: void_call, since 1.
        """
        self._f.music_set_intensity(self._h, intensity)
        return None

    def music_get_intensity(self) -> float:
        """jce.music_get_intensity — shape: value_return, since 1.
        """
        return self._f.music_get_intensity(self._h)

    def music_request_transition(self, to_segment: int) -> float:
        """jce.music_request_transition — shape: value_return, since 1.

        Absolute playhead time of the quantized switch; negative on miss or no
        track, which is why the no-host value is -1 and not 0.

        With the host member absent the C ABI answers the manifest's
        absent_value, not zero.
        """
        return self._f.music_request_transition(self._h, to_segment)

    def gas_activate(self, e: int, ability_id: int) -> bool:
        """jce.gas_activate — shape: value_return, since 1.
        """
        return self._f.gas_activate(self._h, e, ability_id)

    def gas_get(self, e: int, attr_name: str) -> tuple[float] | None:
        """jce.gas_get — shape: fallible_out, since 1.
        """
        out_value = ctypes.c_float()
        if not self._f.gas_get(self._h, e, _s(attr_name), ctypes.byref(out_value)):
            return None
        return (out_value.value,)

    def gas_apply(self, e: int, attr_name: str, op: int | None, magnitude: float, duration_seconds: float | None = 0.0) -> bool:
        """jce.gas_apply — shape: value_return, since 1.
        """
        if op is None:
            op = 0
        if duration_seconds is None:
            duration_seconds = 0.0
        return self._f.gas_apply(self._h, e, _s(attr_name), op, magnitude, duration_seconds)

    def raycast(self, origin: Sequence[float], dir: Sequence[float], max_dist: float) -> tuple[int, float, float, float, float, float, float, float] | int:
        """jce.raycast — shape: fallible_out, since 1.

        8 values on a hit; a MISS pushes integer 0, not nil -- scripts branch
        on `e == 0`.

        A miss answers 0, not None — scripts branch on it.
        """
        _origin = _f(origin, 3, ctypes.c_float * 3, 'raycast')
        _dir = _f(dir, 3, ctypes.c_float * 3, 'raycast')
        out = _RaycastHit()
        if not self._f.raycast(self._h, _origin, _dir, max_dist, ctypes.byref(out)):
            return 0
        return (out.entity, out.point_0, out.point_1, out.point_2, out.normal_0, out.normal_1, out.normal_2, out.distance,)

    def raycast_filtered(self, origin: Sequence[float], dir: Sequence[float], max_dist: float, layer_mask: int | None = 0, hit_triggers: bool | None = False) -> tuple[int, float, float, float, float, float, float, float] | int:
        """jce.raycast_filtered — shape: fallible_out, since 1.

        Closest hit along the ray, honouring a layer mask and the trigger skip
        -- 8 values on a hit; a MISS pushes integer 0, not nil, so scripts
        branch on `e == 0`, the same as jce.raycast. layer_mask 0 means every
        layer and hit_triggers defaults to false, so the common call stays
        origin/dir/distance and the filter is what you add when you need it.
        hit_triggers is separate from the mask because a trigger volume is not
        a layer: collapsing them would make 'ignore triggers on layer 3'
        inexpressible.

        A miss answers 0, not None — scripts branch on it.
        """
        _origin = _f(origin, 3, ctypes.c_float * 3, 'raycast_filtered')
        _dir = _f(dir, 3, ctypes.c_float * 3, 'raycast_filtered')
        if layer_mask is None:
            layer_mask = 0
        if hit_triggers is None:
            hit_triggers = False
        out = _RaycastHit()
        if not self._f.raycast_filtered(self._h, _origin, _dir, max_dist, layer_mask, bool(hit_triggers), ctypes.byref(out)):
            return 0
        return (out.entity, out.point_0, out.point_1, out.point_2, out.normal_0, out.normal_1, out.normal_2, out.distance,)

    def raycast_all(self, origin: Sequence[float], dir: Sequence[float], max_dist: float, layer_mask: int | None = 0, hit_triggers: bool | None = False) -> list[int]:
        """jce.raycast_all — shape: entity_table, since 1.

        Every entity the ray passes through, as one array sorted near to far.
        layer_mask 0 means every layer; hit_triggers defaults to false. Returns
        ENTITIES rather than full hit records because the eight-value hit does
        not survive as an array shape across seven languages without inventing
        a per-language container -- re-query a specific one with
        jce.raycast_filtered when you need its point and normal.
        """
        _origin = _f(origin, 3, ctypes.c_float * 3, 'raycast_all')
        _dir = _f(dir, 3, ctypes.c_float * 3, 'raycast_all')
        if layer_mask is None:
            layer_mask = 0
        if hit_triggers is None:
            hit_triggers = False
        found = (ctypes.c_uint64 * 256)()
        n = self._f.raycast_all(self._h, _origin, _dir, max_dist, layer_mask, bool(hit_triggers), found, 256)
        n = min(n, 256)
        return [found[i] for i in range(n)] if n > 0 else []

    def apply_impulse(self, e: int, x: float, y: float, z: float) -> None:
        """jce.apply_impulse — shape: void_call, since 1.
        """
        self._f.apply_impulse(self._h, e, x, y, z)
        return None

    def set_velocity(self, e: int, x: float, y: float, z: float) -> None:
        """jce.set_velocity — shape: void_call, since 1.
        """
        self._f.set_velocity(self._h, e, x, y, z)
        return None

    def anim_set_float(self, e: int, name: str, v: float) -> None:
        """jce.anim_set_float — shape: void_call, since 1.
        """
        self._f.anim_set_float(self._h, e, _s(name), v)
        return None

    def anim_set_int(self, e: int, name: str, v: int) -> None:
        """jce.anim_set_int — shape: void_call, since 1.
        """
        self._f.anim_set_int(self._h, e, _s(name), v)
        return None

    def anim_set_bool(self, e: int, name: str, v: bool) -> None:
        """jce.anim_set_bool — shape: void_call, since 1.
        """
        self._f.anim_set_bool(self._h, e, _s(name), bool(v))
        return None

    def anim_set_trigger(self, e: int, name: str) -> None:
        """jce.anim_set_trigger — shape: void_call, since 1.
        """
        self._f.anim_set_trigger(self._h, e, _s(name))
        return None

    def is_action_down(self, name: str) -> bool:
        """jce.is_action_down — shape: value_return, since 1.
        """
        return self._f.is_action_down(self._h, _s(name))

    def is_action_pressed(self, name: str) -> bool:
        """jce.is_action_pressed — shape: value_return, since 1.
        """
        return self._f.is_action_pressed(self._h, _s(name))

    def get_axis(self, name: str) -> float:
        """jce.get_axis — shape: value_return, since 1.
        """
        return self._f.get_axis(self._h, _s(name))

    def get_pointer_delta(self) -> tuple[float, float]:
        """jce.get_pointer_delta — shape: void_out_array, since 1.
        """
        out_xy = (ctypes.c_float * 2)()
        self._f.get_pointer_delta(self._h, out_xy)
        return (out_xy[0], out_xy[1],)

    def get_pointer_wheel(self) -> float:
        """jce.get_pointer_wheel — shape: value_return, since 1.
        """
        return self._f.get_pointer_wheel(self._h)

    def is_pointer_down(self, button: int) -> bool:
        """jce.is_pointer_down — shape: value_return, since 1.
        """
        return self._f.is_pointer_down(self._h, button)

    def get_touch_count(self) -> int:
        """jce.get_touch_count — shape: value_return, since 1.

        A host returning a negative count is clamped to 0 so `for i = 1,
        jce.get_touch_count()` cannot underflow.

        The C ABI clamps the result to a minimum of 0.
        """
        return self._f.get_touch_count(self._h)

    def get_touch(self, index: int) -> tuple[int, float, float, float] | None:
        """jce.get_touch — shape: fallible_out, since 1.

        1-based Lua index mapped to 0-based C; an index below 1 returns nil
        without calling the host.

        index is 1-based, as in Lua; below 1 returns without calling the host.
        """
        if index < 1:
            return None
        id = ctypes.c_uint64()
        x = ctypes.c_float()
        y = ctypes.c_float()
        pressure = ctypes.c_float()
        if not self._f.get_touch(self._h, index - 1, ctypes.byref(id), ctypes.byref(x), ctypes.byref(y), ctypes.byref(pressure)):
            return None
        return (id.value, x.value, y.value, pressure.value,)

    def tr(self, key: str) -> str:
        """jce.tr — shape: value_return, since 1.

        Passthrough is the contract, not a fallback: an unlocalized build shows
        readable keys instead of blank UI.

        With the host member absent the C ABI answers the manifest's
        absent_value, not zero.
        """
        v = self._f.tr(self._h, _s(key))
        return v.decode("utf-8", "replace") if v is not None else key

    def get_locale(self) -> str:
        """jce.get_locale — shape: value_return, since 1.
        """
        v = self._f.get_locale(self._h)
        return v.decode("utf-8", "replace") if v is not None else ""

    def set_locale(self, locale: str) -> None:
        """jce.set_locale — shape: void_call, since 1.
        """
        self._f.set_locale(self._h, _s(locale))
        return None

    def get_velocity(self, e: int) -> tuple[float, float, float] | None:
        """jce.get_velocity — shape: fallible_out, since 1.
        """
        out = (ctypes.c_float * 3)()
        if not self._f.get_velocity(self._h, e, out):
            return None
        return (out[0], out[1], out[2],)

    def vehicle_set_input(self, e: int, throttle: float, brake: float, steer: float) -> None:
        """jce.vehicle_set_input — shape: void_call, since 1.
        """
        self._f.vehicle_set_input(self._h, e, throttle, brake, steer)
        return None

    def vehicle_get_speed(self, e: int) -> float:
        """jce.vehicle_get_speed — shape: value_return, since 1.
        """
        return self._f.vehicle_get_speed(self._h, e)

    def get_move(self) -> tuple[float, float, float]:
        """jce.get_move — shape: void_out_array, since 1.
        """
        out = (ctypes.c_float * 3)()
        self._f.get_move(self._h, out)
        return (out[0], out[1], out[2],)

    def ui_get_slider(self, e: int) -> tuple[float] | None:
        """jce.ui_get_slider — shape: fallible_out, since 1.
        """
        out = ctypes.c_float()
        if not self._f.ui_get_slider(self._h, e, ctypes.byref(out)):
            return None
        return (out.value,)

    def ui_set_slider(self, e: int, v: float) -> None:
        """jce.ui_set_slider — shape: void_call, since 1.
        """
        self._f.ui_set_slider(self._h, e, v)
        return None

    def ui_get_progress(self, e: int) -> tuple[float] | None:
        """jce.ui_get_progress — shape: fallible_out, since 1.
        """
        out = ctypes.c_float()
        if not self._f.ui_get_progress(self._h, e, ctypes.byref(out)):
            return None
        return (out.value,)

    def ui_set_progress(self, e: int, v: float) -> None:
        """jce.ui_set_progress — shape: void_call, since 1.
        """
        self._f.ui_set_progress(self._h, e, v)
        return None

    def ui_get_toggle(self, e: int) -> tuple[bool] | None:
        """jce.ui_get_toggle — shape: fallible_out, since 1.
        """
        out = ctypes.c_bool()
        if not self._f.ui_get_toggle(self._h, e, ctypes.byref(out)):
            return None
        return (out.value,)

    def ui_set_toggle(self, e: int, v: bool) -> None:
        """jce.ui_set_toggle — shape: void_call, since 1.
        """
        self._f.ui_set_toggle(self._h, e, bool(v))
        return None

    def ui_set_text(self, e: int, txt: str) -> None:
        """jce.ui_set_text — shape: void_call, since 1.
        """
        self._f.ui_set_text(self._h, e, _s(txt))
        return None

    def send_message(self, target: int, msg: str, number_arg: float | None = 0.0, str_arg: str | None = None) -> None:
        """jce.send_message — shape: void_call, since 1.
        """
        if number_arg is None:
            number_arg = 0.0
        self._f.send_message(self._h, target, _s(msg), number_arg, _s(str_arg))
        return None

    def broadcast(self, msg: str, number_arg: float | None = 0.0, str_arg: str | None = None) -> None:
        """jce.broadcast — shape: void_call, since 1.
        """
        if number_arg is None:
            number_arg = 0.0
        self._f.broadcast(self._h, _s(msg), number_arg, _s(str_arg))
        return None

    def has_component(self, e: int, comp_name: str) -> bool:
        """jce.has_component — shape: value_return, since 1.
        """
        return self._f.has_component(self._h, e, _s(comp_name))

    def is_component_enabled(self, e: int, comp_name: str) -> bool:
        """jce.is_component_enabled — shape: value_return, since 1.
        """
        return self._f.is_component_enabled(self._h, e, _s(comp_name))

    def set_component_enabled(self, e: int, comp_name: str, on: bool) -> None:
        """jce.set_component_enabled — shape: void_call, since 1.
        """
        self._f.set_component_enabled(self._h, e, _s(comp_name), bool(on))
        return None

    def net_is_server(self) -> bool:
        """jce.net_is_server — shape: value_return, since 1.
        """
        return self._f.net_is_server(self._h)

    def net_is_client(self) -> bool:
        """jce.net_is_client — shape: value_return, since 1.
        """
        return self._f.net_is_client(self._h)

    def net_spawn(self, prefab_path: str, x: float, y: float, z: float) -> int:
        """jce.net_spawn — shape: value_return, since 1.
        """
        return self._f.net_spawn(self._h, _s(prefab_path), x, y, z)

    def rpc_send(self, e: int, event: str, target: int | None = 0, payload: str | None = None) -> bool:
        """jce.rpc_send — shape: value_return, since 1.
        """
        if target is None:
            target = 0
        return self._f.rpc_send(self._h, e, _s(event), target, _s(payload))

    def particle_burst(self, e: int, count: int) -> None:
        """jce.particle_burst — shape: void_call, since 1.
        """
        self._f.particle_burst(self._h, e, count)
        return None

    def particle_set_emitting(self, e: int, on: bool) -> None:
        """jce.particle_set_emitting — shape: void_call, since 1.
        """
        self._f.particle_set_emitting(self._h, e, bool(on))
        return None

    def particle_set_color(self, e: int, r: float, g: float, b: float) -> None:
        """jce.particle_set_color — shape: void_call, since 1.
        """
        self._f.particle_set_color(self._h, e, r, g, b)
        return None

    def find_by_name(self, name: str) -> tuple[int | None, int]:
        """jce.find_by_name — shape: first_and_count, since 1.

        first-or-nil AND a match count, so a strict scene director can reject
        duplicate authored names. IDENTICAL C signature to find_by_prefix and a
        DIFFERENT contract.
        """
        found = (ctypes.c_uint64 * 2)()
        n = self._f.find_by_name(self._h, _s(name), found, 2)
        return (found[0], n) if n > 0 else (None, n)

    def find_by_prefix(self, prefix: str) -> list[int]:
        """jce.find_by_prefix — shape: entity_table, since 1.

        One Lua array. IDENTICAL C signature to find_by_name and a DIFFERENT
        contract.
        """
        found = (ctypes.c_uint64 * 1024)()
        n = self._f.find_by_prefix(self._h, _s(prefix), found, 1024)
        n = min(n, 1024)
        return [found[i] for i in range(n)] if n > 0 else []

    def comp_get(self, e: int, type: str) -> str | None:
        """jce.comp_get — shape: owned_string_release, since 1.

        The C ABI releases the host's string through `json_free` before
        returning; nothing is owned here.
        """
        return _owned(self._f.comp_get, (self._h, e, _s(type),), 'comp_get')

    def comp_set(self, e: int, type: str, json: str) -> bool:
        """jce.comp_set — shape: value_return, since 1.
        """
        return self._f.comp_set(self._h, e, _s(type), _s(json))

    def render_get(self) -> str | None:
        """jce.render_get — shape: owned_string_release, since 1.

        The C ABI releases the host's string through `json_free` before
        returning; nothing is owned here.
        """
        return _owned(self._f.render_get, (self._h,), 'render_get')

    def render_set(self, json: str) -> bool:
        """jce.render_set — shape: value_return, since 1.
        """
        return self._f.render_set(self._h, _s(json))

    def audio_set_volume(self, e: int, volume: float) -> None:
        """jce.audio_set_volume — shape: void_call, since 1.
        """
        self._f.audio_set_volume(self._h, e, volume)
        return None

    def ui_get_dropdown(self, e: int) -> tuple[int] | None:
        """jce.ui_get_dropdown — shape: fallible_out, since 1.

        Selected option INDEX of `entity`'s UIDropdown. Absent when the entity
        has no dropdown, so a script can tell 'no dropdown' from 'a dropdown
        reading 0'. The index and not the label: branching on which option is
        the common case, and a label would make it a string compare.
        """
        out = ctypes.c_int()
        if not self._f.ui_get_dropdown(self._h, e, ctypes.byref(out)):
            return None
        return (out.value,)

    def ui_set_dropdown(self, e: int, index: int) -> None:
        """jce.ui_set_dropdown — shape: void_call, since 1.

        Select an option by INDEX. Clamped into [0, option_count-1] rather than
        refused, the way ui_set_progress clamps and the way the scene loader
        clamps: the draw already clamps, so storing outside the range would
        make the component and the picture disagree.
        """
        self._f.ui_set_dropdown(self._h, e, index)
        return None

    def ui_get_input_text(self, e: int) -> str:
        """jce.ui_get_input_text — shape: value_return, since 1.

        Current text of `entity`'s UIInputField, or '' when it has none. The
        string is the component's own buffer and is valid until the next
        mutation of that entity -- the same contract tr() and get_locale()
        carry; every binding copies it and none may store it.
        """
        v = self._f.ui_get_input_text(self._h, e)
        return v.decode("utf-8", "replace") if v is not None else ""

    def ui_set_input_text(self, e: int, text: str) -> None:
        """jce.ui_set_input_text — shape: void_call, since 1.

        Replace the UIInputField's text. Truncated to the field's capacity and
        to char_limit when one is set -- the same cap the canvas applies to
        typed input, so a script write and a keystroke cannot disagree about
        what the field holds. A truncation is logged rather than silent.
        """
        self._f.ui_set_input_text(self._h, e, _s(text))
        return None

    def ui_get_scroll(self, e: int) -> tuple[float, float] | None:
        """jce.ui_get_scroll — shape: fallible_out, since 1.

        Scroll offset (x, y) of `entity`'s UIScrollView, in REFERENCE units --
        what the component stores and what the wheel path clamps, not device
        px. Absent when the entity has no scroll view.
        """
        out_xy = (ctypes.c_float * 2)()
        if not self._f.ui_get_scroll(self._h, e, out_xy):
            return None
        return (out_xy[0], out_xy[1],)

    def ui_set_scroll(self, e: int, x: float, y: float) -> None:
        """jce.ui_set_scroll — shape: void_call, since 1.

        Set the scroll offset in reference units. A disabled axis is pinned to
        0 and each axis is clamped the way the wheel path clamps, so a script
        cannot push the offset somewhere a wheel could not; the canvas
        re-clamps against the resolved viewport on the next render.
        """
        self._f.ui_set_scroll(self._h, e, x, y)
        return None

    def world_get_hour(self) -> float:
        """jce.world_get_hour — shape: value_return, since 1.

        Live hour of day in [0, 24) -- what the sky is showing now, NOT the
        authored tod_hour seed a scene starts from. Reading the seed would
        return the level's start-of-day forever while the sky moved.
        """
        return self._f.world_get_hour(self._h)

    def world_set_hour(self, hour: float) -> None:
        """jce.world_set_hour — shape: void_call, since 1.

        Move the live clock, wrapping into [0, 24). For 'sleep until dawn'. The
        authored seed is untouched, so reloading the scene still starts where
        the designer set it.
        """
        self._f.world_set_hour(self._h, hour)
        return None

    def world_is_daytime(self) -> bool:
        """jce.world_is_daytime — shape: value_return, since 1.

        True while the sun is above the horizon. THE predicate for 'is it
        night?' -- every key-light chooser in the engine is required to agree
        on this one, so a script that rolled its own threshold would disagree
        with the lighting it can see.
        """
        return self._f.world_is_daytime(self._h)

    def world_get_weather(self) -> int:
        """jce.world_get_weather — shape: value_return, since 1.

        Authored weather type: 0 clear, 1 rain, 2 snow.
        """
        return self._f.world_get_weather(self._h)

    def world_get_weather_intensity(self) -> float:
        """jce.world_get_weather_intensity — shape: value_return, since 1.

        Authored weather intensity in [0, 1].
        """
        return self._f.world_get_weather_intensity(self._h)

    def world_get_wind_speed(self) -> float:
        """jce.world_get_wind_speed — shape: value_return, since 1.

        Instantaneous wind speed in m/s -- the sustained speed plus this
        moment's gust. Do NOT key a cache on it: it changes every frame by
        design. It is the same number the ocean spectrum and the vegetation
        shader read, so a script cannot disagree with what is on screen.
        """
        return self._f.world_get_wind_speed(self._h)

    def request_scene(self, scene_path: str) -> bool:
        """jce.request_scene — shape: value_return, since 1.
        """
        return self._f.request_scene(self._h, _s(scene_path))

    def is_transitioning(self) -> bool:
        """jce.is_transitioning — shape: value_return, since 1.
        """
        return self._f.is_transitioning(self._h)

    def audio_play(self, e: int) -> bool:
        """jce.audio_play — shape: value_return, since 1.
        """
        return self._f.audio_play(self._h, e)

    def audio_stop(self, e: int) -> bool:
        """jce.audio_stop — shape: value_return, since 1.
        """
        return self._f.audio_stop(self._h, e)

    def audio_is_playing(self, e: int) -> bool:
        """jce.audio_is_playing — shape: value_return, since 1.
        """
        return self._f.audio_is_playing(self._h, e)

    def save_game(self, path: str) -> bool:
        """jce.save_game — shape: value_return, since 1.
        """
        return self._f.save_game(self._h, _s(path))

    def load_game(self, path: str) -> bool:
        """jce.load_game — shape: value_return, since 1.
        """
        return self._f.load_game(self._h, _s(path))

    def overlap_sphere(self, x: float, y: float, z: float, radius: float, layer_mask: int | None = 0) -> list[int]:
        """jce.overlap_sphere — shape: entity_table, since 1.

        Entities whose collider overlaps the sphere, as one array. layer_mask 0
        means all layers. Triggers are skipped. layer_mask is OPTIONAL:
        omitting it means every layer, which is what an explosion or a pickup
        check wants and keeps the common call to its coordinates and its size.
        """
        if layer_mask is None:
            layer_mask = 0
        found = (ctypes.c_uint64 * 256)()
        n = self._f.overlap_sphere(self._h, x, y, z, radius, layer_mask, found, 256)
        n = min(n, 256)
        return [found[i] for i in range(n)] if n > 0 else []

    def overlap_box(self, x: float, y: float, z: float, hx: float, hy: float, hz: float, layer_mask: int | None = 0) -> list[int]:
        """jce.overlap_box — shape: entity_table, since 1.

        Entities whose collider overlaps the axis-aligned box (half-extents),
        as one array. layer_mask 0 means all layers. layer_mask is OPTIONAL:
        omitting it means every layer, which is what an explosion or a pickup
        check wants and keeps the common call to its coordinates and its size.
        """
        if layer_mask is None:
            layer_mask = 0
        found = (ctypes.c_uint64 * 256)()
        n = self._f.overlap_box(self._h, x, y, z, hx, hy, hz, layer_mask, found, 256)
        n = min(n, 256)
        return [found[i] for i in range(n)] if n > 0 else []

    def get_param(self, e: int, name: str) -> tuple[int, float, int] | None:
        """jce.get_param — shape: fallible_out, since 1.

        Returns kind, number, entity for an AUTHORED script parameter --
        Unity's [SerializeField], Godot's @export. Returns nil when the entity
        has no script component, when no parameter of that name is authored, or
        when the name is empty: three absences a script cannot act differently
        on, so `jce.get_param(e, 'speed') or 3.0` reads the way an author
        expects.
        """
        out_kind = ctypes.c_int()
        out_number = ctypes.c_double()
        out_entity = ctypes.c_uint64()
        if not self._f.get_param(self._h, e, _s(name), ctypes.byref(out_kind), ctypes.byref(out_number), ctypes.byref(out_entity)):
            return None
        return (out_kind.value, out_number.value, out_entity.value,)

    def get_param_text(self, e: int, name: str) -> str:
        """jce.get_param_text — shape: value_return, since 1.

        The TEXT value of an authored script parameter, or '' when the entity
        has no script component, no parameter of that name, or one that is not
        text. Empty rather than nil for the same reason ui_get_input_text is
        empty: a script comparing strings should not have to test for nil
        first. The string is the component's own buffer -- copy it if you keep
        it.
        """
        v = self._f.get_param_text(self._h, e, _s(name))
        return v.decode("utf-8", "replace") if v is not None else ""

    def curve_eval(self, path: str, channel: str, t: float) -> tuple[float] | None:
        """jce.curve_eval — shape: fallible_out, since 1.

        Sample an AUTHORED curve -- the documents the editor's Curve Editor
        writes, which nothing could read until this binding existed. Unity's
        AnimationCurve shape: the curve is a designer-authored function and the
        script decides what it means, so the engine never has to invent what a
        curve DRIVES. Returns nil when the path does not resolve, the document
        does not parse, the named channel is absent, or that channel has no
        keys -- so a curve that genuinely evaluates to 0 and a curve that is
        not there are never one reading, and `jce.curve_eval(p, 'kick', t) or
        0.0` reads the way an author expects. An empty channel name means the
        FIRST channel, which is a different request from a name that is not
        there. The parsed curve is cached per runtime, so a call inside
        on_update costs a name compare, not a JSON parse.
        """
        out_value = ctypes.c_double()
        if not self._f.curve_eval(self._h, _s(path), _s(channel), t, ctypes.byref(out_value)):
            return None
        return (out_value.value,)

    def vcam_activate(self, name: str) -> int:
        """jce.vcam_activate — shape: value_return, since 1.

        Cut to the virtual camera with this name, ahead of priority. Returns 1
        when the name resolves to a camera that is active and enabled, 0
        otherwise -- the request is recorded either way, so naming a camera in
        a streaming cell that has not loaded yet does not silently become
        'whatever priority says'. Pass an empty string to clear it and hand the
        decision back to priority. It does NOT rewrite the authored components:
        the override lives in the vcam system, so a cutscene cannot bake its
        camera choice into the level file.
        """
        return self._f.vcam_activate(self._h, _s(name))
