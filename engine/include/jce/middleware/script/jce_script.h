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
 *       on_fixed_update(self, dt) -- called once per PHYSICS step, with the
 *                                   fixed dt, BEFORE that step: 0..N times
 *                                   per rendered frame, never a varying dt
 *       on_collision(self, other) -- called on first physics contact (other =
 *                                    the other body's entity id)
 *       on_destroy(self)       -- called when the instance is released
 *     `self.entity` is the owning entity id; `self` is a per-instance table
 *     (per-instance state lives there) whose metatable indexes the script.
 *   - The global `jce` table exposes engine bindings backed by JceScriptHost:
 *       jce.log(msg)
 *       jce.get_position(entity) -> x,y,z      (nil if no transform)
 *       jce.set_position(entity, x,y,z)
 *       jce.set_parent(child, parent, keep_world) -> bool
 *       jce.get_parent(child) -> entity_or_zero
 *       jce.is_key_down(keycode) -> bool
 *       jce.set_time_scale(scale)              -- slow-mo / hitstop / fast
 *       jce.pause(bool)                        -- pause/resume the simulation
 *       jce.shake_camera(amount)               -- add camera trauma (0..1)
 *       jce.request_scene(path)                -- queue a level swap
 *       jce.is_transitioning()                 -- true while one runs
 *       jce.audio_play(e) / audio_stop(e)      -- the AUTHORED source
 *       jce.audio_is_playing(e)                -- ...is it sounding
 *       jce.save_game(path) / load_game(path) -- persist a session
 *       jce.overlap_sphere(x,y,z,r[,mask])    -- entities in a sphere
 *       jce.overlap_box(x,y,z,hx,hy,hz[,mask])-- entities in a box
 *       jce.raycast(ox,oy,oz, dx,dy,dz, max)   -- -> hit_entity,px,py,pz,
 *                                                 nx,ny,nz,dist (0 on a miss)
 *       jce.apply_impulse(entity, x,y,z)       -- impulse on a dynamic body
 *       jce.set_velocity(entity, x,y,z)        -- set linear velocity
 *       jce.get_velocity(entity) -> x,y,z      (nil if no body)
 *       jce.play_sound(path [,x,y,z] [,vol [,min,max,rolloff]])
 *                                              -- one-shot 2D/3D sound
 *       jce.ui_get_slider(entity) -> number    (nil if no slider)
 *       jce.ui_set_slider(entity, value)
 *       jce.ui_get_toggle(entity) -> bool      (nil if no toggle)
 *       jce.ui_set_toggle(entity, on)
 *       jce.ui_set_text(entity, str)
 *       jce.ui_get_progress(entity) -> number  (nil if no progress bar)
 *       jce.ui_set_progress(entity, value)     (clamped to [min,max])
 *       jce.ui_get_dropdown(entity) -> integer (nil if no dropdown)
 *       jce.ui_set_dropdown(entity, index)     (clamped to the option range)
 *       jce.ui_get_input_text(entity) -> string ("" if no input field)
 *       jce.ui_set_input_text(entity, str)
 *       jce.ui_get_scroll(entity) -> x,y       (nil if no scroll view)
 *       jce.ui_set_scroll(entity, x, y)        (clamped per axis)
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
 *       jce.asset_read_text(path) -> string|nil
 *                                              -- bounded virtual text asset
 *       jce.asset_read_json(path) -> table|nil,error
 *                                              -- strict bounded JSON asset;
 *                                                 JSON null is jce.json_null
 *       jce.get_touch_count() -> integer       -- current frame touch sample
 *       jce.get_touch(index) -> id,x,y,p|nil   -- one-based touch lookup
 *
 *   THIS LIST IS A SAMPLE, NOT THE TABLE.  It stopped being complete a while
 *   ago -- jce.get_param, jce.get_param_text and jce.curve_eval are all
 *   missing from it, among others -- and prose beside a generated surface is
 *   the one part of a file that nothing checks, so it drifts silently and
 *   then misleads.  Rather than pretend, it says so: the authoritative list
 *   is engine/src/middleware/script/script_exposure.json (the decisions),
 *   published as contracts/script-api.json (the contract), and the KEY SET
 *   is pinned by the hand-authored golden list in
 *   tests/middleware/script/test_jce_script_table_shape.c.
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

/* Text assets exposed to Lua are intentionally bounded.  Scripts can parse
 * authored sidecars without gaining an unbounded allocation primitive. */
#define JCE_SCRIPT_TEXT_ASSET_MAX_BYTES (1024u * 1024u)

/* Script-side cap on a single line_set_points upload.
 *
 * The scripting layer may not include scene headers (this file's own layering
 * note: jce_core + Lua only), so it cannot read JCE_LINE_MAX_POINTS.  The two
 * MUST be equal; jce_rt_script.c sees both and carries a compile-time
 * assertion that they are, so a change to either fails the build rather than
 * silently truncating a polyline at the smaller of the two. */
#define JCE_SCRIPT_LINE_MAX_POINTS 64

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
    /* Raw pointer input for scene-authored orbit/strategy controls.  Button
     * numbers are 1-based (1 = left).  Deltas/wheel are per-frame values. */
    void  (*pointer_delta)(void *user, float out_xy[2]);
    float (*pointer_wheel)(void *user);
    bool  (*pointer_button)(void *user, int button);
    /* Backend-neutral touch sample.  Indices are zero-based at the host
     * boundary; the Lua binding presents one-based indices. */
    int   (*touch_count)(void *user);
    bool  (*touch_get)(void *user, int index, uint64_t *id,
                       float *x, float *y, float *pressure);
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
    /* Read a script or text asset (PAK / mounted dirs). Returns a heap buffer
     * the VM frees with jce_free, or NULL on miss. Required for
     * jce_script_instantiate(path) and backs bounded asset reads.
     *
     * Bounded-call convention: a caller may initialize *out_size to a nonzero
     * byte cap. A host that understands the convention rejects before a large
     * allocation and leaves the full required size in *out_size. Legacy hosts
     * may treat it as output-only; the VM still validates after the callback.
     * Zero on entry means unbounded (used for loading the script itself). */
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
    /* particle_set_color : retint the entity emitter's newly-spawned particles
     *                      (RGB; alphas preserved). Backs
     *                      jce.particle_set_color(entity, r, g, b). */
    void (*particle_set_color)(void *user, JceScriptEntity e,
                               float r, float g, float b);

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

    /* ── Scene-driver surface (editor-Play/runtime logic parity) ──────────
     * Lets a scene-bound script own look/season/weather logic that used to
     * require app exe code, so editor Play simulates identically to the
     * shipped runtime.
     *   find_by_name   : entities whose NAME matches exactly. Returns count;
     *                    Lua exposes the first entity plus that match count so
     *                    strict scene directors can reject duplicate names.
     *   find_by_prefix : entities whose name STARTS WITH prefix (bulk sets
     *                    like "leaf_"). Returns count.
     *   comp_get_json  : serialize one live component ("Water", "PointLight",
     *                    "MeshRenderer", ...) to its authored scene-JSON
     *                    object. Heap string — release via json_free.
     *   comp_set_json  : apply a JSON object of authored fields to a
     *                    component (routes through the typed setters, so
     *                    epochs/derived state update like a scene load).
     *   render_get_json/render_set_json : same for the scene-level rendering
     *                    settings (dome/fog/ambient/postfx). set merges —
     *                    absent keys keep their current value.
     *   audio_set_volume : live volume of the entity's AudioSource voice
     *                    (looping soundscapes; base for occlusion math). */
    int   (*find_by_name)(void *user, const char *name,
                          JceScriptEntity *out, int max);
    int   (*find_by_prefix)(void *user, const char *prefix,
                            JceScriptEntity *out, int max);
    char *(*comp_get_json)(void *user, JceScriptEntity e, const char *type);
    bool  (*comp_set_json)(void *user, JceScriptEntity e, const char *type,
                           const char *json);
    char *(*render_get_json)(void *user);
    bool  (*render_set_json)(void *user, const char *json);
    void  (*json_free)(void *user, char *s);
    void  (*audio_set_volume)(void *user, JceScriptEntity e, float volume);

    /* ── APPEND ONLY BELOW THIS LINE ──────────────────────────────────
     *
     * This struct is a TABLE OF FUNCTION POINTERS the GAME fills in and the
     * engine copies whole (jce_script.c: `s->host = *host;`).  Inserting a
     * member anywhere but the end shifts every slot after it, so a game built
     * against an older header makes the engine call through the WRONG SLOT —
     * not garbage data, a jump to a different function with a different
     * signature.
     *
     * set_parent / get_parent were first added after set_scale, i.e. in the
     * middle.  A HUMAN READING THE DIFF caught that — this comment used to
     * credit the ABI snapshot gate, and the gate could not have: it compared
     * the whole normalised record text and said CHANGED for an append and an
     * insertion alike, so "append only" was a sentence nothing enforced.
     *
     * It is enforced now, by the ORDERED-PREFIX RULE in
     * check_abi_snapshot.py: for a record in both the old and the
     * new snapshot, the old member list must be a PREFIX of the new one.  An
     * append passes; an insertion, a reorder or a rename fails and the report
     * names the member and its index.  It runs unskippably in
     * tools/lint/run_all.py (`check_abi_snapshot.py --committed`) and in
     * run_architecture_audit.py, and is covered by
     * test_abi_ordered_prefix.py.
     *
     * Appending is behaviourally identical and safe for an older consumer,
     * whose shorter struct simply leaves these NULL — which callers must
     * check anyway, as they already do for every optional host hook. */

    /* Hierarchy ownership. Entity 0 means no parent. set_parent validates the
     * operation in the host and reports whether it was applied. */
    bool (*set_parent)(void *user, JceScriptEntity child,
                       JceScriptEntity parent, bool preserve_world);
    JceScriptEntity (*get_parent)(void *user, JceScriptEntity child);

    /* Extended positional one-shot playback.  Appended for host ABI safety.
     * Lua selects this callback only when all attenuation arguments are
     * supplied: jce.play_sound(path,x,y,z,volume,min,max,rolloff).  Older
     * hosts fall back to play_sound and retain the engine defaults. */
    void (*play_sound_spatial)(void *user, const char *path,
                               const float pos[3], float volume,
                               float min_distance, float max_distance,
                               float rolloff);

    /* Game-content localization (L10n).  Appended for host ABI safety: an
     * older host leaves these NULL and the Lua side degrades to key
     * passthrough, which is exactly what jce_loc_t does when a key is
     * missing -- so an unlocalized build shows keys, never empty strings.
     *   loc_translate : key -> localized string, or the key itself
     *   loc_get_locale: current locale name ("" before one is selected)
     *   loc_set_locale: switch locale and reload its table at runtime */
    const char *(*loc_translate)(void *user, const char *key);
    const char *(*loc_get_locale)(void *user);
    void        (*loc_set_locale)(void *user, const char *locale);

    /* Appended for host ABI safety, same rule as the L10n trio above.
     *
     * These two landed mid-struct on the branch they came from, at slots 7
     * and 8, which shifts every later slot by two.  jce_script.c:633 copies
     * min(host_size, sizeof) raw bytes over a zeroed table, so a caller
     * still built against the 74-member layout would have had its slot 7
     * read as get_world_position -- a call through a pointer of a different
     * signature, silent rather than a crash.  Appending keeps every existing
     * slot where it was and leaves script_api_version at 1. */
    /* WORLD position: the local TRS composed up the parent chain.
     *
     * get_position above returns the entity's own LOCAL translation -- that is
     * what jce_scene_get_transform holds, and jce_scene.h says so where it
     * introduces jce_scene_get_world_matrix.  The exposure doc used to claim
     * get_position was world-space; it was not, and there was no way to ask
     * for the world pose from a script at all.  Any scene that parents its
     * parts (every mechanism rig does) therefore had a documented API that
     * returned a different quantity from the documented one.
     *
     * Both are kept: a rig solver wants the local value, a distance or an
     * aim wants the world one.  The doc on each now says which. */
    bool (*get_world_position)(void *user, JceScriptEntity e, float out_xyz[3]);
    /* Upload a polyline's points to the entity's LineRenderer in ONE call.
     *
     * Before this the only route from a script was comp_set with the whole
     * component re-serialised as JSON carrying flat "px0","py0","pz0","px1"...
     * keys.  cJSON looks a key up by walking the object's child list, so the
     * i-th point costs O(i) comparisons and a polyline costs O(n^2): space/'s
     * S3 frame measured ~135k string compares and ~1700 cJSON nodes -- about
     * 3400 CRT malloc/free per frame, outside the engine's own allocator.
     * The component stores the points as a packed float array the whole time;
     * only the transport was quadratic.
     *
     * `xyz` is 3*count floats, tightly packed.  Returns the number actually
     * stored (clamped to JCE_LINE_MAX_POINTS) so a script can tell when its
     * polyline was too long instead of finding out by looking at the screen. */
    int  (*line_set_points)(void *user, JceScriptEntity e,
                            const float *xyz, int count);

    /* UIProgressBar's value.
     *
     * The third value widget, and the one a gameplay script drives most often
     * -- health, loading, a cooldown -- was the only one with no accessor,
     * while UISlider and UIToggle have had a get/set pair since the UI landed.
     * A script could still reach it through comp_get/comp_set, but that
     * re-serialises the WHOLE component as JSON on every write, which is the
     * per-frame cost line_set_points was appended to avoid for polylines.
     *
     * get returns false when `e` carries no UIProgressBar, so a script can
     * tell "no bar" from "a bar reading zero" -- the same shape as
     * ui_get_slider, deliberately: two value widgets whose accessors disagree
     * about how absence is reported is a difference every caller has to learn.
     * The value is in the component's OWN [min_value, max_value] range and NOT
     * 0..1 -- the bar's fill is clamp((value-min)/(max-min)), so 0..1 is the
     * FILL, not the value.  Same convention as ui_get_slider, whose component
     * carries the same pair.  set clamps into that range rather than refusing,
     * because the draw already clamps: storing outside it would make the
     * component and the picture disagree, and a script reading back what it
     * just wrote would get a number the bar is not showing. */
    bool (*ui_get_progress)(void *user, JceScriptEntity e, float *out);
    void (*ui_set_progress)(void *user, JceScriptEntity e, float v);

    /* ── The three widgets that had no accessor ──────────────────────────
     *
     * UIDropdown's selection, UIInputField's text and UIScrollView's scroll
     * offset were reachable ONLY through comp_get_json / comp_set_json.  That
     * route works -- checked in both directions before adding these, because
     * MeshRenderer.visible once did not -- and it costs a full serialize AND
     * parse of the whole component to read one value: UIInputField writes
     * about thirty keys, four colours and two 256-byte paths among them, to
     * answer "what did the player type".  A HUD that samples an input field
     * every frame pays that every frame.  Same argument the manifest already
     * makes for line_set_points.
     *
     * Every get answers FALSE when the entity carries no such widget, so a
     * script can tell "no dropdown" from "a dropdown reading 0" -- the shape
     * ui_get_slider and ui_get_progress already use.
     *
     * ui_get_input_text returns a POINTER INTO THE COMPONENT, valid until the
     * next mutation of that entity, exactly like tr() and get_locale().  Every
     * binding copies it before returning to script code; none of them may
     * store it.  It is not an owned_string_release because there is nothing to
     * release -- the buffer is the component's own field, and handing back a
     * heap copy would make the common case allocate to read a name.
     *
     * ui_set_dropdown CLAMPS into [0, option_count-1] rather than refusing,
     * and ui_set_scroll clamps each axis the way the wheel path does, for the
     * reason ui_set_progress gives: the draw already clamps, so storing
     * outside the range would make the component and the picture disagree and
     * a script reading back what it just wrote would get a number the widget
     * is not showing. */
    bool (*ui_get_dropdown)(void *user, JceScriptEntity e, int *out);
    void (*ui_set_dropdown)(void *user, JceScriptEntity e, int index);
    const char *(*ui_get_input_text)(void *user, JceScriptEntity e);
    void (*ui_set_input_text)(void *user, JceScriptEntity e, const char *text);
    bool (*ui_get_scroll)(void *user, JceScriptEntity e, float out_xy[2]);
    void (*ui_set_scroll)(void *user, JceScriptEntity e, float x, float y);

    /* ── World domain: the clock and the weather ───────────────────────
     *
     * Gameplay could not ask what time it was.  `time_of_day` and `weather`
     * occurred ZERO times in contracts/script-api.json, and the live hour was
     * a private field of the scene RENDERER until the environment authority
     * moved to the scene -- so "is it night?", the single most ordinary
     * question a quest or an NPC schedule asks, was unexpressible in every one
     * of the seven scripting languages.
     *
     * Scene-scoped with no handle argument: a script's world is the runtime's
     * current scene, and the host resolves it.  Handing scripts a JceScene*
     * would put a raw engine pointer in a sandbox for no gain -- there is only
     * ever one.
     *
     * The hour is the LIVE one (jce_scene_environment_hour), not the authored
     * seed in JceSceneRenderingSettings::tod_hour: a script that read the seed
     * would get the level's start-of-day forever while the sky moved.
     *
     * world_set_hour moves the live clock only; the authored seed is left
     * alone, so reloading the scene still starts where the designer set it.
     * That is what makes "sleep until dawn" expressible without a script
     * silently editing the level. */
    float (*world_get_hour)(void *user);
    void  (*world_set_hour)(void *user, float hour);
    bool  (*world_is_daytime)(void *user);
    int   (*world_get_weather)(void *user);
    float (*world_get_weather_intensity)(void *user);
    float (*world_get_wind_speed)(void *user);

    /* ── Level transition (FEATURE 9.4) ───────────────────────────────
     *
     * jce_runtime_request_scene queues a swap to another authored scene
     * WITHOUT tearing the runtime down: physics world, script VM, audio
     * device and save registry survive, and a FADE_OUT / LOAD / FADE_IN state
     * machine runs inside jce_runtime_step.  Its own header says "Safe to
     * call from gameplay scripts / triggers" -- and it had ZERO CALLERS
     * anywhere in the tree, in either host, with no binding in any of the
     * seven languages.  A game built on this engine could not change level.
     *
     * request_scene returns false on a NULL runtime, an empty path, or when a
     * transition is already in flight (the in-flight one wins).  Gate on
     * is_transitioning rather than retrying: a script that calls every frame
     * would otherwise spin against a swap that is already happening.
     *
     * APPENDED at the end of this struct.  JceScriptHost is copied BY BYTES
     * with min(caller, engine) size, so a member inserted anywhere but here
     * silently calls a differently-typed pointer in every host built against
     * an older header. */
    bool  (*request_scene)(void *user, const char *scene_path);
    bool  (*is_transitioning)(void *user);

    /* ── AudioSource control (Unity's Play / Stop / isPlaying) ─────────
     *
     * An authored JceAudioSourceComponent sounded EXACTLY ONCE, at scene
     * spawn, and only with play_on_awake.  jce.play_sound(path, ...) is the
     * only other route and it discards everything the component authors --
     * loop, pitch, per-source volume, mixer bus, the whole 3D attenuation
     * block -- so a door creak, a gunshot or a script-armed alarm could not
     * use the component at all.
     *
     * audio_play RESTARTS a source that is already sounding, which is Unity's
     * semantics and stops a repeating event stacking voices until the mixer
     * runs out.  APPENDED, for the reason above this pair. */
    bool  (*audio_play)(void *user, JceScriptEntity e);
    bool  (*audio_stop)(void *user, JceScriptEntity e);
    bool  (*audio_is_playing)(void *user, JceScriptEntity e);

    /* ── Save / load a session ────────────────────────────────────────
     *
     * The save system shipped WRITE-ONLY.  jce_runtime_save_to_file had no
     * counterpart in the engine and no binding in any of the seven languages,
     * so gameplay code could not persist anything at all -- not a checkpoint,
     * not a slot, not a key/value.  A game could write .jsnp files nothing in
     * its own executable could read back.
     *
     * save_game returns false when there is no registry or the write fails.
     * load_game restores AND rebuilds the runtime (physics bodies, script
     * instances, voices, triggers); a missing slot returns false having
     * changed nothing, so `if not jce.load_game(slot) then new_game() end` is
     * the correct shape for a Continue button.
     *
     * APPENDED. */
    bool  (*save_game)(void *user, const char *path);
    bool  (*load_game)(void *user, const char *path);

    /* ── Spatial queries ──────────────────────────────────────────────
     *
     * jce_physics_overlap_sphere / _overlap_box / _raycast_all are
     * implemented down to Bullet, honour the layer mask and the trigger skip,
     * and had ZERO consumers outside their own module.  The one physics query
     * a script could reach was a closest-hit raycast that ignored the filter.
     *
     * So "what is inside this sphere" -- explosion damage, melee arcs, aggro
     * and proximity checks, line-of-sight fans, pickup detection -- had to be
     * faked by walking entities and comparing distances in script, which
     * ignores colliders, layers and triggers entirely.
     *
     * `layer_mask` 0 means every layer (the filter's 0xFFFFFFFF); the
     * results are ENTITIES, mapped from body handles by the runtime, which is
     * the only thing that holds that mapping.  Entities whose body the runtime
     * does not know (a body created outside the spawn walk) are skipped rather
     * than reported as entity 0.
     *
     * APPENDED. */
    int   (*overlap_sphere)(void *user, float x, float y, float z,
                            float radius, uint32_t layer_mask,
                            JceScriptEntity *out, int max);
    int   (*overlap_box)(void *user, float x, float y, float z,
                         float hx, float hy, float hz, uint32_t layer_mask,
                         JceScriptEntity *out, int max);

    /*
     * "Cut to the camera called BossIntro."
     *
     * JceVirtualCameraComponent.vcam_name was authored, serialised and shown
     * in the editor's VCam Manager while NOTHING under engine/src looked at
     * it: the vcam system picks the highest-priority active camera, so the
     * name was a label in a panel.  A cutscene, a trigger, or a scene authored
     * from the SDK could not name a shot at all.
     *
     * It does NOT rewrite the authored components -- the override lives in the
     * vcam system beside the damping state, so Ctrl+S cannot bake a cutscene's
     * camera choice into the level.  Returns 1 when the name resolves to a
     * camera that is active and enabled, 0 otherwise; the request is recorded
     * either way, so naming a camera in a streaming cell that has not loaded
     * yet does not silently become "whatever priority says".
     *
     * Passing NULL or "" clears the override and hands the decision back to
     * priority.
     *
     * APPENDED. */
    int   (*vcam_activate)(void *user, const char *name);

    /* Read an AUTHORED parameter off the entity's script component -- Unity's
     * [SerializeField] and Godot's @export, reaching a running script.
     *
     * A HOST CALLBACK AND NOT A NEW JceScriptVM SLOT, deliberately.  A VM slot
     * is the right shape for a LIFECYCLE hook, because a callback that exists
     * in six languages and not the seventh is a half-feature -- and it costs
     * seven backends, two lifecycle differentials and three probe tables to
     * add one.  A parameter READ is the same shape as get_position: one entry
     * in this table reaches all seven languages through bindings that are
     * already generated from it.
     *
     * Returns false when the entity has no script component, when no
     * parameter of that name is authored, or when `name` is NULL/empty --
     * three cases a script cannot distinguish and should not need to, since
     * all three mean "the author did not give me this".  `out` is untouched
     * on false, so a caller's default survives.
     *
     * APPENDED. */
    bool  (*get_script_param)(void *user, JceScriptEntity e, const char *name,
                              int *out_kind, double *out_number,
                              JceScriptEntity *out_entity);

    /* The TEXT value of an authored parameter, or "" when the entity has no
     * script component, no parameter of that name, or one whose kind is not
     * TEXT.  Empty rather than NULL for the same reason ui_get_input_text is:
     * a script comparing strings should not have to test for nil first.
     *
     * RETURNS A POINTER INTO THE COMPONENT, valid until the component is
     * reassigned or the entity destroyed -- the identical lifetime rule
     * ui_get_input_text documents, and the identical obligation on a caller
     * that wants to keep it: copy it.
     *
     * APPENDED. */
    const char *(*get_script_param_text)(void *user, JceScriptEntity e,
                                         const char *name);

    /* Sample an authored curve asset -- the documents the editor's Curve
     * Editor writes, which until now nothing anywhere could read.
     *
     * FALSE, with *out_value untouched, when the path does not resolve, the
     * document does not parse, or the curve has no channel of that name.  A
     * curve that legitimately evaluates to 0 and a curve that is not there
     * must not be one reading -- the same rule get_script_param follows, and
     * it is the whole reason this is not `float curve_eval(...)`.
     *
     * The host caches the parsed curve per runtime, so a call inside
     * on_update costs a name compare rather than a JSON parse.
     *
     * `*out_value` untouched is THIS table's contract and not the C ABI's:
     * the generated jce_script_api_* forwarder zeroes every out slot on its
     * absent branch, uniformly, for every fallible_out -- because across a C
     * ABI handing back an unwritten buffer is worse than a defined value.
     * Both rules are right where they are; they are not one promise.
     *
     * APPENDED. */
    bool  (*curve_eval)(void *user, const char *path, const char *channel,
                        double t, double *out_value);

    /* ── Filtered and multi-hit raycasts ──────────────────────────────
     *
     * `raycast` above takes no filter at all: closest hit, every layer,
     * triggers decided by whatever the C default happens to be.  The overlap
     * queries beside it have taken a layer_mask since they landed, and the
     * comment on them records this same defect being fixed for THEM -- the
     * ray was left as it was.  So a script could ask "what is inside this
     * sphere, ignoring the player" and could not ask "what did this shot
     * hit, ignoring the player", which is the more common question of the
     * two.
     *
     * Both of these are pure READERS of physics that already exists:
     * jce_physics_raycast_filtered and jce_physics_raycast_all are
     * implemented down to Bullet, honour the mask and the trigger skip, and
     * had no script binding.
     *
     * `layer_mask` 0 means every layer, the same convention overlap_sphere
     * uses -- so the common call stays origin/dir/distance and the filter is
     * the thing you add when you need it.  `hit_triggers` is separate from
     * the mask because a trigger volume is not a layer: Unity splits them the
     * same way (layerMask vs QueryTriggerInteraction) and collapsing them
     * would make "ignore triggers on layer 3" inexpressible.
     *
     * raycast_all writes ENTITIES sorted near->far and returns how many.
     * Entities whose body the runtime does not know are skipped rather than
     * reported as entity 0 -- the rule the overlap queries already follow.
     * It returns entities and not full hits because the eight-value hit
     * record does not survive as an array shape across seven languages
     * without inventing a per-language container; a script that needs the
     * point and normal of a specific one re-queries it with raycast_filtered.
     * That is a real limit and it is named in the ledger rather than hidden.
     *
     * APPENDED. */
    bool  (*raycast_filtered)(void *user, const float origin[3],
                              const float dir[3], float max_dist,
                              uint32_t layer_mask, bool hit_triggers,
                              JceScriptRaycastHit *out);
    int   (*raycast_all)(void *user, const float origin[3],
                         const float dir[3], float max_dist,
                         uint32_t layer_mask, bool hit_triggers,
                         JceScriptEntity *out, int max);
} JceScriptHost;

