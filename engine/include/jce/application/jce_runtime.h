/*
 * jce_runtime.h  Layer 6 — Application.
 *
 * Headless play-mode driver: takes an authored JceScene and brings it
 * to life — spins up a physics world from RigidBody / Collider /
 * CharacterController components, autoplays AudioSource clips, ticks
 * physics + scene_update + 3D audio listener every frame.
 *
 * Used by:
 *   - in-editor Play button (editor/src/core/jce_editor_play.cpp)
 *   - deployed game exe template (engine/src/application/jce_project.c
 *     TPL_MAIN_EMPTY)
 *
 * Both callers share this one driver so the editor preview matches the
 * shipped binary bit-for-bit (no behaviour drift between the two paths).
 *
 * Ownership: runtime never owns the scene — the caller stays responsible
 * for jce_scene_create / jce_scene_destroy.  Physics bodies, character
 * controller, and audio voices live for the JceRuntime's lifetime.
 */

#ifndef JCE_APPLICATION_RUNTIME_H
#define JCE_APPLICATION_RUNTIME_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <jce/middleware/physics/jce_physics_debug.h>  /* jce_contact_listener_fn */
#include <jce/middleware/world/jce_gas.h>  /* JceGameplayAbilitySystem (live GAS) */
#include <jce/middleware/scene/jce_world_origin.h>  /* JceWorldOrigin accessor */
#include <jce/middleware/animation/jce_anim_ik.h>  /* JceAnimEvent (anim-event dispatch) */
#include <jce/os/platform/jce_input_actions.h>  /* JceInputActions (data-driven action bridge) */
#include <stdbool.h>

JCE_EXTERN_C_BEGIN

typedef struct JceRuntime           JceRuntime;
typedef struct JceScene             JceScene;
typedef struct JcePakArchive        JcePakArchive;
typedef struct JceAudio             JceAudio;
typedef struct JcePhysicsWorld      JcePhysicsWorld;
typedef struct JceSnapshotRegistry  JceSnapshotRegistry;
typedef struct JceBtContext         JceBtContext;
typedef struct JceBlackboard        JceBlackboard;
/* Opaque ragdoll handle.  Defined in the INTERNAL animation header
 * (engine/src/middleware/animation/jce_ragdoll.h); forward-declared here as an
 * opaque type so jce_runtime_entity_ragdoll can hand it back WITHOUT exposing
 * the internal definition (which would drag physics/skeleton internals into the
 * public surface).  Callers that act on it include the internal header. */
typedef struct JceRagdoll           JceRagdoll;

#define JCE_RUNTIME_MAX_TOUCHES 5

typedef struct {
	uint64_t id;
	float x;
	float y;
	float pressure;
} JceRuntimeTouch;

