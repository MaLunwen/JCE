/*
 * jce_script.h  Gameplay scripting VM (Lua) — subsystem public header.
 *
 * Layer: Middleware (L4). The script VM is a GENERIC Lua host: it depends only
 * on core + Lua and knows nothing about the scene/ECS. The runtime wires it to
 * gameplay by supplying a JceScriptHost callback bridge (mirroring how the AI
 * layer's behavior-tree actions are registered by the runtime), so scripts can
 * read input / move entities / control time / log without this layer depending
 * upward.  The <jce/api_script.h> facade re-exports this for SDK consumers.
 *
 * Scripting model (Unity/Godot-like):
 *   - A `.lua` script returns a TABLE with optional functions:
 *       on_start(self)         -- called once when the instance spawns
 *       on_update(self, dt)    -- called every frame (dt seconds, time-scaled)
 *       on_collision(self, other) -- called on first physics contact (other =
 *                                    the other body's entity id)
 *       on_destroy(self)       -- called when the instance is released
 *     `self.entity` is the owning entity id; `self` is a per-instance table
 *     (per-instance state lives there) whose metatable indexes the script.
 *   - The global `jce` table exposes engine bindings backed by JceScriptHost:
 *       jce.log(msg)
 *       jce.get_position(entity) -> x,y,z      (nil if no transform)
 *       jce.set_position(entity, x,y,z)
 *       jce.is_key_down(keycode) -> bool
 *       jce.set_time_scale(scale)              -- slow-mo / hitstop / fast
 *       jce.pause(bool)                        -- pause/resume the simulation
 *       jce.shake_camera(amount)               -- add camera trauma (0..1)
 *       jce.raycast(ox,oy,oz, dx,dy,dz, max)   -- -> hit_entity,px,py,pz,
 *                                                 nx,ny,nz,dist (0 on a miss)
 *       jce.apply_impulse(entity, x,y,z)       -- impulse on a dynamic body
 *       jce.set_velocity(entity, x,y,z)        -- set linear velocity
 *       jce.get_velocity(entity) -> x,y,z      (nil if no body)
 *       jce.play_sound(path [,x,y,z] [,vol])   -- one-shot 2D/3D sound
 *       jce.ui_get_slider(entity) -> number    (nil if no slider)
 *       jce.ui_set_slider(entity, value)
 *       jce.ui_get_toggle(entity) -> bool      (nil if no toggle)
 *       jce.ui_set_toggle(entity, on)
 *       jce.ui_set_text(entity, str)
 *       jce.send_message(target, msg [, number] [, string])
 *                                              -- call method `msg` on the
 *                                                 target entity's live script
 *                                                 (decoupled gameplay comms);
 *                                                 no-op if the target has no
 *                                                 script or no such handler
 *       jce.net_is_server() -> bool            -- live replication role is the
 *                                                 authoritative server
 *       jce.net_is_client() -> bool            -- live role is a connected client
 *       jce.net_spawn(prefab_path, x,y,z) -> entity
 *                                              -- server-authoritative networked
 *                                                 spawn (0 on a client / failure)
 *       jce.particle_burst(entity, count)      -- one-shot particle burst from
 *                                                 the entity's emitter
 *       jce.particle_set_emitting(entity, on)  -- start/stop the entity's
 *                                                 continuous particle emission
 *       jce.music_set_intensity(value)         -- set adaptive-music intensity
 *       jce.music_get_intensity()              -- read it back (0..1)
 *       jce.music_request_transition(segment)  -- beat/bar-quantized switch
 */

#ifndef JCE_SCRIPT_H
#define JCE_SCRIPT_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Entity identifier as seen by scripts (matches JceEntity = uint64_t). */
typedef uint64_t JceScriptEntity;

/* Result of jce.raycast(...) marshalled back to the script.  Plain POD so the
 * script layer never pulls in a physics type (the host maps the hit body back
 * to its entity).  `entity` is 0 on a miss. */
typedef struct JceScriptRaycastHit {
    JceScriptEntity entity;   /* hit body's entity id (0 if none) */
    float           point[3]; /* world-space contact point */
    float           normal[3];/* world-space surface normal */
    float           distance; /* distance from origin to the hit */
} JceScriptRaycastHit;

/* Host bridge supplied by the runtime. Any callback may be NULL (the matching
 * Lua binding then becomes a no-op / returns nil). `user` is passed back to
 * every callback. */
