/* jce_script_api.h -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_c_abi.py --write
 *
 * The script-facing C ABI: every entry of the scripting surface in
 * contracts/script-api.json, flattened into plain C so that a
 * ctypes / JNA / JNI / C++ binding can call it.  The Lua entry points
 * themselves cannot be this ABI -- they are `static int (lua_State *)`.
 * What is exported is the JceScriptHost member each one calls, with
 * `void *user` replaced by an opaque handle.
 *
 * THE EXPORTED SET IS THE WHOLE SURFACE.  The shared library exports
 * these names and nothing else -- no engine symbol, no allocator, no
 * asset reader.  That is the sandbox property, and it is asserted at the
 * symbol table by tests/scripting/c_abi/test_jce_script_api_abi.c.
 *
 * ABSENT MEMBERS.  A host callback may be NULL, and a host built against
 * an older header is shorter than this one -- jce_script_api_open copies
 * min(caller, engine) over a zeroed table, so its missing members stay
 * NULL.  Either way the entry point does nothing, zero-fills every out
 * parameter and returns false / 0 / NULL / -1.  That is what the Lua
 * binding does when it pushes nil.  Out parameters are meaningful only
 * when the call returns true (always, for the void ones).
 */
#ifndef JCE_SCRIPT_API_H
#define JCE_SCRIPT_API_H

/* JceScriptHost, JceScriptEntity, JceScriptRaycastHit: this ABI restates
 * no engine type.  Consumers that compile C need engine/include on the
 * include path; ctypes and JNA consumers do not read this header at all
 * -- they read contracts/script-api.json. */
#include <jce/middleware/script/jce_script.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* On Windows the export set is jce_script_api.def, generated beside this
 * header from the same manifest: ONE list decides what leaves the DLL,
 * and an accidental __declspec(dllexport) elsewhere shows up as an extra
 * export the symbol-table test names.  On ELF/Mach-O the library is built
 * -fvisibility=hidden and this attribute is what opts a symbol back in. */
#if defined(JCE_SCRIPT_API_BUILD) && (defined(__GNUC__) || defined(__clang__))
#  define JCE_SCRIPT_API __attribute__((visibility("default")))
#else
#  define JCE_SCRIPT_API
#endif

/* The scripting surface's version.  Monotonic, and NOT the engine ABI
 * version -- that handshake is jce_api_version(), which this library
 * deliberately does not export, because a binding never calls the engine
 * directly.
 *
 * Entries are never renamed or removed, so:
 *   older binding, newer library -> loads and runs.  It is a strict
 *     subset: the binding declares no entry whose `since` exceeds the
 *     script_api_version it was generated from.
 *   newer binding, older library -> jce_script_api_open() returns NULL.
 *     It cannot degrade: the binding would call entries that do not
 *     exist.  The loader reports both numbers -- its own script_api_min
 *     and jce_script_api_version(). */
#define JCE_SCRIPT_API_VERSION 1u

/* Entry points that are a manifest entry; the library exports these plus
 * the 3 meta entry points below. */
#define JCE_SCRIPT_API_ENTRY_COUNT 71

/* Opaque: the library owns the host copy, the caller owns nothing. */
typedef struct JceScriptApi JceScriptApi;

/* The script_api_version this library implements. */
JCE_SCRIPT_API uint32_t jce_script_api_version(void);

/* Bind to a host.  `host_size` is the CALLER's sizeof(JceScriptHost) --
 * always from sizeof, never summed -- and `script_api_min` is the
 * script_api_version the caller was generated from.
 *
 * Returns NULL when `host` is NULL, `host_size` is 0, or `script_api_min`
 * is newer than JCE_SCRIPT_API_VERSION.  The host table is COPIED, so the
 * caller may keep it on the stack. */
JCE_SCRIPT_API JceScriptApi *jce_script_api_open(const JceScriptHost *host,
                                                 size_t host_size,
                                                 uint32_t script_api_min);

/* Releases the handle.  NULL is a no-op. */
JCE_SCRIPT_API void jce_script_api_close(JceScriptApi *api);