typedef struct JceScript JceScript;

/* A per-entity script instance handle. 0 == invalid. */
typedef uint32_t JceScriptInstance;

/* Create / destroy the VM. `host` is copied; pass NULL for a binding-less VM
 * (bindings become no-ops).
 *
 * Pass sizeof(JceScriptHost) as `host_size`.  JceScriptHost is allocated by
 * the CALLER and gains members over time, so the engine must not copy it at
 * its own sizeof — that reads past the end of a host built against an older
 * header and then calls whatever followed it.  With the size, the engine
 * copies min(caller, engine) over a zeroed table; members the caller does not
 * have stay NULL and are skipped (every call site null-checks).
 *
 * The macro below applies this automatically, so ordinary callers keep
 * writing jce_script_create(&host).  Define JCE_NO_SCRIPT_HOST_SIZE_SHIM to
 * reach the raw symbol (it assumes your layout matches the engine's). */
JCE_API JceScript *jce_script_create_sized(const JceScriptHost *host,
                                           size_t host_size);
JCE_API JceScript *jce_script_create(const JceScriptHost *host);
JCE_API void       jce_script_destroy(JceScript *s);

#if !defined(JCE_BUILDING_ENGINE) && !defined(JCE_NO_SCRIPT_HOST_SIZE_SHIM)
#  define jce_script_create(host) \
       jce_script_create_sized((host), sizeof(JceScriptHost))