typedef struct JceScriptHost {
    void *user;
    void (*log)(void *user, const char *msg);
    /* Transform (position in world units; rotation as Euler degrees XYZ;
     * scale as a per-axis multiplier).  get_* return false if the entity has
     * no transform. */
    bool (*get_position)(void *user, JceScriptEntity e, float out_xyz[3]);
    void (*set_position)(void *user, JceScriptEntity e, float x, float y, float z);
    bool (*get_rotation)(void *user, JceScriptEntity e, float out_euler_deg[3]);
    void (*set_rotation)(void *user, JceScriptEntity e, float x, float y, float z);
    bool (*get_scale)(void *user, JceScriptEntity e, float out_xyz[3]);
    void (*set_scale)(void *user, JceScriptEntity e, float x, float y, float z);
    bool (*is_key_down)(void *user, int keycode);
    /* Entity ops: find the first entity carrying `tag` (0 if none); destroy
     * an entity (and its subtree). */
    JceScriptEntity (*find_with_tag)(void *user, const char *tag);
    void (*destroy_entity)(void *user, JceScriptEntity e);
    /* Instantiate a prefab (.prefab.json, resolved host-fs then PAK) into the
     * live scene at world position (x,y,z) and return its root entity (0 on
     * failure).  Safe to call from on_update / on_collision; calling from
     * on_start during initial scene load is a no-op (returns 0). */
    JceScriptEntity (*spawn)(void *user, const char *prefab_path,
                             float x, float y, float z);
    /* Player movement intent (the processed input the runtime drives the
     * character with): move axes in [-1,1], and the jump/sprint buttons. */
    void (*move_axis)(void *user, float out_xz[2]);
    bool (*input_button)(void *user, int button);  /* 0=jump pressed, 1=sprint */
    /* Data-driven input actions (generic action/axis bridge): query the live
     * editor/shipped action map by NAME so games define arbitrary verbs and
     * axes beyond the fixed move/jump/sprint/attack set.  A no-op default
     * (false / 0) when no action map is bound this frame or the name is
     * unknown — so scripts that read custom actions degrade gracefully.
     *   action_down    : is the named action currently held?
     *   action_pressed : did it go down THIS frame (rising edge)?
     *   action_axis    : its analog value (see jce_action_value semantics). */
    bool  (*action_down)(void *user, const char *name);
    bool  (*action_pressed)(void *user, const char *name);
    float (*action_axis)(void *user, const char *name);
    /* Time control (Phase 0.2): let scripts drive global slow-motion /
     * hitstop / pause (jce.set_time_scale(s) / jce.pause(bool)). */
    void (*set_time_scale)(void *user, float scale);
    void (*set_paused)(void *user, bool paused);
    /* Camera shake (FEATURE 6.5): add trauma to the live camera-shake
     * generator (jce.shake_camera(amount)).  `amount` is clamped into the
     * trauma model's [0,1] range and decays automatically. */
    void (*shake_camera)(void *user, float amount);
    /* Gameplay Ability System (GAS consumption last-mile): act on the live
     * per-entity JceGameplayAbilitySystem the runtime built from the entity's
     * authored JceGameplayAbilitySystemComponent.  All resolve the entity's
     * live GAS via the runtime; a no-op / default when the entity has none.
     *   gas_activate : try to fire ability `ability_id` (returns success).
     *   gas_get      : read attribute `attr_name` CURRENT value (out param;
     *                  returns false if the entity has no GAS / no such attr).
     *   gas_apply    : apply a one-attribute effect (op 0=add/1=mult/2=override;
     *                  duration_seconds <= 0 => INSTANT to base, else a continuous
     *                  TIMED modifier).  Returns true if applied. */
    bool (*gas_activate)(void *user, JceScriptEntity e, uint32_t ability_id);
    bool (*gas_get)(void *user, JceScriptEntity e, const char *attr_name,
                    float *out_value);
    bool (*gas_apply)(void *user, JceScriptEntity e, const char *attr_name,
                      int op, float magnitude, float duration_seconds);
    /* Read a script file (PAK / mounted dirs). Returns a heap buffer the VM
     * frees with jce_free, or NULL on miss. Required for jce_script_instantiate
     * (file path); jce_script_instantiate_source does not use it. */
    void *(*read_file)(void *user, const char *path, uint64_t *out_size);

    /* ── Physics queries / forces (gameplay scripting depth) ───────────────
     * All operate on the runtime's live physics world.  Pure POD across the
     * boundary (no physics type leaks here): the host owns the body↔entity
     * mapping.  A NULL callback is a safe no-op / "miss".
     *   raycast       : cast a ray (origin + direction + max_dist); fills `out`
     *                   and returns true on a hit, false (and `out` untouched)
     *                   on a miss / when physics is unavailable.
     *   apply_impulse : add an instantaneous impulse to the entity's dynamic
     *                   body (no-op if the entity has no body / is not dynamic).
     *   set_velocity  : set the entity's linear velocity (m/s).
     *   get_velocity  : read the entity's linear velocity into out[3]; returns
     *                   false if the entity has no body. */
    bool (*raycast)(void *user, const float origin[3], const float dir[3],
                    float max_dist, JceScriptRaycastHit *out);
    void (*apply_impulse)(void *user, JceScriptEntity e, float x, float y, float z);
    void (*set_velocity)(void *user, JceScriptEntity e, float x, float y, float z);
    bool (*get_velocity)(void *user, JceScriptEntity e, float out[3]);

    /* ── Animation state-machine control (script -> SM params) ─────────────
     * Drive the entity's animator state machine: set a float/int/bool param or
     * fire a one-shot trigger by name (the SM's transitions consume them).  The
     * runtime queues these onto the scene anim relay; the renderer applies them
     * to the entity's SM binding next frame.  No-op if the entity has no
     * animator / SM. */
    void (*anim_set_float)(void *user, JceScriptEntity e, const char *name, float v);
    void (*anim_set_int)(void *user, JceScriptEntity e, const char *name, int v);
    void (*anim_set_bool)(void *user, JceScriptEntity e, const char *name, bool v);
    void (*anim_set_trigger)(void *user, JceScriptEntity e, const char *name);

    /* ── Audio (one-shot sound playback) ───────────────────────────────────
     * play_sound : load (PAK/host-fs) and play a sound by path.  `pos` non-NULL
     *              makes the voice positional (3D) at that world point; NULL is
     *              a 2D (non-spatial) sound.  `volume` is 0..1.  No-op (no crash)
     *              on a missing file / when audio is unavailable. */
    void (*play_sound)(void *user, const char *path, const float pos[3],
                       float volume);

    /* ── UI widget value access (gameplay ↔ HUD) ──────────────────────────
     * Operate on the live scene's UI components.  get_* return false when the
     * entity has no such widget (so the binding returns nil).
     *   ui_get_slider / ui_set_slider : the slider's current value.
     *   ui_get_toggle / ui_set_toggle : the toggle's on/off state.
     *   ui_set_text                   : replace a UIText's displayed string
     *                                   (bounds-clamped into the fixed buffer). */
    bool (*ui_get_slider)(void *user, JceScriptEntity e, float *out);
    void (*ui_set_slider)(void *user, JceScriptEntity e, float v);
    bool (*ui_get_toggle)(void *user, JceScriptEntity e, bool *out);
    void (*ui_set_toggle)(void *user, JceScriptEntity e, bool v);
    void (*ui_set_text)(void *user, JceScriptEntity e, const char *txt);

    /* ── Script-to-script messaging (decoupled gameplay communication) ──────
     * Deliver a message to `target` entity's live script instance; no-op if
     * the target has no script or no matching handler.  The runtime resolves
     * entity->instance and dispatches method `msg` on it with the (number,
     * string) payload (`str` may be NULL -> the handler sees nil).  Backs
     * jce.send_message(target, msg [, number] [, string]). */
    void (*send_message)(void *user, JceScriptEntity target, const char *msg,
                         double number_arg, const char *str_arg);

    /* Deliver a message to EVERY live script instance (a global event bus): the
     * runtime calls method `msg` on each instance with the (number, string)
     * payload; instances that define no handler of that name are skipped (clean
     * no-op).  The sender is NOT excluded.  Backs
     * jce.broadcast(msg [, number] [, string]). */
    void (*broadcast)(void *user, const char *msg,
                      double number_arg, const char *str_arg);

    /* ── Networking (replication role + server-authoritative spawn) ─────────
     * Kept deliberately type-clean: the script layer never pulls in a net
     * type.  The role is exposed as TWO bool getters (not a JceNetRole enum)
     * so this header stays pure POD.  A NULL callback returns the safe default.
     *   net_is_server : true when the live session/replication role is the
     *                   authoritative server (host or dedicated).  false (the
     *                   default) on a pure client / standalone / no host.
     *   net_is_client : true when the live role is a connected client (not the
     *                   server).  false (the default) otherwise.
     *   net_spawn     : server-authoritative networked spawn of a prefab at
     *                   world (x,y,z).  Returns the backing entity id of the
     *                   spawned NetworkObject, or 0 on a client / failure (the
     *                   spawn is server-only by the replication contract).
     *                   Backs jce.net_spawn(prefab_path, x, y, z). */
    bool (*net_is_server)(void *user);
    bool (*net_is_client)(void *user);
    JceScriptEntity (*net_spawn)(void *user, const char *prefab_path,
                                 float x, float y, float z);

    /* Send a scripted Remote Procedure Call on entity `e`'s NetworkObject: the
     * host packs (event, payload) onto a generic script-RPC channel and routes
     * it via the replication transport with the given JceRpcTarget (0=server,
     * 1=owner, 2=all-clients, 3=not-owner, 4=specific).  On the RECEIVING peer
     * the runtime dispatches `event` AS A METHOD on that entity's script
     * instance — inst:<event>(0, payload) — exactly mirroring jce.send_message
     * but over the network.  `payload` may be NULL.  Returns true if the RPC was
     * queued (false when `e` has no NetworkObject / no session).  Backs
     * jce.rpc_send(entity, event [, target] [, payload]). */
    bool (*rpc_send)(void *user, JceScriptEntity e, const char *event,
                     int target, const char *payload);

    /* ── Particles (per-entity emitter control from scripts) ────────────────
     * Operate on the live scene's JceParticleEmitterComponent for entity `e`
     * (resolved + driven through the public scene particle API by the host).
     * Pure POD across the boundary (no particle type leaks here).  A NULL
     * callback / an entity with no particle emitter is a safe no-op.
     *   particle_burst        : fire a one-shot burst of `count` particles from
     *                           the entity's emitter.  Backs
     *                           jce.particle_burst(entity, count).
     *   particle_set_emitting : start (`on`) / stop (!on) continuous emission;
     *                           stopping lets live particles age out.  Backs
     *                           jce.particle_set_emitting(entity, on). */
    void (*particle_burst)(void *user, JceScriptEntity e, int count);
    void (*particle_set_emitting)(void *user, JceScriptEntity e, bool on);

    /* ── Component presence / enable (lightweight runtime reflection) ────────
     * Query / toggle a component on entity `e` BY NAME (the registry name, e.g.
     * "MeshRenderer", "RigidBody", "Light"), resolved through the dense
     * component registry by the host.  Pure POD across the boundary (no
     * component types leak here).  A NULL callback / unknown component name is a
     * safe default (has/enabled -> false; set -> no-op).
     *   has_component         : true when `e` carries the named component.
     *                           Backs jce.has_component(entity, name).
     *   is_component_enabled  : true when present AND enabled (the per-component
     *                           enable flag).  Backs
     *                           jce.is_component_enabled(entity, name).
     *   set_component_enabled : enable (`on`) / disable the named component at
     *                           runtime (e.g. toggle a behaviour, hide a mesh).
     *                           Backs jce.set_component_enabled(entity,name,on). */
    bool (*has_component)(void *user, JceScriptEntity e, const char *comp_name);
    bool (*is_component_enabled)(void *user, JceScriptEntity e, const char *comp_name);
    void (*set_component_enabled)(void *user, JceScriptEntity e,
                                  const char *comp_name, bool on);

    /* ── Raycast-vehicle control (drive an authored Vehicle component) ──────
     * Operate on the live runtime's vehicle bound to entity `e` (a Vehicle
     * component in SCRIPT input mode).  A NULL callback / an entity with no live
     * vehicle is a safe no-op / 0.
     *   vehicle_set_input : push drive input — throttle & brake in [0..1] (throttle
     *                       may be negative for reverse), steer in [-1..1] (scaled
     *                       to the vehicle's max steering).  Backs
     *                       jce.vehicle_set_input(entity, throttle, brake, steer).
     *   vehicle_get_speed : signed forward speed (m/s).  Backs
     *                       jce.vehicle_get_speed(entity). */
    void  (*vehicle_set_input)(void *user, JceScriptEntity e,
                               float throttle, float brake, float steer);
    float (*vehicle_get_speed)(void *user, JceScriptEntity e);

    /* ── Raw player movement intent (the axes the host fed this frame) ──────
     * out[0]=steer/strafe (walk_x, -1..1), out[1]=throttle/forward (walk_z,
     * -1..1), out[2]=brake/jump (0 or 1).  Lets a script read WASD without an
     * authored action map (e.g. a car_controller in SCRIPT vehicle mode).  Backs
     * jce.get_move() -> steer, throttle, brake.  NULL callback -> all zero. */
    void  (*get_move)(void *user, float out[3]);

    /* ── Adaptive music director (FEATURE 5.3) ─────────────────────────────
     * Drive the runtime's single live JceMusicDirector (built from the scene's
     * MusicTrack component).  A NULL callback / no live director is a safe
     * no-op / 0.
     *   music_set_intensity      : set the game intensity (0..1, clamped),
     *                              re-layering the stems.  Backs
     *                              jce.music_set_intensity(value).
     *   music_get_intensity      : read the current intensity (0 when no
     *                              director).  Backs jce.music_get_intensity().
     *   music_request_transition : request a beat/bar-quantized switch to
     *                              segment `to_segment`; returns the absolute
     *                              playhead time it will fire (< 0 on miss).
     *                              Backs jce.music_request_transition(seg). */
    void  (*music_set_intensity)(void *user, float intensity);
    float (*music_get_intensity)(void *user);
    float (*music_request_transition)(void *user, int to_segment);
} JceScriptHost;

