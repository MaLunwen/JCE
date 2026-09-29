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
#define JCE_SCRIPT_API_ENTRY_COUNT 101

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
 *    line_set_points
 *        marshalling, not glue: the host takes a packed `const float *xyz,
 *        int count` and no generated shape reads a Lua array into one.
 *        Hand-written so the table walk, the JCE_LINE_MAX_POINTS clamp and
 *        the returned stored-count live in one place. Exists because the
 *        comp_set/JSON route is O(n^2): cJSON resolves each flat px/py/pz key
 *        by walking the object's child list (jce_scene_components_render.c
 *        parse_line_renderer)
 *    log
 *        no-host fallback: the LOG_INFO else-branch at jce_script.c:63 is the
 *        only one among the 78
 *    asset_read_text
 *        policy, not glue: script_virtual_asset_path_valid (:67, called
 *        :103), the 1 MiB JCE_SCRIPT_TEXT_ASSET_MAX_BYTES cap
 *        (jce_script.h:114), jce_free on every exit path. A generated
 *        read_file template is a sandbox escape (P0-2)
 *    asset_read_json
 *        all of asset_read_text (the same script_virtual_asset_path_valid at
 *        :220) plus a depth- and node-capped JSON walk, non-finite rejection,
 *        the json_null sentinel and distinct string error codes (P0-2)
 *    play_sound
 *        arity dispatch across two members: lua_gettop at :264 routes to
 *        play_sound_spatial (:287) or play_sound (:290); its own comment at
 *        :281 concedes top == 3 is ambiguous
 *    start_coroutine
 *        Lua VM machinery: lua_newthread / lua_xmove / luaL_ref / lua_resume;
 *        owns s->coros[]
 *    wait_seconds
 *        lua_yield into the same scheduler
 *    stop_coroutine
 *        scans s->coros[]
 * ------------------------------------------------------------------ */

/* jce_script_api_get_position -- shape: fallible_out, since 1
 * LOCAL position of `entity` -- its own translation, not composed up the
 * parent chain. Absent when the entity has no transform. Use
 * get_world_position for the composed pose. This doc said 'World-space' until
 * 2026-08-26; the implementation always returned the local TRS
 * (jce_scene_get_transform), and the wrong word was generated into all five
 * language SDKs.
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

/* jce_script_api_get_world_position -- shape: fallible_out, since 1
 * WORLD position of `entity`: its local TRS composed up the parent chain
 * (jce_scene_get_world_matrix). Absent when the entity has no transform.
 * Every parented rig -- arms, jaws, fingers, pads -- needs this rather than
 * get_position.
 */