#endif

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

/* ── THE FAILING-CALLBACK RULE ────────────────────────────────────────────
 *
 * One rule, stated once here and obeyed by every JceScriptVM backend: WHEN A
 * FIXED-NAME LIFECYCLE CALLBACK RAISES, IT IS CAUGHT, LOGGED, AND THEN
 * DISABLED ON THAT INSTANCE.  Without the third clause a script that errors
 * every frame writes one log line per frame forever — sixty a second, from a
 * defect that was fully described by the first one.
 *
 * WHICH CALLBACK.  Exactly the handler that raised, on exactly the instance it
 * raised on.  `on_update` failing on entity A leaves `on_collision` on A and
 * `on_update` on B running.  The participating handlers are the REPEATING ones
 * whose NAME is fixed by this header and therefore means the same thing in
 * every backend:
 *
 *     on_start   on_update   on_collision   on_anim_event
 *
 * WHAT DOES NOT PARTICIPATE, AND WHY EACH DOES NOT.  Three exclusions, none of
 * them an oversight; each is pinned by a named test so that "make it uniform"
 * has to argue with something.
 *
 *   - `on_destroy`.  It is dispatched exactly once, by jce_script_release,
 *     which drops the instance on the next line.  There is no second call for
 *     a disable to suppress, so setting the flag would be a store nothing can
 *     read and the notice below would tell the user to hot-reload an instance
 *     that no longer exists — advice that is not merely useless but false.
 *     Its error is still caught and logged.
 *   - jce_script_call_message().  Its method name comes from the CALLER, and
 *     the C++ backend routes every name through one `on_message` thunk, so
 *     "the offending callback" has no identity the four backends share.
 *   - jce_script_call_named() and its two siblings; see their own comment for
 *     why a named global has nothing to be disabled on.
 *
 * Note what the four have in common and the three do not: each of the four can
 * fire again, unboundedly, on the same instance.  That is the whole reason the
 * rule exists.
 *
 * FOR HOW LONG.  For the life of the instance's current binding.  The disable
 * never expires on its own — an expiry would just restart the spam.  It is
 * lifted by exactly two things:
 *
 *   - jce_script_rebind_instance() (hot reload).  Rebinding is the engine
 *     saying "the code behind this instance may have changed", so it clears
 *     EVERY disabled handler on that instance.  Without this a script you
 *     fixed and saved would stay dead until the process restarted, which is a
 *     worse defect than the one the rule exists to fix.
 *   - releasing the instance and instantiating it again (respawn, scene
 *     reload, VM destroy).  A fresh instance has nothing disabled.
 *
 * HOW A USER FINDS OUT.  A disabled callback that reported nothing would be
 * indistinguishable from one the script never declared, so the disable
 * announces itself: immediately after the "<handler> error: <detail>" line,
 * and through the SAME sinks (JceScriptHost::log and the engine log), the
 * backend writes
 *
 *     <handler> disabled for this script instance after the error above;
 *     hot-reload the script or respawn the entity to re-enable
 *
 * once, and then nothing further from that handler.  There is deliberately no
 * query API: adding one would mean adding a JceScriptVM slot, and that vtable
 * is pinned by a compile-time completeness assertion and by
 * check_script_vm_parity.py.  The log line is the signal.
 *
 * *Enforced by:* tests/middleware/script/test_jce_script_disable.c (Lua, the
 * reference), and cross-language by all three lifecycle differentials —
 * tests/scripting/python/lifecycle_differential.py,
 * tests/scripting/java/vm/run_lifecycle_differential.py and
 * tests/scripting/cpp/test_jce_script_vm_cpp_lifecycle.cpp.  Each drives a
 * handler that raises and then the SAME handler again, and compares the
 * resulting host-call stream against Lua's; the python and java ones go on to
 * rebind and require the handler back, while the cpp one pins the opposite for
 * the reason given at jce_script_rebind_instance below.
 */