typedef struct JceScript JceScript;

/* A per-entity script instance handle. 0 == invalid. */
typedef uint32_t JceScriptInstance;

/* Create / destroy the VM. `host` is copied; pass NULL for a binding-less VM
 * (bindings become no-ops). */
JCE_API JceScript *jce_script_create(const JceScriptHost *host);
JCE_API void       jce_script_destroy(JceScript *s);

/* Load a `.lua` file through the engine filesystem (PAK / mounted dirs) and
 * create an instance bound to `owner`. Returns 0 on read/compile/run error
 * (the error is logged via the host). */
JCE_API JceScriptInstance jce_script_instantiate(JceScript *s,
                                                 const char *path,
                                                 JceScriptEntity owner);

/* Same, but from an in-memory chunk (tests / embedded scripts). `name` is a
 * short chunk name used in error messages. */
JCE_API JceScriptInstance jce_script_instantiate_source(JceScript *s,
                                                        const char *name,
                                                        const char *source,
                                                        JceScriptEntity owner);

/* Lifecycle dispatch. Safe with invalid handles (no-op). Errors are caught,
 * logged, and disable that instance's offending callback for the run. */
JCE_API void jce_script_call_start (JceScript *s, JceScriptInstance inst);
JCE_API void jce_script_call_update(JceScript *s, JceScriptInstance inst, float dt);
JCE_API void jce_script_release    (JceScript *s, JceScriptInstance inst);