/* ------------------------------------------------------------------ *
 *  NOT in this ABI: the manifest's hand-written entries.
 *
 *  They are excluded as a class, by rule: each is either Lua-VM
 *  machinery or carries policy that lives in `static` functions inside
 *  jce_script.c.  Re-implementing that policy here would be a second
 *  copy of a security decision; exporting the raw host member instead
 *  would hand a binding the unvalidated primitive.  The manifest's own
 *  reasons, verbatim:
 *
 *    log
 *        no-host fallback: the LOG_INFO else-branch at jce_script.c:62 is the
 *        only one among the 78
 *    asset_read_text
 *        policy, not glue: script_virtual_asset_path_valid (:66, called
 *        :102), the 1 MiB JCE_SCRIPT_TEXT_ASSET_MAX_BYTES cap
 *        (jce_script.h:86), jce_free on every exit path. A generated
 *        read_file template is a sandbox escape (P0-2)
 *    asset_read_json
 *        all of asset_read_text (the same script_virtual_asset_path_valid at
 *        :219) plus a depth- and node-capped JSON walk, non-finite rejection,
 *        the json_null sentinel and distinct string error codes (P0-2)
 *    play_sound
 *        arity dispatch across two members: lua_gettop at :263 routes to
 *        play_sound_spatial (:286) or play_sound (:289); its own comment at
 *        :280 concedes top == 3 is ambiguous
 *    start_coroutine
 *        Lua VM machinery: lua_newthread / lua_xmove / luaL_ref / lua_resume;
 *        owns s->coros[]
 *    wait_seconds
 *        lua_yield into the same scheduler
 *    stop_coroutine
 *        scans s->coros[]
 * ------------------------------------------------------------------ */

/* jce_script_api_get_position -- shape: fallible_out, since 1
 * World-space position of `entity`. Absent when the entity has no transform.
 */
JCE_SCRIPT_API bool jce_script_api_get_position(JceScriptApi *api,
                                                JceScriptEntity e,
                                                float out_xyz[3]);

/* jce_script_api_set_position -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_set_position(JceScriptApi *api,
                                                JceScriptEntity e, float x,
                                                float y, float z);

/* jce_script_api_get_rotation -- shape: fallible_out, since 1
 */
JCE_SCRIPT_API bool jce_script_api_get_rotation(JceScriptApi *api,
                                                JceScriptEntity e,
                                                float out_euler_deg[3]);

/* jce_script_api_set_rotation -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_set_rotation(JceScriptApi *api,
                                                JceScriptEntity e, float x,
                                                float y, float z);

/* jce_script_api_get_scale -- shape: fallible_out, since 1
 */
JCE_SCRIPT_API bool jce_script_api_get_scale(JceScriptApi *api,
                                             JceScriptEntity e,
                                             float out_xyz[3]);

/* jce_script_api_set_scale -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_set_scale(JceScriptApi *api,
                                             JceScriptEntity e, float x,
                                             float y, float z);

/* jce_script_api_set_parent -- shape: value_return, since 1
 * The ONLY boolean argument on this surface that is type-checked; the other
 * five accept any truthy value. The luaL_checktype is emitted from this
 * entry's `strict` modifier -- no line citation, because the hand-written
 * body that carried it is gone.
 * test_strict_emits_a_type_check_only_for_the_named_parameter is what fails
 * if the emitter drops it.
 */
JCE_SCRIPT_API bool jce_script_api_set_parent(JceScriptApi *api,
                                              JceScriptEntity child,
                                              JceScriptEntity parent,
                                              bool preserve_world);

/* jce_script_api_get_parent -- shape: value_return, since 1
 */
JCE_SCRIPT_API JceScriptEntity jce_script_api_get_parent(JceScriptApi *api,
                                                         JceScriptEntity child);

/* jce_script_api_is_key_down -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_is_key_down(JceScriptApi *api, int keycode);

/* jce_script_api_find_with_tag -- shape: value_return, since 1
 */
JCE_SCRIPT_API JceScriptEntity jce_script_api_find_with_tag(JceScriptApi *api,
                                                            const char * tag);

/* jce_script_api_destroy -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_destroy(JceScriptApi *api,
                                           JceScriptEntity e);

/* jce_script_api_spawn -- shape: value_return, since 1
 */