typedef struct {
	/* Scene to drive — must remain valid for the runtime's lifetime. */
	JceScene      *scene;

	/* PAK used to resolve audio clip paths.  Pass svc->pak (with all
	 * bundle overlays already pushed) in deployed exes.  NULL disables
	 * scene audio autoplay. */
	JcePakArchive *pak;

	/* Audio engine — pass svc->audio.  NULL disables audio. */
	JceAudio      *audio;

	/* Spawn a physics world during create().  When false, RigidBody /
	 * CharacterController components are ignored and step() reduces to
	 * jce_scene_update only. */
	bool           enable_physics;

	/* Optional overrides — leave 0 for sensible defaults. */
	float          gravity_y;        /* default -9.81 */
	float          fixed_timestep;   /* default 1/60  */

    /* Optional audio resolver.  When set, the runtime calls this for
     * every synchronous one-shot and editor-hosted AudioSource clip instead
     * of `jce_audio_load(audio, pak, clip)`.
	 * Use this to plug in a host-filesystem probe (editor in-tree assets)
	 * or any custom path resolution layer.  Return JCE_SOUND_INVALID to
	 * signal "could not load" — the runtime will warn-log and skip the
	 * voice.  `user_data` is forwarded verbatim. */
	uint32_t     (*audio_load_fn)(void *user_data,
	                              JceAudio *audio,
	                              const char *clip_path);
	void          *user_data;

	/* Optional asset-path resolver.  When set, the runtime calls this to
	 * map a scene-stored (typically project-relative) asset path to a
	 * readable host path BEFORE it loads the file directly via jce_fs
	 * (currently: per-object compound / mesh collider model files and their
	 * sibling ".jcol" pre-cooked blobs).  This is the SAME resolution the
	 * editor's scene renderer uses for the visual mesh (jce_editor_scene_
	 * asset_cache_resolve_mesh_path), so a model-based collider resolves to
	 * the exact file its mesh renders from — without it, the editor (which
	 * never chdir's and mounts no global VFS in loose Play) resolves the raw
	 * relative path against the process CWD and the collider silently fails
	 * to spawn while the mesh still renders.  Write the resolved path into
	 * `out_path` (capacity `out_size`) and return true; return false to fall
	 * back to the raw path (the deployed `pak` path needs no resolver).
	 * `user_data` is forwarded verbatim (shared with audio_load_fn). */
	bool         (*resolve_path_fn)(void *user_data,
	                                const char *in_path,
	                                char *out_path, int out_size);

	/* Optional path to an audio_mixer.json (the same format the editor's
	 * Audio Mixer panel writes to ~/.jce/audio_mixer.json).  When set and
	 * readable, the runtime seeds its mixer bus tree from it so the authored
	 * Music/SFX/Voice/UI bus volumes (and mute/solo) drive live playback.
	 * NULL/"" or an unreadable path falls back to a default bus layout
	 * (Master/Music/SFX/Voice/UI). */
	const char    *mixer_config_path;

	/* Optional path to a .navmesh.bin baked by the editor's NavMesh panel
	 * (jce_recast_build_to_file).  When set and loadable, the runtime loads
	 * the Detour navmesh, creates a nav-agent set bound to it, and resolves
	 * agent paths through jce_recast_find_path.  NULL/"" or an unreadable
	 * path disables navigation (nav-agent set stays NULL). */
	const char    *navmesh_path;

	/* Optional base directory for save snapshots.  The runtime registers a
	 * scene/ECS snapshot provider at create() and, when the player overlaps
	 * an authored SavePoint volume, writes "<saves_dir>/<save_id>.jsnp" via
	 * jce_snapshot_save_to_file (the directory is created on demand).  Pass
	 * "<project>/saves" in the editor, the user write-dir in a shipped game.
	 * NULL/"" disables SavePoint auto-save (the snapshot provider is still
	 * registered, so a game can save/load through jce_runtime_save_registry). */
	const char    *saves_dir;

	/* Optional host directory containing game string tables as flat
	 * <locale>.json files (e.g. "<exe_dir>/resources/_cooked/i18n").  When
	 * set — or when `pak` is present, which enables the in-PAK
	 * "i18n/<locale>.json" source — the runtime initialises the
	 * process-global localization table (jce_loc_*) at create() and selects
	 * `locale` (below), so authored UIText locale_key fields resolve in
	 * shipped builds.  NULL/"" with pak==NULL leaves localization state
	 * completely untouched (editor Play passes neither: the editor owns
	 * jce_loc and its preview locale must survive Play sessions). */
	const char    *locales_dir;

	/* Initial locale tag for the localization init above, e.g. "en" /
	 * "zh_cn".  NULL/"" = auto: the host OS preferred locale
	 * (jce_host_preferred_locale), falling back to "en".  Ignored when
	 * localization is not initialised (see locales_dir). */
	const char    *locale;
} JceRuntimeDesc;

/* Player input applied to the scene's CharacterController each step.
 * walk_x / walk_z are a scene-space DIRECTION clamped to unit length —
 * the runtime scales it by the authored CharacterController move_speed
 * (and sprint_mult while `sprint` is held), so movement feel lives in
 * scene data, not in each caller.  speed_mult is an extra gameplay
 * multiplier on top (crouch, slow zones; 0 → treated as 1).
 * jump_pressed is edge-triggered — held inside the runtime until
 * consumed (it feeds a short jump buffer, so slightly-early presses
 * still jump on landing).  jump_held enables variable jump height:
 * releasing it while ascending cuts the jump short. */
typedef struct {
	float walk_x;
	float walk_z;
	bool  jump_pressed;
	float speed_mult;
	bool  sprint;
	bool  jump_held;
	bool  attack_pressed;   /* edge-triggered melee button (script: jce.attack_pressed) */
	/* Raw pointer state for data-driven gameplay scripts.  This is a frame
	 * sample: deltas, wheel, and buttons are cleared after each runtime step,
	 * so every host supplies the current state immediately before step().
	 * Button bits use JCE's 1-based numbering:
	 * bit 0 = button 1 (left), bit 1 = button 2 (middle), etc. */
	float pointer_dx;
	float pointer_dy;
	float pointer_wheel;
	uint32_t pointer_buttons;
	/* Bounded transient touch sample.  Hosts replace it each frame; the runtime
	 * clears it after step() so focus loss cannot leave a gesture held. */
	int touch_count;
	JceRuntimeTouch touches[JCE_RUNTIME_MAX_TOUCHES];
} JceRuntimeInput;