/* Dispatch on_collision(self, other_entity) — called by the runtime when the
 * instance's physics body begins contact with another body.  `other_entity` is
 * the other body's entity id (0 if it is untagged, e.g. the character capsule).
 * No-op when the script defines no on_collision. */
JCE_API void jce_script_call_collision(JceScript *s, JceScriptInstance inst,
                                       JceScriptEntity other_entity);

/* Dispatch a script-to-script message: call method `msg_name` on instance
 * `inst` with the Lua args (number_arg, str_arg) — i.e. the receiver sees
 * `inst:msg_name(number_arg, str_arg)`.  `str_arg` may be NULL (the handler
 * then sees `nil` for that parameter).  This is the per-instance dispatch the
 * runtime drives from jce.send_message after resolving the target entity to its
 * live instance; games rarely call it directly.
 *
 * Tolerant by design (mirrors jce_script_call_collision): a no-op when `s` is
 * NULL / `inst` is invalid / the receiver defines no method named `msg_name`, so
 * a missing handler is a clean no-op rather than an error.  A runtime error
 * inside the handler is caught + logged via the host (never propagated). */
JCE_API void jce_script_call_message(JceScript *s, JceScriptInstance inst,
                                     const char *msg_name, double number_arg,
                                     const char *str_arg);

/* Dispatch an ANIMATION frame event: call method `on_anim_event` on instance
 * `inst` with the Lua args (id, name, f0, f1, i0) — i.e. the receiver sees
 * `inst:on_anim_event(id, name, f0, f1, i0)`.  `name` is the optional event
 * label; when it is NULL or empty the handler sees `nil` for that parameter
 * (numeric-only events authored without a name).  This is the per-instance
 * sink the runtime drives from the scene renderer's anim-event hook after
 * resolving the firing entity to its live script instance; games rarely call
 * it directly.
 *
 * The five scalars (NOT the engine's JceAnimEvent struct) are passed so this
 * generic Lua-host header stays free of any animation dependency.
 *
 * Tolerant by design (mirrors jce_script_call_message): a no-op when `s` is
 * NULL / `inst` is invalid / the receiver defines no `on_anim_event`, so a
 * missing handler is a clean no-op (most scripts won't define it) rather than
 * an error.  A runtime error inside the handler is caught + logged via the
 * host (never propagated). */