JCE_SCRIPT_API JceScriptEntity jce_script_api_spawn(JceScriptApi *api,
                                                    const char * prefab_path,
                                                    float x, float y, float z);

/* jce_script_api_move_axis -- shape: void_out_array, since 1
 */
JCE_SCRIPT_API void jce_script_api_move_axis(JceScriptApi *api,
                                             float out_xz[2]);

/* jce_script_api_jump_pressed -- shape: value_return, since 1
 * The binding supplies button=0 (manifest bind_args), so it is not an
 * argument here either.
 */
JCE_SCRIPT_API bool jce_script_api_jump_pressed(JceScriptApi *api);

/* jce_script_api_sprint -- shape: value_return, since 1
 * The binding supplies button=1 (manifest bind_args), so it is not an
 * argument here either.
 */
JCE_SCRIPT_API bool jce_script_api_sprint(JceScriptApi *api);

/* jce_script_api_attack_pressed -- shape: value_return, since 1
 * The binding supplies button=2 (manifest bind_args), so it is not an
 * argument here either.
 */
JCE_SCRIPT_API bool jce_script_api_attack_pressed(JceScriptApi *api);

/* jce_script_api_set_time_scale -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_set_time_scale(JceScriptApi *api,
                                                  float scale);

/* jce_script_api_pause -- shape: void_call, since 1
 * jce.pause() with no argument pauses; jce.pause(false) resumes.
 */
JCE_SCRIPT_API void jce_script_api_pause(JceScriptApi *api, bool paused);

/* jce_script_api_shake_camera -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_shake_camera(JceScriptApi *api,
                                                float amount);

/* jce_script_api_music_set_intensity -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_music_set_intensity(JceScriptApi *api,
                                                       float intensity);

/* jce_script_api_music_get_intensity -- shape: value_return, since 1
 */
JCE_SCRIPT_API float jce_script_api_music_get_intensity(JceScriptApi *api);

/* jce_script_api_music_request_transition -- shape: value_return, since 1
 * Absolute playhead time of the quantized switch; negative on miss or no
 * track, which is why the no-host value is -1 and not 0.
 * When the host member is absent this returns -1.0, not 0.
 */
JCE_SCRIPT_API float jce_script_api_music_request_transition(JceScriptApi *api,
                                                             int to_segment);

/* jce_script_api_gas_activate -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_gas_activate(JceScriptApi *api,
                                                JceScriptEntity e,
                                                uint32_t ability_id);

/* jce_script_api_gas_get -- shape: fallible_out, since 1
 */
JCE_SCRIPT_API bool jce_script_api_gas_get(JceScriptApi *api,
                                           JceScriptEntity e,
                                           const char * attr_name,
                                           float *out_value);

/* jce_script_api_gas_apply -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_gas_apply(JceScriptApi *api,
                                             JceScriptEntity e,
                                             const char * attr_name, int op,
                                             float magnitude,
                                             float duration_seconds);

/* jce_script_api_raycast -- shape: fallible_out, since 1
 * 8 values on a hit; a MISS pushes integer 0, not nil -- scripts branch on `e
 * == 0`.
 */
JCE_SCRIPT_API bool jce_script_api_raycast(JceScriptApi *api,
                                           const float origin[3],
                                           const float dir[3], float max_dist,
                                           JceScriptRaycastHit *out);

/* jce_script_api_apply_impulse -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_apply_impulse(JceScriptApi *api,
                                                 JceScriptEntity e, float x,
                                                 float y, float z);

/* jce_script_api_set_velocity -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_set_velocity(JceScriptApi *api,
                                                JceScriptEntity e, float x,
                                                float y, float z);

/* jce_script_api_anim_set_float -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_anim_set_float(JceScriptApi *api,
                                                  JceScriptEntity e,
                                                  const char * name, float v);

/* jce_script_api_anim_set_int -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_anim_set_int(JceScriptApi *api,
                                                JceScriptEntity e,
                                                const char * name, int v);

/* jce_script_api_anim_set_bool -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_anim_set_bool(JceScriptApi *api,
                                                 JceScriptEntity e,
                                                 const char * name, bool v);

/* jce_script_api_anim_set_trigger -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_anim_set_trigger(JceScriptApi *api,
                                                    JceScriptEntity e,
                                                    const char * name);

/* jce_script_api_is_action_down -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_is_action_down(JceScriptApi *api,
                                                  const char * name);

/* jce_script_api_is_action_pressed -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_is_action_pressed(JceScriptApi *api,
                                                     const char * name);

/* jce_script_api_get_axis -- shape: value_return, since 1
 */