/* Snapshot the scene into physics/audio state.  Returns NULL on failure.
 * Reads `desc` once; no aliasing afterwards. */
JCE_API JceRuntime *JCE_CALL jce_runtime_create(const JceRuntimeDesc *desc);

/* Tear down bodies, stop voices, free the runtime.  Safe with NULL. */
JCE_API void        JCE_CALL jce_runtime_destroy(JceRuntime *rt);

/* Advance one frame.  Physics is driven by an internal fixed-timestep
 * accumulator (Glenn Fiedler "Fix Your Timestep"): the frame `dt` is
 * banked and the simulation is advanced in whole `fixed_timestep` chunks
 * (from the world desc, default 1/60), with a per-frame step clamp to
 * avoid the spiral of death.  Steps:
 *   1. drive character controller from the most recent set_input()
 *   2. run N fixed physics ticks (N = banked time / fixed_timestep)
 *   3. write INTERPOLATED physics transforms back into scene Transform
 *      components using the residual accumulator as the blend factor, so
 *      rendering stays smooth regardless of frame/sim rate mismatch
 *   4. jce_scene_update(scene, dt)
 *   5. update audio 3D listener + spatial voice positions
 * Safe with NULL or dt <= 0. */
JCE_API void        JCE_CALL jce_runtime_step(JceRuntime *rt, float dt);

/* ── Time control (Phase 0.2) ── Unity-style global time scale + pause.
 * The simulation (physics, scene, sequencer, particles, gameplay + scripts)
 * advances by dt * time_scale each jce_runtime_step; pausing freezes it
 * (equivalent to scale 0) while audio and the editor/UI keep running on the
 * host's real dt.  Use for bullet-time / hitstop / slow-motion / pause-menu.
 * Scale is clamped to [0, 100]; default 1.0 (host seeds it from
 * JceProjectTime.time_scale).  All no-op on a NULL runtime. */
JCE_API void        JCE_CALL jce_runtime_set_time_scale(JceRuntime *rt, float scale);
JCE_API float       JCE_CALL jce_runtime_get_time_scale(const JceRuntime *rt);
JCE_API void        JCE_CALL jce_runtime_set_paused(JceRuntime *rt, bool paused);
JCE_API bool        JCE_CALL jce_runtime_is_paused(const JceRuntime *rt);

/* Global actor budget (large-world): cap the TOTAL live spawn-manager actors
 * (peds/vehicles) across all managers at `max_actors`, and the spawns committed
 * per frame at `per_frame_quota` (rate-limits pop-in bursts).  Either 0 = no
 * limit (default; behaviour identical to before).  Spawns are refused once the
 * pool/quota is hit; the manager simply retries on later frames as actors despawn. */
JCE_API void        JCE_CALL jce_runtime_set_actor_budget(JceRuntime *rt,
                                                          uint32_t max_actors,
                                                          uint32_t per_frame_quota);
/* Live actor count + configured budget (either out-param may be NULL). */
JCE_API void        JCE_CALL jce_runtime_get_actor_stats(const JceRuntime *rt,
                                                         uint32_t *out_count,
                                                         uint32_t *out_budget);

/* ── Streamed-cell gameplay wiring (streaming M3) ─────────────────────
 *
 * The world streamer spawns/destroys the SCENE entities of a chunk as the
 * camera moves, but the runtime's gameplay subsystems (physics bodies,
 * triggers, Lua scripts + on_start, behavior trees, nav agents, GAS,
 * ragdolls, …) are otherwise materialised ONLY by the one-shot scene walk at
 * create()/scene-load.  These two calls let the host wire a freshly-streamed
 * cell's entities into — and release them from — the live runtime, so a script
 * / trigger / NPC authored in a streamed cell actually comes alive (on_start /
 * on_update, trigger observer, runtime body) instead of just rendering.
 *
 * spawn: for each id, runs the SAME per-entity wiring the create() walk and
 *   jce.spawn use (body/character/audio + trigger/spawner/weapon/save-point/
 *   BT/script+on_start/GAS/ragdoll/nav).  Re-wiring an id that already has live
 *   gameplay would double it, so pass ONLY the freshly-spawned cell ids (the
 *   streamer's on_spawn roster) — base-scene entities are already wired.
 *
 * despawn: for each id, RELEASES every runtime-side handle that references the
 *   entity (destroys its physics body/collider, removes its trigger observer,
 *   releases its script instance firing on_destroy, halts+frees its BT agent,
 *   removes its nav agent, drops its GAS/ragdoll/vehicle/etc.) so the streamer
 *   can then destroy the scene entity with NO dangling runtime reference.  Call
 *   this from the streamer's on_despawn (which fires while the ids are still
 *   valid, BEFORE the scene entities are destroyed).
 *
 * Both are safe with a NULL runtime / NULL ids / count 0, and silently skip an
 * id that has no tracked gameplay state.  `ids` are JceEntity values. */