/* The EXACT text of the notice the rule above requires, spelled once so that
 * four backends cannot each invent their own wording.  `%s` is the handler
 * name.  The three lifecycle differentials compare this line byte for byte
 * across languages (it carries no language-specific detail, unlike the error
 * line above it, which is why it can be compared at all).
 *
 * The Python VM cannot include this header; scripting/python/jce_script/vm.py
 * carries the same string and tests/scripting/python/test_emit_python.py ::
 * test_the_disabled_notice_matches_the_engine fails if the two ever differ. */
#define JCE_SCRIPT_DISABLED_NOTICE_FMT \
    "%s disabled for this script instance after the error above; " \
    "hot-reload the script or respawn the entity to re-enable"

/* Lifecycle dispatch. Safe with invalid handles (no-op). Errors are caught,
 * logged, and disable that instance's offending callback until the instance is
 * rebound or replaced — see THE FAILING-CALLBACK RULE above. */
JCE_API void jce_script_call_start (JceScript *s, JceScriptInstance inst);
JCE_API void jce_script_call_update(JceScript *s, JceScriptInstance inst, float dt);

/* Dispatch on_fixed_update(self, dt) — once per PHYSICS step, with the fixed
 * dt, immediately BEFORE the step so a force applied here is integrated by
 * that very step.  This is Unity's FixedUpdate, Unreal's substepped tick and
 * Godot's _physics_process, and it exists for the reason all three do:
 * on_update runs on a render frame whose dt varies with the frame rate, so a
 * script that applies force there produces different physics on a fast
 * machine than on a slow one.  A jump that clears a gap at 144 Hz and does
 * not at 30 Hz is the bug, and no amount of care inside on_update fixes it.
 *
 * ZERO OR MANY TIMES PER RENDERED FRAME.  The fixed clock is an accumulator:
 * a frame that took 3 fixed steps dispatches this 3 times, one that took none
 * dispatches it not at all.  A script that assumes "once per frame" wants
 * on_update; one that assumes "the same dt every time" wants this.
 *
 * Same tolerance and failing-callback rule as on_update: a no-op when the
 * script defines no on_fixed_update, and a runtime error inside it disables
 * on_fixed_update on that instance without touching on_update.  The two have
 * separate disable bits on purpose -- a handler that throws every physics
 * step must not take the render-frame callback down with it. */