JCE_SCRIPT_API float jce_script_api_get_axis(JceScriptApi *api,
                                             const char * name);

/* jce_script_api_get_pointer_delta -- shape: void_out_array, since 1
 */
JCE_SCRIPT_API void jce_script_api_get_pointer_delta(JceScriptApi *api,
                                                     float out_xy[2]);

/* jce_script_api_get_pointer_wheel -- shape: value_return, since 1
 */
JCE_SCRIPT_API float jce_script_api_get_pointer_wheel(JceScriptApi *api);

/* jce_script_api_is_pointer_down -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_is_pointer_down(JceScriptApi *api,
                                                   int button);

/* jce_script_api_get_touch_count -- shape: value_return, since 1
 * A host returning a negative count is clamped to 0 so `for i = 1,
 * jce.get_touch_count()` cannot underflow.
 * The returned value is clamped to a minimum of 0.
 */
JCE_SCRIPT_API int jce_script_api_get_touch_count(JceScriptApi *api);

/* jce_script_api_get_touch -- shape: fallible_out, since 1
 * 1-based Lua index mapped to 0-based C; an index below 1 returns nil without
 * calling the host.
 * INDEX BASE: the Lua binding takes a 1-based index and subtracts 1 before
 * calling the host. This ABI is the transport, so it passes the host's own
 * 0-based index straight through.
 */
JCE_SCRIPT_API bool jce_script_api_get_touch(JceScriptApi *api, int index,
                                             uint64_t *id, float *x, float *y,
                                             float *pressure);

/* jce_script_api_tr -- shape: value_return, since 1
 * Passthrough is the contract, not a fallback: an unlocalized build shows
 * readable keys instead of blank UI.
 * When the host member is absent this returns `key` itself, not NULL.
 */
JCE_SCRIPT_API const char *jce_script_api_tr(JceScriptApi *api,
                                             const char * key);

/* jce_script_api_get_locale -- shape: value_return, since 1
 */
JCE_SCRIPT_API const char *jce_script_api_get_locale(JceScriptApi *api);

/* jce_script_api_set_locale -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_set_locale(JceScriptApi *api,
                                              const char * locale);

/* jce_script_api_get_velocity -- shape: fallible_out, since 1
 */
JCE_SCRIPT_API bool jce_script_api_get_velocity(JceScriptApi *api,
                                                JceScriptEntity e,
                                                float out[3]);

/* jce_script_api_vehicle_set_input -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_vehicle_set_input(JceScriptApi *api,
                                                     JceScriptEntity e,
                                                     float throttle,
                                                     float brake, float steer);

/* jce_script_api_vehicle_get_speed -- shape: value_return, since 1
 */
JCE_SCRIPT_API float jce_script_api_vehicle_get_speed(JceScriptApi *api,
                                                      JceScriptEntity e);

/* jce_script_api_get_move -- shape: void_out_array, since 1
 */
JCE_SCRIPT_API void jce_script_api_get_move(JceScriptApi *api, float out[3]);

/* jce_script_api_ui_get_slider -- shape: fallible_out, since 1
 */
JCE_SCRIPT_API bool jce_script_api_ui_get_slider(JceScriptApi *api,
                                                 JceScriptEntity e, float *out);

/* jce_script_api_ui_set_slider -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_ui_set_slider(JceScriptApi *api,
                                                 JceScriptEntity e, float v);

/* jce_script_api_ui_get_toggle -- shape: fallible_out, since 1
 */
JCE_SCRIPT_API bool jce_script_api_ui_get_toggle(JceScriptApi *api,
                                                 JceScriptEntity e, bool *out);