JCE_API void JCE_CALL jce_runtime_spawn_gameplay_for_ids(JceRuntime *rt,
                                                         const uint64_t *ids,
                                                         uint32_t count);
JCE_API void JCE_CALL jce_runtime_despawn_gameplay_for_ids(JceRuntime *rt,
                                                           const uint64_t *ids,
                                                           uint32_t count);

/* ── Floating-origin large-world coordinates ─────────────────────────
 *
 * Returns the runtime's mutable JceWorldOrigin so gameplay/streaming code can
 * convert between an entity's float LOCAL coordinate and its true DOUBLE
 * absolute world coordinate (jce_world_origin_to_absolute / _to_local).  The
 * runtime advances this origin only when the active scene opts in via
 * rendering_settings.floating_origin_enabled; until then origin stays at
 * (0,0,0) and local == absolute.  Returns NULL for a NULL runtime; the pointer
 * is owned by the runtime and valid for its lifetime. */
JCE_API JceWorldOrigin *JCE_CALL jce_runtime_world_origin(JceRuntime *rt);

/* ── Scene / level transition (FEATURE 9.4) ──────────────────────────
 *
 * Queue a transition to another authored scene WITHOUT tearing the runtime
 * (and its physics world / script VM / audio device / save registry) down
 * and rebuilding it.  jce_runtime_step advances an internal state machine:
 *
 *   FADE_OUT  transition_alpha ramps 0 -> 1 over a short timer
 *   LOAD      the old scene's tracked bodies / nav agents / script instances
 *             / triggers / spawners / weapons / voices are released, the new
 *             scene JSON is loaded INTO the same JceScene the runtime drives
 *             (so the caller's renderer keeps pointing at it), and the
 *             physics / scripts / terrain / gameplay spawn walks re-run so
 *             every subsystem re-inits for the new scene
 *   FADE_IN   transition_alpha ramps 1 -> 0
 *   IDLE      done
 *
 * `scene_path` is resolved host-filesystem-first then from the mounted PAK
 * (+ bundle overlays), the same host->PAK fallback scripts/terrain/audio use,
 * so it works in both the editor and a shipped single-file exe.  Returns true
 * when the request was accepted (queued); false on a NULL runtime / empty
 * path, or when a transition is already in flight (the in-flight one wins —
 * call jce_runtime_is_transitioning first to gate).
 *
 * The load is SYNCHRONOUS (it happens inside the one LOAD step); a threaded /
 * streamed background load that keeps rendering the old scene during the read
 * is a documented follow-up.  Safe to call from gameplay scripts / triggers. */
JCE_API bool  JCE_CALL jce_runtime_request_scene(JceRuntime *rt,
                                                 const char *scene_path);

/* The fade quad alpha for the current transition: 0 = fully clear (draw
 * nothing), 1 = fully black (scene hidden).  The app / editor reads this each
 * frame after jce_runtime_step and draws a screen-space quad at this opacity
 * so the level swap is hidden behind a fade.  Returns 0 when idle / NULL. */
JCE_API float JCE_CALL jce_runtime_transition_alpha(const JceRuntime *rt);

/* True while a queued scene transition is in flight (any non-idle state).
 * Goes true on the first jce_runtime_step after jce_runtime_request_scene and
 * false again once FADE_IN completes.  False for a NULL runtime. */
JCE_API bool  JCE_CALL jce_runtime_is_transitioning(const JceRuntime *rt);

/* ── Camera shake (FEATURE 6.5) ── Add trauma to the live virtual-camera
 * resolver's trauma-shake generator (jce_vcam_system).  Call on impactful
 * gameplay events (a hit, an explosion, a landing): `amount` is added to the
 * current trauma (clamped to [0,1]); intensity is trauma² so it eases out and
 * decays back to zero over the next frames.  While trauma is active, the active
 * VCam's resolved pose gets a bounded positional shake offset.  Also exposed to
 * gameplay scripts as jce.shake_camera(amount).  Safe with a NULL runtime. */
JCE_API void        JCE_CALL jce_runtime_shake_camera(JceRuntime *rt, float amount);