JCE_SCRIPT_API bool jce_script_api_get_world_position(JceScriptApi *api,
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

/* jce_script_api_raycast_filtered -- shape: fallible_out, since 1
 * Closest hit along the ray, honouring a layer mask and the trigger skip -- 8
 * values on a hit; a MISS pushes integer 0, not nil, so scripts branch on `e
 * == 0`, the same as jce.raycast. layer_mask 0 means every layer and
 * hit_triggers defaults to false, so the common call stays
 * origin/dir/distance and the filter is what you add when you need it.
 * hit_triggers is separate from the mask because a trigger volume is not a
 * layer: collapsing them would make 'ignore triggers on layer 3'
 * inexpressible.
 */
JCE_SCRIPT_API bool jce_script_api_raycast_filtered(JceScriptApi *api,
                                                    const float origin[3],
                                                    const float dir[3],
                                                    float max_dist,
                                                    uint32_t layer_mask,
                                                    bool hit_triggers,
                                                    JceScriptRaycastHit *out);

/* jce_script_api_raycast_all -- shape: entity_table, since 1
 * Every entity the ray passes through, as one array sorted near to far.
 * layer_mask 0 means every layer; hit_triggers defaults to false. Returns
 * ENTITIES rather than full hit records because the eight-value hit does not
 * survive as an array shape across seven languages without inventing a
 * per-language container -- re-query a specific one with jce.raycast_filtered
 * when you need its point and normal.
 */
JCE_SCRIPT_API int jce_script_api_raycast_all(JceScriptApi *api,
                                              const float origin[3],
                                              const float dir[3],
                                              float max_dist,
                                              uint32_t layer_mask,
                                              bool hit_triggers,
                                              JceScriptEntity *out, int max);

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

/* jce_script_api_ui_get_progress -- shape: fallible_out, since 1
 */
JCE_SCRIPT_API bool jce_script_api_ui_get_progress(JceScriptApi *api,
                                                   JceScriptEntity e,
                                                   float *out);

/* jce_script_api_ui_set_progress -- shape: void_call, since 1
 */
JCE_SCRIPT_API void jce_script_api_ui_set_progress(JceScriptApi *api,
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

/* jce_script_api_ui_get_dropdown -- shape: fallible_out, since 1
 * Selected option INDEX of `entity`'s UIDropdown. Absent when the entity has
 * no dropdown, so a script can tell 'no dropdown' from 'a dropdown reading
 * 0'. The index and not the label: branching on which option is the common
 * case, and a label would make it a string compare.
 */
JCE_SCRIPT_API bool jce_script_api_ui_get_dropdown(JceScriptApi *api,
                                                   JceScriptEntity e, int *out);

/* jce_script_api_ui_set_dropdown -- shape: void_call, since 1
 * Select an option by INDEX. Clamped into [0, option_count-1] rather than
 * refused, the way ui_set_progress clamps and the way the scene loader
 * clamps: the draw already clamps, so storing outside the range would make
 * the component and the picture disagree.
 */
JCE_SCRIPT_API void jce_script_api_ui_set_dropdown(JceScriptApi *api,
                                                   JceScriptEntity e,
                                                   int index);

/* jce_script_api_ui_get_input_text -- shape: value_return, since 1
 * Current text of `entity`'s UIInputField, or '' when it has none. The string
 * is the component's own buffer and is valid until the next mutation of that
 * entity -- the same contract tr() and get_locale() carry; every binding
 * copies it and none may store it.
 */
JCE_SCRIPT_API const char *jce_script_api_ui_get_input_text(JceScriptApi *api,
                                                            JceScriptEntity e);

/* jce_script_api_ui_set_input_text -- shape: void_call, since 1
 * Replace the UIInputField's text. Truncated to the field's capacity and to
 * char_limit when one is set -- the same cap the canvas applies to typed
 * input, so a script write and a keystroke cannot disagree about what the
 * field holds. A truncation is logged rather than silent.
 */
JCE_SCRIPT_API void jce_script_api_ui_set_input_text(JceScriptApi *api,
                                                     JceScriptEntity e,
                                                     const char * text);

/* jce_script_api_ui_get_scroll -- shape: fallible_out, since 1
 * Scroll offset (x, y) of `entity`'s UIScrollView, in REFERENCE units -- what
 * the component stores and what the wheel path clamps, not device px. Absent
 * when the entity has no scroll view.
 */
JCE_SCRIPT_API bool jce_script_api_ui_get_scroll(JceScriptApi *api,
                                                 JceScriptEntity e,
                                                 float out_xy[2]);

/* jce_script_api_ui_set_scroll -- shape: void_call, since 1
 * Set the scroll offset in reference units. A disabled axis is pinned to 0
 * and each axis is clamped the way the wheel path clamps, so a script cannot
 * push the offset somewhere a wheel could not; the canvas re-clamps against
 * the resolved viewport on the next render.
 */
JCE_SCRIPT_API void jce_script_api_ui_set_scroll(JceScriptApi *api,
                                                 JceScriptEntity e, float x,
                                                 float y);

/* jce_script_api_world_get_hour -- shape: value_return, since 1
 * Live hour of day in [0, 24) -- what the sky is showing now, NOT the
 * authored tod_hour seed a scene starts from. Reading the seed would return
 * the level's start-of-day forever while the sky moved.
 */
JCE_SCRIPT_API float jce_script_api_world_get_hour(JceScriptApi *api);

/* jce_script_api_world_set_hour -- shape: void_call, since 1
 * Move the live clock, wrapping into [0, 24). For 'sleep until dawn'. The
 * authored seed is untouched, so reloading the scene still starts where the
 * designer set it.
 */
JCE_SCRIPT_API void jce_script_api_world_set_hour(JceScriptApi *api,
                                                  float hour);

/* jce_script_api_world_is_daytime -- shape: value_return, since 1
 * True while the sun is above the horizon. THE predicate for 'is it night?'
 * -- every key-light chooser in the engine is required to agree on this one,
 * so a script that rolled its own threshold would disagree with the lighting
 * it can see.
 */
JCE_SCRIPT_API bool jce_script_api_world_is_daytime(JceScriptApi *api);

/* jce_script_api_world_get_weather -- shape: value_return, since 1
 * Authored weather type: 0 clear, 1 rain, 2 snow.
 */
JCE_SCRIPT_API int jce_script_api_world_get_weather(JceScriptApi *api);

/* jce_script_api_world_get_weather_intensity -- shape: value_return, since 1
 * Authored weather intensity in [0, 1].
 */
JCE_SCRIPT_API float jce_script_api_world_get_weather_intensity(JceScriptApi *api);

/* jce_script_api_world_get_wind_speed -- shape: value_return, since 1
 * Instantaneous wind speed in m/s -- the sustained speed plus this moment's
 * gust. Do NOT key a cache on it: it changes every frame by design. It is the
 * same number the ocean spectrum and the vegetation shader read, so a script
 * cannot disagree with what is on screen.
 */
JCE_SCRIPT_API float jce_script_api_world_get_wind_speed(JceScriptApi *api);

/* jce_script_api_request_scene -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_request_scene(JceScriptApi *api,
                                                 const char * scene_path);

/* jce_script_api_is_transitioning -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_is_transitioning(JceScriptApi *api);

/* jce_script_api_audio_play -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_audio_play(JceScriptApi *api,
                                              JceScriptEntity e);

/* jce_script_api_audio_stop -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_audio_stop(JceScriptApi *api,
                                              JceScriptEntity e);

/* jce_script_api_audio_is_playing -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_audio_is_playing(JceScriptApi *api,
                                                    JceScriptEntity e);

/* jce_script_api_save_game -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_save_game(JceScriptApi *api,
                                             const char * path);

/* jce_script_api_load_game -- shape: value_return, since 1
 */
JCE_SCRIPT_API bool jce_script_api_load_game(JceScriptApi *api,
                                             const char * path);

/* jce_script_api_overlap_sphere -- shape: entity_table, since 1
 * Entities whose collider overlaps the sphere, as one array. layer_mask 0
 * means all layers. Triggers are skipped. layer_mask is OPTIONAL: omitting it
 * means every layer, which is what an explosion or a pickup check wants and
 * keeps the common call to its coordinates and its size.
 */
JCE_SCRIPT_API int jce_script_api_overlap_sphere(JceScriptApi *api, float x,
                                                 float y, float z,
                                                 float radius,
                                                 uint32_t layer_mask,
                                                 JceScriptEntity *out, int max);

/* jce_script_api_overlap_box -- shape: entity_table, since 1
 * Entities whose collider overlaps the axis-aligned box (half-extents), as
 * one array. layer_mask 0 means all layers. layer_mask is OPTIONAL: omitting
 * it means every layer, which is what an explosion or a pickup check wants
 * and keeps the common call to its coordinates and its size.
 */
JCE_SCRIPT_API int jce_script_api_overlap_box(JceScriptApi *api, float x,
                                              float y, float z, float hx,
                                              float hy, float hz,
                                              uint32_t layer_mask,
                                              JceScriptEntity *out, int max);

/* jce_script_api_get_param -- shape: fallible_out, since 1
 * Returns kind, number, entity for an AUTHORED script parameter -- Unity's
 * [SerializeField], Godot's @export. Returns nil when the entity has no
 * script component, when no parameter of that name is authored, or when the
 * name is empty: three absences a script cannot act differently on, so
 * `jce.get_param(e, 'speed') or 3.0` reads the way an author expects.
 */
JCE_SCRIPT_API bool jce_script_api_get_param(JceScriptApi *api,
                                             JceScriptEntity e,
                                             const char * name, int *out_kind,
                                             double *out_number,
                                             JceScriptEntity *out_entity);

/* jce_script_api_get_param_text -- shape: value_return, since 1
 * The TEXT value of an authored script parameter, or '' when the entity has
 * no script component, no parameter of that name, or one that is not text.
 * Empty rather than nil for the same reason ui_get_input_text is empty: a
 * script comparing strings should not have to test for nil first. The string
 * is the component's own buffer -- copy it if you keep it.
 */
JCE_SCRIPT_API const char *jce_script_api_get_param_text(JceScriptApi *api,
                                                         JceScriptEntity e,
                                                         const char * name);

/* jce_script_api_curve_eval -- shape: fallible_out, since 1
 * Sample an AUTHORED curve -- the documents the editor's Curve Editor writes,
 * which nothing could read until this binding existed. Unity's AnimationCurve
 * shape: the curve is a designer-authored function and the script decides
 * what it means, so the engine never has to invent what a curve DRIVES.
 * Returns nil when the path does not resolve, the document does not parse,
 * the named channel is absent, or that channel has no keys -- so a curve that
 * genuinely evaluates to 0 and a curve that is not there are never one
 * reading, and `jce.curve_eval(p, 'kick', t) or 0.0` reads the way an author
 * expects. An empty channel name means the FIRST channel, which is a
 * different request from a name that is not there. The parsed curve is cached
 * per runtime, so a call inside on_update costs a name compare, not a JSON
 * parse.
 */
JCE_SCRIPT_API bool jce_script_api_curve_eval(JceScriptApi *api,
                                              const char * path,
                                              const char * channel, double t,
                                              double *out_value);

/* jce_script_api_vcam_activate -- shape: value_return, since 1
 * Cut to the virtual camera with this name, ahead of priority. Returns 1 when
 * the name resolves to a camera that is active and enabled, 0 otherwise --
 * the request is recorded either way, so naming a camera in a streaming cell
 * that has not loaded yet does not silently become 'whatever priority says'.
 * Pass an empty string to clear it and hand the decision back to priority. It
 * does NOT rewrite the authored components: the override lives in the vcam
 * system, so a cutscene cannot bake its camera choice into the level file.
 */
JCE_SCRIPT_API int jce_script_api_vcam_activate(JceScriptApi *api,
                                                const char * name);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SCRIPT_API_H */