/* jce_script_api_ui_set_toggle -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_ui_set_toggle(JceScriptApi *api,
                                                 JceScriptEntity e, bool v);

/* jce_script_api_ui_set_text -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_ui_set_text(JceScriptApi *api,
                                               JceScriptEntity e,
                                               const char * txt);

/* jce_script_api_send_message -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_send_message(JceScriptApi *api,
                                                JceScriptEntity target,
                                                const char * msg,
                                                double number_arg,
                                                const char * str_arg);

/* jce_script_api_broadcast -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_broadcast(JceScriptApi *api,
                                             const char * msg,
                                             double number_arg,
                                             const char * str_arg);

/* jce_script_api_has_component -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_has_component(JceScriptApi *api,
                                                 JceScriptEntity e,
                                                 const char * comp_name);

/* jce_script_api_is_component_enabled -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_is_component_enabled(JceScriptApi *api,
                                                        JceScriptEntity e,
                                                        const char * comp_name);

/* jce_script_api_set_component_enabled -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_set_component_enabled(JceScriptApi *api,
                                                         JceScriptEntity e,
                                                         const char * comp_name,
                                                         bool on);

/* jce_script_api_net_is_server -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_net_is_server(JceScriptApi *api);

/* jce_script_api_net_is_client -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_net_is_client(JceScriptApi *api);

/* jce_script_api_net_spawn -- shape: value_return, since 1
 */
JCE_SCRIPT_API JceScriptEntity jce_script_api_net_spawn(JceScriptApi *api,
                                                        const char * prefab_path,
                                                        float x, float y,
                                                        float z);

/* jce_script_api_rpc_send -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_rpc_send(JceScriptApi *api,
                                            JceScriptEntity e,
                                            const char * event, int target,
                                            const char * payload);

/* jce_script_api_particle_burst -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_particle_burst(JceScriptApi *api,
                                                  JceScriptEntity e, int count);

/* jce_script_api_particle_set_emitting -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_particle_set_emitting(JceScriptApi *api,
                                                         JceScriptEntity e,
                                                         bool on);

/* jce_script_api_particle_set_color -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_particle_set_color(JceScriptApi *api,
                                                      JceScriptEntity e,
                                                      float r, float g,
                                                      float b);

/* jce_script_api_find_by_name -- shape: first_and_count, since 1
 * first-or-nil AND a match count, so a strict scene director can reject
 * duplicate authored names. IDENTICAL C signature to find_by_prefix and a
 * DIFFERENT contract.
 */
JCE_SCRIPT_API int jce_script_api_find_by_name(JceScriptApi *api,
                                               const char * name,
                                               JceScriptEntity *out, int max);

/* jce_script_api_find_by_prefix -- shape: entity_table, since 1
 * One Lua array. IDENTICAL C signature to find_by_name and a DIFFERENT
 * contract.
 */
JCE_SCRIPT_API int jce_script_api_find_by_prefix(JceScriptApi *api,
                                                 const char * prefix,
                                                 JceScriptEntity *out, int max);

/* jce_script_api_comp_get -- shape: owned_string_release, since 1
 * Copies at most out_cap-1 bytes plus a NUL into `out`, releases
 * the host's string through `json_free`, and returns the full
 * length (which may exceed out_cap-1) or -1 when absent.  Call
 * with out=NULL, out_cap=0 to ask for the length.
 */
JCE_SCRIPT_API int jce_script_api_comp_get(JceScriptApi *api,
                                           JceScriptEntity e,
                                           const char * type, char *out,
                                           int out_cap);

/* jce_script_api_comp_set -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_comp_set(JceScriptApi *api,
                                            JceScriptEntity e,
                                            const char * type,
                                            const char * json);

/* jce_script_api_render_get -- shape: owned_string_release, since 1
 * Copies at most out_cap-1 bytes plus a NUL into `out`, releases
 * the host's string through `json_free`, and returns the full
 * length (which may exceed out_cap-1) or -1 when absent.  Call
 * with out=NULL, out_cap=0 to ask for the length.
 */
JCE_SCRIPT_API int jce_script_api_render_get(JceScriptApi *api, char *out,
                                             int out_cap);

/* jce_script_api_render_set -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_render_set(JceScriptApi *api,
                                              const char * json);

/* jce_script_api_audio_set_volume -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_audio_set_volume(JceScriptApi *api,
                                                    JceScriptEntity e,
                                                    float volume);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SCRIPT_API_H */