/* ── Destruction / fracture (opt-in) ── Shatter a fracturable entity into
 * dynamic convex-hull fragment bodies.  The entity must carry an ENABLED
 * JceFracture component (default OFF) or this is a no-op.  The body-swap (the
 * intact body is destroyed and replaced by a deterministic Voronoi box-shatter
 * of the entity's AABB; fragments inherit the parent velocity + a small outward
 * kick and get mass = density × cell-volume) is performed DEFERRED, after the
 * fixed-step loop — so this is safe to call from on_collision / a contact
 * callback / mid-step gameplay code.  Calling it again after the entity has
 * already shattered is a no-op (the component disables itself on break).  Safe
 * with a NULL runtime. */
JCE_API void        JCE_CALL jce_runtime_fracture_entity(JceRuntime *rt, uint64_t entity);

/* ── Vehicle control (opt-in) ── Script/host driver input for a raycast
 * vehicle.  `entity` must carry an ENABLED JceVehicle component (the runtime
 * stood up its chassis + wheels at spawn) or this is a no-op.  throttle in
 * [-1,1] (negative = reverse), brake in [0,1], steer in [-1,1].  In PLAYER
 * input mode the runtime also maps rt->input each tick (this call is then
 * overwritten next tick); use SCRIPT input mode for exclusive script control.
 * Safe with a NULL runtime / unknown entity. */
JCE_API void        JCE_CALL jce_runtime_vehicle_set_input(JceRuntime *rt,
                                                           uint64_t entity,
                                                           float throttle,
                                                           float brake,
                                                           float steer);

/* Forward speed (m/s, chassis local +Z) of a runtime vehicle, or 0 when the
 * entity carries no live vehicle.  Safe with a NULL runtime. */
JCE_API float       JCE_CALL jce_runtime_vehicle_get_speed(JceRuntime *rt,
                                                           uint64_t entity);

/* Hot-reload a Lua script (Phase 0 keystone follow-up): re-read the source at
 * `path`, recompile it, and rebind every live instance of that script in place
 * — per-instance `self` state is preserved and on_start is NOT re-run, so a
 * running game keeps its state while picking up edited on_update/on_collision
 * logic.  A compile error keeps the previous version.  The editor drives this
 * automatically via a file watcher; exposed for scripted live-coding too. */
JCE_API void        JCE_CALL jce_runtime_reload_script(JceRuntime *rt, const char *path);

/* Renderer interpolation factor in [0,1] left over from the last
 * jce_runtime_step: residual_accumulator / fixed_timestep.  This is the
 * same alpha the runtime uses to blend body transforms; callers that
 * interpolate their own (non-physics) render state on the same timeline
 * can read it here.  Returns 0 when rt is NULL or physics is disabled. */
JCE_API float       JCE_CALL jce_runtime_interpolation_alpha(const JceRuntime *rt);

/* Push the latest player input.  Latched until the next call.  When
 * `in->jump_pressed` is true, the flag remains set until step() consumes
 * it — callers may issue press-and-release pulses on key down without
 * worrying about exact frame alignment. */
JCE_API void        JCE_CALL jce_runtime_set_input(JceRuntime *rt,
                                                   const JceRuntimeInput *in);

/* Supply one frame of raw pointer state without disturbing movement/jump
 * input.  The complete pointer sample is cleared after the next step(). */
JCE_API void        JCE_CALL jce_runtime_set_pointer_input(
    JceRuntime *rt, float dx, float dy, float wheel, uint32_t buttons);

/* Replace the current frame's touch sample without disturbing movement or
 * pointer state.  At most JCE_RUNTIME_MAX_TOUCHES finite samples are kept in
 * stable input order; NULL/zero releases all touches. */
JCE_API void        JCE_CALL jce_runtime_set_touch_input(
    JceRuntime *rt, const JceRuntimeTouch *touches, int count);

/* Bind the live data-driven action map for THIS frame so gameplay scripts can
 * query arbitrary authored verbs/axes by name (jce.is_action_down / get_axis),
 * beyond the fixed walk/jump/sprint/attack fields of JceRuntimeInput.  The
 * runtime borrows the pointer (does not own it); pass NULL to clear.  The host
 * (editor game view / shipped drop-in main) calls this each frame after
 * jce_actions_update, before jce_runtime_step. */
JCE_API void        JCE_CALL jce_runtime_set_actions(JceRuntime *rt,
                                                     const JceInputActions *actions);

/* Read the live character controller world position.  Returns false
 * when no CharacterController exists in the scene. */
JCE_API bool        JCE_CALL jce_runtime_get_player_position(const JceRuntime *rt,
                                                             jce_vec3 *out_pos);