JCE_API void jce_script_call_anim_event(JceScript *s, JceScriptInstance inst,
                                        uint32_t id, const char *name,
                                        float f0, float f1, int i0);

/* Invoke a named GLOBAL Lua function by name, passing a single entity-id
 * argument: `fn_name(arg_entity)`.  This is the dispatch path for event
 * handlers that are NOT tied to a per-entity script instance — notably a
 * UIButton's authored on_click_handler, where the handler is a game-global
 * function and `arg_entity` is the clicked button's entity id.  The handler
 * may register itself by simply declaring a global function of that name.
 *
 * Returns true iff a global function of that name existed and was invoked
 * (regardless of whether the call itself errored — a runtime error inside the
 * handler is caught, logged, and still counts as "invoked").  Returns false
 * (no-op) when s/fn_name is NULL/empty or no such global function exists, so
 * callers can treat "no handler" as a clean no-op.  Errors are caught + logged
 * via the host, mirroring the lifecycle dispatchers above. */
JCE_API bool jce_script_call_named(JceScript *s, const char *fn_name,
                                   JceScriptEntity arg_entity);

/* Invoke a named GLOBAL Lua function passing an entity id AND a number:
 * `fn_name(arg_entity, value)`.  This is the dispatch path for UI widgets whose
 * authored on_value_changed handler is a game-global function receiving the
 * widget's entity id plus its new numeric value — UISlider.value,
 * UIToggle.is_on (0/1), or UIDropdown.selected_index.  Same tolerance + return
 * contract as jce_script_call_named (clean no-op + false when no such global
 * exists; runtime errors caught/logged and still count as invoked). */