JCE_API void jce_script_call_fixed_update(JceScript *s, JceScriptInstance inst,
                                          float dt);

JCE_API void jce_script_release    (JceScript *s, JceScriptInstance inst);

/* Dispatch on_collision(self, other_entity) — called by the runtime when the
 * instance's physics body begins contact with another body.  `other_entity` is
 * the other body's entity id (0 if it is untagged, e.g. the character capsule).
 * No-op when the script defines no on_collision.  A runtime error inside the
 * handler is caught, logged and DISABLES on_collision on that instance — see
 * THE FAILING-CALLBACK RULE above; a body resting against a wall re-fires this
 * as readily as on_update re-fires per frame. */
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
 * inside the handler is caught + logged via the host (never propagated).
 *
 * WHY THIS DISPATCHER DOES NOT DISABLE.  `msg_name` is chosen
 * by the caller, so there is no fixed handler identity for the rule above to
 * name — and the C++ backend cannot supply one even in principle: it routes
 * every message name through a single `on_message` thunk, so disabling "the
 * offending callback" there would disable every message the instance receives.
 * Rather than have the four backends mean different things by one sentence,
 * this dispatcher keeps log-and-continue.  A message handler that errors on
 * every send therefore still logs on every send; the send rate is set by the
 * script that calls jce.send_message, not by the frame clock. */
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
 * an error.  A runtime error inside the handler is caught, logged and DISABLES
 * on_anim_event on that instance — see THE FAILING-CALLBACK RULE above. */
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
 * via the host, mirroring the lifecycle dispatchers above.
 *
 * A NAMED GLOBAL IS NEVER DISABLED, and that is the deliberate exception to
 * THE FAILING-CALLBACK RULE.  Three reasons, any one of them sufficient:
 *
 *   - the rule disables a callback ON AN INSTANCE, and a global has none.  In
 *     Lua the handler lives in `_G` and is shared by every widget bound to the
 *     name, so "disable it" would mean disabling all of them.
 *   - the return value has no room for it.  `false` already means "no such
 *     global", which every caller treats as a correctly-absent handler; a
 *     disabled handler answering `false` would make a UISlider / UIToggle /
 *     UIDropdown / UIInputField go silently dead with the widget reporting
 *     nothing wrong, and answering `true` without calling would be a lie.
 *   - the backends do not even agree on WHICH function a name resolves to
 *     (Lua: one `_G` entry; Python: the newest instance's module namespace;
 *     Java: a list of public static methods), so there is nothing stable to
 *     attach a disable to.
 *
 * A global handler that errors on every click therefore logs on every click.
 * That is a user-paced rate, not a frame-paced one. */
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
 * via their metatable, so releasing the temp handle is safe).
 *
 * A rebind also CLEARS every callback THE FAILING-CALLBACK RULE disabled on
 * that instance, because rebinding is the engine saying the code may have
 * changed.  That is what makes "fix the script and save" a working repair: a
 * handler disabled by an error in the old module runs again under the new one.
 * (The "cpp" backend has no rebind — a compiled class cannot be recompiled
 * in-process, its rebind_instance is a documented no-op, and its re-enable is
 * jce_script_vm_cpp_unload + _load_library + re-instantiate, which is also its
 * only hot-reload path.) */
typedef uint32_t JceScriptModule;   /* 0 == invalid */

JCE_API JceScriptModule jce_script_compile_module(JceScript *s, const char *name,
                                                  const char *source, size_t len);
JCE_API void jce_script_rebind_instance(JceScript *s, JceScriptInstance inst,
                                        JceScriptModule mod);
JCE_API void jce_script_release_module(JceScript *s, JceScriptModule mod);

JCE_EXTERN_C_END

#endif /* JCE_SCRIPT_H */