/* Character's horizontal forward: (sin(char_yaw),0,cos(char_yaw)) once the
 * locomotion yaw is valid, else the spawn transform's +Z projection (so it is
 * meaningful on frame 0 of Play before the first movement).  False when there
 * is no CharacterController. */
JCE_API bool        JCE_CALL jce_runtime_get_player_forward(const JceRuntime *rt,
                                                            jce_vec3 *out_fwd);

/* ── Client-side prediction (rollback/replay) ─────────────────────────
 *
 * Establish (or tear down) client-side prediction for ONE locally-owned,
 * server-authoritative entity — the predicted player.  When `entity` is a
 * valid transform-bearing entity:
 *   - a prediction ring is created (jce_net_prediction) sized to the
 *     locomotion input/state blobs,
 *   - its initial state is seeded from the entity's current transform,
 *   - the net-transform layer is told this owned object is runtime-predicted
 *     (jce_net_transform_set_predicted), so it stops applying its own snap-
 *     correction and the runtime owns the transform instead.
 * After this, each fixed tick the runtime predicts forward from the latest
 * input (AFTER the existing character driver runs — prediction does not alter
 * Bullet movement) and reconciles against authoritative snapshots as they
 * arrive (rollback + replay of buffered inputs).
 *
 * Passing entity == 0 disables prediction and destroys the ring.  Safe with a
 * NULL runtime.  With no predicted entity established (the default) every
 * prediction hook in jce_runtime_step is a provable no-op.
 *
 * NOTE: the prediction step is a PURE deterministic kinematic integrator
 * (jce_predict_locomotion); a deterministic-Bullet-resim prediction path is a
 * documented follow-up. */
JCE_API void        JCE_CALL jce_runtime_set_predicted_entity(JceRuntime *rt,
                                                              uint64_t entity);

/* The entity client-prediction is currently driving, or 0 when prediction is
 * not established.  0 for a NULL runtime. */
JCE_API uint64_t    JCE_CALL jce_runtime_predicted_entity(const JceRuntime *rt);

/* ── In-game UI button click dispatch (FEATURE 4.1) ───────────────────
 *
 * Drain one clicked UIButton into the gameplay script VM.  The shipping
 * app loop polls jce_ui_canvas_last_clicked() after rendering the canvas;
 * when it is non-zero it looks up that button's authored on_click_handler
 * and calls this seam, which invokes the named global Lua function with the
 * clicked button's entity id (handler(button_entity)) through the runtime's
 * script VM.  This is what makes authored in-game buttons actually do
 * something in a shipped build, not just tint on hover.
 *
 * Fire it exactly once on the release frame (last_clicked already fires only
 * on that frame).  Returns true iff a script VM exists and a global handler
 * of that name was found and invoked; false (clean no-op) when rt has no
 * script VM, or handler is NULL/empty, or no such handler exists. */
JCE_API bool        JCE_CALL jce_runtime_dispatch_ui_click(JceRuntime *rt,
                                                           uint64_t button_entity,
                                                           const char *handler);

/* ── UI widget value/text/submit → script dispatch ───────────────────
 *
 * The companions to jce_runtime_dispatch_ui_click for the non-button widgets.
 * The shipping app loop (and the editor Play loop) drain the canvas after
 * rendering — jce_ui_canvas_last_value_changed / _last_text_changed /
 * _last_submitted — and forward each non-zero entity to one of these seams,
 * which resolve the widget on the runtime's own scene (jce_runtime_scene),
 * read its authored handler + current value, and fire the named global Lua
 * function through the script VM:
 *
 *   value_changed → UISlider.value | UIToggle.is_on(0/1) | UIDropdown.selected_index
 *                   → on_value_changed(entity, value)        [number]
 *   text_changed  → UIInputField.text → on_value_changed(entity, text)  [string]
 *   submit        → UIInputField.text → on_submit(entity, text)         [string]
 *
 * Pass ONLY the drained entity id; the runtime does the component lookup.
 * Returns true iff a script VM exists and a global handler of the widget's
 * authored name was found and invoked; false (clean no-op) otherwise — an
 * empty handler / non-widget entity / no VM are all clean no-ops. */
JCE_API bool        JCE_CALL jce_runtime_dispatch_ui_value_changed(JceRuntime *rt,
                                                                   uint64_t entity);
JCE_API bool        JCE_CALL jce_runtime_dispatch_ui_text_changed(JceRuntime *rt,
                                                                  uint64_t entity);
JCE_API bool        JCE_CALL jce_runtime_dispatch_ui_submit(JceRuntime *rt,
                                                            uint64_t entity);