JCE_API bool jce_script_call_named_num(JceScript *s, const char *fn_name,
                                       JceScriptEntity arg_entity, double value);

/* Invoke a named GLOBAL Lua function passing an entity id AND a string:
 * `fn_name(arg_entity, str)`.  This is the dispatch path for a UIInputField's
 * authored on_value_changed / on_submit handler (the widget's entity id plus
 * its current text).  `str` may be NULL (the handler then sees `nil`).  Same
 * tolerance + return contract as jce_script_call_named. */
JCE_API bool jce_script_call_named_str(JceScript *s, const char *fn_name,
                                       JceScriptEntity arg_entity, const char *str);

/* Number of live instances (diagnostics). */
JCE_API int  jce_script_instance_count(const JceScript *s);

/* Advance the cooperative coroutine scheduler by `dt` seconds: any coroutine
 * started with jce.start_coroutine() and parked on jce.wait_seconds() whose
 * timer has elapsed is resumed (running until its next wait or completion).
 * The runtime drives this once per gameplay tick with the (time-scaled) dt,
 * right after the per-instance on_update pass.  No-op when nothing is parked. */
JCE_API void jce_script_update_coroutines(JceScript *s, float dt);

/* ── Hot-reload ───────────────────────────────────────────────────────────
 * Recompile a script's source into a fresh module table and re-point live
 * instances at it WITHOUT re-running on_start, so per-instance state (the
 * `self` table) is preserved while the methods (on_update/on_collision/…) and
 * any module-level constants pick up the edit.  The runtime drives this from a
 * file watcher; games can also call it for scripted live-coding.
 *
 * Usage: mod = jce_script_compile_module(s, "@path", src, len); for each live
 * instance of that script: jce_script_rebind_instance(s, inst, mod); then
 * jce_script_release_module(s, mod) (rebound instances keep the module alive
 * via their metatable, so releasing the temp handle is safe). */
typedef uint32_t JceScriptModule;   /* 0 == invalid */

JCE_API JceScriptModule jce_script_compile_module(JceScript *s, const char *name,
                                                  const char *source, size_t len);
JCE_API void jce_script_rebind_instance(JceScript *s, JceScriptInstance inst,
                                        JceScriptModule mod);
JCE_API void jce_script_release_module(JceScript *s, JceScriptModule mod);

JCE_EXTERN_C_END

#endif /* JCE_SCRIPT_H */