/* ── Animation frame-event → script dispatch (P1 anim-events) ─────────
 *
 * Route a fired animation frame event to `entity`'s live gameplay script.
 * The scene renderer advances each skeletal animator's per-clip event track
 * every frame and (when its anim-event hook is set — the editor Play loop and
 * the shipped default_main point it at this seam via a small trampoline) calls
 * this for every event in the (prev,cur] clip-time window.  The runtime
 * resolves `entity` to its live script instance (the same scan rt_script_send_
 * message uses) and calls `on_anim_event(id, name, f0, f1, i0)` on it
 * (jce_script_call_anim_event), so an authored footstep / hitbox-on / etc.
 * event reaches gameplay code.
 *
 * Clean no-op when rt has no script VM, `ev` is NULL, or the entity has no
 * script / no on_anim_event handler (most scripts won't define it).  `ev` is
 * borrowed for the duration of the call.  Safe with a NULL runtime. */
JCE_API void        JCE_CALL jce_runtime_dispatch_anim_event(JceRuntime *rt,
                                                            uint64_t entity,
                                                            const JceAnimEvent *ev);

/* ── Animation state-change → script dispatch (state-enter/exit) ──────
 *
 * Route an animation state-machine state change to `entity`'s live gameplay
 * script.  The scene renderer drives each skeletal animator's bound SM every
 * frame and (when its anim-state hook is set — the editor Play loop and the
 * shipped default_main point it at this seam via a small trampoline) calls
 * this once whenever the SM's active state changes.  The runtime resolves
 * `entity` to its live script instance (the same scan rt_script_send_message
 * uses) and calls, via the generic jce_script_call_message primitive:
 *   - `on_state_exit(self, 0, from_state)`  — only when `from_state` is a
 *     non-empty name (skipped on the initial enter, which has no prior state),
 *   - `on_state_enter(self, 0, to_state)`   — always (when an instance exists).
 *
 * Clean no-op when rt has no script VM, the entity has no script, or the
 * script defines neither handler (jce_script_call_message tolerates a missing
 * method).  `from_state` / `to_state` are borrowed for the call (NULL treated
 * as empty).  Safe with a NULL runtime. */
JCE_API void        JCE_CALL jce_runtime_dispatch_anim_state(JceRuntime *rt,
                                                            uint64_t entity,
                                                            const char *from_state,
                                                            const char *to_state);

/* Ground raycast against the runtime's physics world — the hook the scene
 * renderer installs for Foot IK (jce_scene_renderer_set_ground_query_fn).
 * Casts `dir` from `origin` for `max_dist`; on a hit writes the hit world-Y
 * into *out_hit_y and the surface normal into out_normal[3] (out_normal may be
 * NULL) and returns true; false on miss / NULL runtime / no physics world. */
JCE_API bool        JCE_CALL jce_runtime_ground_raycast(JceRuntime *rt,
                                                        const float origin[3],
                                                        const float dir[3],
                                                        float max_dist,
                                                        float *out_hit_y,
                                                        float out_normal[3]);

/* Pause / resume scene audio voices (mirrors editor Play→Pause). */
JCE_API void        JCE_CALL jce_runtime_pause_audio (JceRuntime *rt);
JCE_API void        JCE_CALL jce_runtime_resume_audio(JceRuntime *rt);

/* Subscribe to contact / trigger events (BEGIN/STAY/END).  The callback
 * receives JceContactEvent with entity_a / entity_b already resolved to the
 * scene entities (bodies are tagged at spawn).  Pass fn=NULL to unsubscribe.
 * Registering the first listener activates per-step manifold diffing; until
 * then it costs nothing. */
JCE_API void        JCE_CALL jce_runtime_set_contact_listener(
                                JceRuntime *rt,
                                jce_contact_listener_fn fn,
                                void *userdata);

/* Sub-system accessors so projects can layer custom systems (raycasts,
 * ad-hoc voices) on top of the runtime.  NULL when the matching feature
 * wasn't enabled. */
JCE_API JcePhysicsWorld *JCE_CALL jce_runtime_physics(const JceRuntime *rt);
JCE_API JceAudio        *JCE_CALL jce_runtime_audio  (const JceRuntime *rt);
JCE_API JceScene        *JCE_CALL jce_runtime_scene  (const JceRuntime *rt);

/* The snapshot registry the runtime stands up at create() with the standard
 * providers (scene/ECS) already registered.  Games can register additional
 * sections and drive jce_snapshot_save_to_file / jce_snapshot_load_from_file
 * against it directly (e.g. menu-driven save slots).  NULL when rt is NULL. */
JCE_API JceSnapshotRegistry *JCE_CALL jce_runtime_save_registry(
                                const JceRuntime *rt);

/* Save the current play session to `path` (a .jsnp file) through the runtime
 * registry, creating parent directories as needed.  Returns false on failure
 * or when rt/path is NULL.  Equivalent to calling jce_snapshot_save_to_file on
 * jce_runtime_save_registry(rt) plus a mkdir -p of the parent directory. */
JCE_API bool JCE_CALL jce_runtime_save_to_file(JceRuntime *rt, const char *path);

/* Switch the active game locale at runtime (reloads the string table and
 * fires jce_loc listeners; authored UIText keys re-resolve on the next
 * frame).  Pass-through to jce_loc_set_locale — safe regardless of whether
 * the runtime initialised localization.  No-op on NULL/empty input. */
JCE_API void JCE_CALL jce_runtime_set_locale(JceRuntime *rt, const char *locale);

/* ── Behavior trees + perception (P2-perception-bt-binding) ───────────
 *
 * The runtime owns one JceBtContext shared by every agent that authored a
 * JceBehaviorTree (its tree_path is loaded at create() and ticked every
 * gameplay frame).  A small set of perception-reading actions is bundled
 * (IsTargetVisible / HasTarget / HasHeardSound / IsTargetInRange).  Games
 * register their OWN action nodes (MoveTo / Attack / …) on this context
 * BEFORE the first step() so authored trees can call them; those actions
 * read the agent's blackboard via jce_runtime_bt_blackboard. */
JCE_API JceBtContext *JCE_CALL jce_runtime_bt_context(const JceRuntime *rt);

/* Number of agents that successfully loaded a behavior tree. */
JCE_API int JCE_CALL jce_runtime_bt_count(const JceRuntime *rt);

/* The perception/working blackboard for the agent whose entity id matches
 * `entity` (the same id the authored component lives on).  Perception writes
 * target.visible / target.position / target.distance / sound.* into it each
 * frame; game BT actions read+write it.  NULL when the entity has no loaded
 * tree.  Valid only for the runtime's lifetime. */
JCE_API JceBlackboard *JCE_CALL jce_runtime_bt_blackboard(const JceRuntime *rt,
                                                          uint64_t entity);

/* The loaded behavior-tree handle index (JceBtTreeHandle.idx inside the
 * runtime's jce_runtime_bt_context) for the agent whose entity id matches
 * `entity`.  Out-param keeps this header free of jce_bt.h; rebuild the
 * handle with (JceBtTreeHandle){ *out_tree_idx }.  Returns false when the
 * entity has no successfully loaded tree.  Read-only tooling (the editor
 * BT visualizer) polls node state through this. */
JCE_API bool JCE_CALL jce_runtime_bt_tree(const JceRuntime *rt, uint64_t entity,
                                          uint32_t *out_tree_idx);

/* ── Gameplay Ability System (GAS consumption last-mile) ──────────────
 *
 * The live JceGameplayAbilitySystem the runtime built from the entity's
 * authored JceGameplayAbilitySystemComponent at create() (rt_spawn_gameplay),
 * ticked every gameplay frame (jce_gas_tick).  Games / native code act on it
 * directly (jce_gas_activate_ability / jce_gas_attribute_current / …); the Lua
 * bindings (jce.gas_activate / jce.gas_get / jce.gas_apply) resolve through
 * this same pointer.  NULL when the entity authored no ability system.  Valid
 * for the runtime's lifetime (the pointer is stable until a scene transition
 * rebuilds the live systems). */
JCE_API JceGameplayAbilitySystem *JCE_CALL jce_runtime_entity_gas(JceRuntime *rt,
                                                                  uint64_t entity);

/* ── Ragdoll (skeleton-driven physics, scene-pass last-mile) ──────────
 *
 * The live JceRagdoll the runtime built from the entity's authored JceRagdoll
 * component at create() (rt_spawn_gameplay), driven each physics step and
 * published into the scene pose relay.  Games / native code (e.g. a death
 * script) act on it directly — most usefully jce_ragdoll_set_blend_weight to
 * collapse (0) or restore (1) the ragdoll at runtime; the component's
 * blend_weight is reconciled into it each tick, so prefer editing the component
 * for persistence.  Returns NULL when the entity authored no (enabled) ragdoll.
 * Valid for the runtime's lifetime until a scene transition rebuilds the live
 * ragdolls.  Acting on the handle requires the internal jce_ragdoll.h. */
JCE_API JceRagdoll *JCE_CALL jce_runtime_entity_ragdoll(JceRuntime *rt,
                                                        uint64_t entity);

JCE_EXTERN_C_END
#endif /* JCE_APPLICATION_RUNTIME_H */
