/* jce_rt_internal.h  Internal shared state for the runtime modules.
 *
 * Holds the JceRuntime struct + the runtime Entry/helper types/enums and
 * the engine includes they need, so the (formerly monolithic) runtime can be
 * split across jce_rt_*.c translation units that all share ONE definition of
 * the runtime state.  Internal to the runtime implementation — NOT a public
 * header and never installed.  (Extracted from jce_runtime.c.)
 */
#ifndef JCE_RT_INTERNAL_H
#define JCE_RT_INTERNAL_H

#include <jce/application/jce_runtime.h>

#include <jce/middleware/audio/jce_audio.h>
#include <jce/middleware/audio/jce_audio_occlusion.h>
#include <jce/middleware/audio/jce_audio_mixer.h>
#include <jce/middleware/audio/jce_audio_mixer_config.h>
#include <jce/middleware/audio/jce_music.h>   /* adaptive/interactive music director */
#include <jce/middleware/audio/jce_reverb_zones.h>
#include <jce/middleware/physics/jce_physics.h>
#include <jce/middleware/physics/jce_physics2d.h>
#include <jce/middleware/physics/jce_physics_types.h>
#include <jce/middleware/physics/jce_physics_layers.h>
#include <jce/middleware/physics/jce_physics_material.h>
#include <jce/middleware/physics/jce_physics_debug.h>
#include <jce/middleware/physics/jce_collider_cook.h>
#include <jce/middleware/physics/jce_collider_asset.h>
#include <jce/middleware/physics/jce_fracture.h>   /* Voronoi shatter geometry core */
#include <jce/middleware/physics/jce_softbody.h>    /* volumetric / pressure soft body */
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_lod.h>     /* distance-tier hysteresis (sim-LOD) */
#include <jce/middleware/scene/jce_water.h>   /* buoyancy force + surface sampling */
#include <jce/middleware/scene/jce_component_registry.h>  /* comp-id enable gate */
#include <jce/middleware/scene/jce_terrain.h>
#include <jce/middleware/scene/jce_scene_sequencer.h>
#include <jce/middleware/scene/jce_tilemap.h>
#include <jce/middleware/scene/jce_vcam_system.h>  /* camera trauma-shake seam */
#include <jce/middleware/scene/jce_world_origin.h> /* floating-origin rebase core */
#include <jce/middleware/world/jce_trigger_volume.h>
#include <jce/middleware/world/jce_spawn_manager.h>
#include <jce/middleware/world/jce_weapon.h>
#include <jce/middleware/world/jce_gas.h>   /* live per-entity ability system */
#include <jce/middleware/world/jce_gas_replication.h> /* server-auth GAS attribute replication */
#include <jce/middleware/ai/jce_bt.h>
#include <jce/middleware/ai/jce_perception.h>
#include <jce/middleware/script/jce_script.h>
#include <jce/middleware/script/jce_script_vm.h> /* per-script language selection */
#include <jce/os/platform/jce_file_watcher.h>   /* script hot-reload (editor) */
#include <jce/middleware/ai/jce_nav_agent.h>
#include <jce/middleware/ai/jce_navmesh_recast.h>
#include <jce/middleware/save/jce_snapshot.h>
#include <jce/middleware/save/jce_save_providers.h>
#include <jce/middleware/save/jce_save_migration.h>
#include <jce/middleware/net/jce_session.h>
#include <jce/middleware/net/jce_replication.h>
#include <jce/middleware/net/jce_network_variable.h>
#include <jce/middleware/net/jce_net_transform.h>
#include <jce/middleware/net/jce_net_prediction.h>      /* client prediction core */
#include <jce/middleware/net/jce_predict_locomotion.h>  /* deterministic step fn  */
#include <jce/middleware/net/jce_rpc.h>                 /* upstream input cmd RPC  */
#include <jce/middleware/net/jce_net_input_command.h>   /* client->server input    */
#include <jce/middleware/ui/jce_localization.h>
#include <jce/os/platform/jce_host_locale.h>
#include <jce/resource/jce_model_importer.h>
#include <jce/renderer/jce_model.h>   /* ragdoll: load skeleton-bearing model from PAK/host FS */
#include <jce/middleware/animation/jce_skeleton.h>   /* ragdoll: rest pose + joint count */
#include "middleware/animation/jce_ragdoll.h"   /* INTERNAL src header (jce_animation); see test_jce_ragdoll.c */
#include <jce/resource/jce_pak_loader.h>   /* PAK-resident script source (shipped) */
#include <jce/resource/jce_scene_serial.h> /* additive load for jce.spawn */
#include <jce/middleware/scene/jce_scene_components_json.h> /* serial base-dir for relative material backfill */
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_console.h>   /* real engine cvars driving the sim (gap 9.1) */
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_fixed_clock.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv (sim-LOD KPI toggle) */
#include <string.h>
#include <math.h>
#include <float.h>
#include <ctype.h>

#define LOG_TAG "runtime"

/* ── Internal types ──────────────────────────────────────────────── */

typedef struct {
	JceEntity     entity;
	JceBodyHandle body;
	/* Runtime TRS sync: last pose written to the scene (to detect external
	 * edits) + the entity's scale at spawn (scale changes apply relative). */
	jce_vec3      last_pos;
	jce_quat      last_rot;
	jce_vec3      last_scale;
	jce_vec3      spawn_scale;
	/* Fixed-tick history for render interpolation: pose at the end of the
	 * previous fixed tick (prev_*) and at the end of the current one
	 * (cur_*).  rt_sync_transforms blends prev->cur by the accumulator
	 * alpha so the scene transform stays smooth between fixed ticks. */
	jce_vec3      prev_pos;
	jce_quat      prev_rot;
	jce_vec3      cur_pos;
	jce_quat      cur_rot;
	/* Collider center offset (LOCAL, scaled, pre-rotation) baked into the body
	 * origin at spawn (bd.position = entity + rotate(center_local)).  The body
	 * pose tracks the collider CENTER; the entity Transform is the ORIGIN.  Push
	 * adds rotate(center_local); sync subtracts it — so a moved entity keeps its
	 * collider centered instead of collapsing the offset onto the origin. */
	jce_vec3      center_local;
	uint8_t       kind;        /* JceBodyType: static bodies skip write-back. */
} BodyEntry;

/* ── Physics draw-distance deferred static collider (big-world spawn) ────
 *
 * A full-load mega-scene (e.g. ~5000 static building boxes) spawning every
 * collider at Play start costs a ~3s freeze + a body-count cliff.  Small
 * STATIC box/sphere/capsule colliders that are FAR from the player are not
 * created up front; instead they are recorded here and spawned lazily once
 * the player walks within PHYS_DD_RADIUS, then destroyed again when the
 * player leaves (PHYS_DD_RADIUS + PHYS_DD_HYST).  Large colliders (the
 * ground / big mesh boxes) and all dynamic/kinematic bodies are NOT deferred
 * — they spawn immediately at scene load as before.  `center` is the
 * collider world center computed at classification time (XZ distance is
 * tested against it); `spawned` tracks whether the body currently exists. */
typedef struct {
	JceEntity entity;
	jce_vec3  center;     /* collider world center (== bd.position at spawn) */
	bool      spawned;    /* a body for this entry currently exists */
} DDEntry;

/* Defer a static box/sphere/capsule only when its largest world half-extent
 * is below this (metres): catches city buildings (small boxes) but never the
 * ground / large mesh colliders. */
#define PHYS_DD_SMALL  60.0f
/* Spawn radius (XZ, metres) and hysteresis pad so a body straddling the ring
 * does not thrash spawn/despawn every frame. */
#define PHYS_DD_RADIUS 180.0f
#define PHYS_DD_HYST   40.0f

typedef struct {
	JceEntity entity;
	JceSound  sound;
	JceVoice  voice;
	bool      spatial;
	float     base_volume;   /* authored volume; occlusion scales it each frame */
	char      bus[32];       /* mixer bus name this voice is routed to */
} VoiceEntry;

/* 2D rigid body spawned from a RigidBody2D (+ optional Collider2D) component.
 * Simulated in the XY plane by the Box2D world (rt->physics2d); the body
 * position/angle is written back to the entity Transform each frame (x,y in
 * the XY plane, angle -> Z-rotation quaternion). */
typedef struct {
	JceEntity     entity;
	JceBodyHandle body;
	uint8_t       kind;        /* JceBodyType: static bodies skip write-back. */
} Body2DEntry;

/* ── Gameplay-bridge entries (P0-master-bridge) ──────────────────────
 *
 * Each authored gameplay POD component is mirrored into the matching
 * runtime subsystem at create() time and ticked from jce_runtime_step's
 * variable-update region.  The scene component remains the source of
 * truth for authored fields; the entry caches the engine-side handle. */

typedef struct {
	JceEntity        entity;
	JceTriggerHandle handle;   /* trigger registered in rt->trigger_world */
} TriggerEntry;

typedef struct {
	JceEntity        entity;
	JceSpawnManager *mgr;        /* owned */
} SpawnEntry;

typedef struct {
	JceEntity          entity;
	JceWeaponArchetype arch;    /* derived from the authored component */
	JceWeaponInstance  inst;    /* live fire-control state */
	uint64_t           rng;     /* per-weapon spread RNG */
} WeaponEntry;

/* Authored NavAgent mirrored into the runtime agent set (rt->nav_agents).
 * has_dest/last_goal_* cache the destination last issued so auto_repath can
 * re-issue set_destination only when the goal actually moves. */
typedef struct {
	JceEntity         entity;
	JceNavAgentHandle handle;
	bool              has_dest;
	float             last_goal_x, last_goal_z;
	/* Simulation-LOD cadence (consulted only when the entity carries a
	 * JceSimLodComponent gating NavAgent): accumulate dt and advance steering
	 * by the folded accumulated dt at the tier's rate. */
	float             simlod_accum;
	int               simlod_prev_tier;
} NavAgentEntry;

/* Authored SavePoint mirrored as a sphere trigger in rt->trigger_world.
 * When the player observer enters the volume the runtime writes a snapshot
 * to "<saves_dir>/<save_id>.jsnp" (P2-save-snapshot).  one_shot points fire
 * at most once per session. */
typedef struct {
	JceEntity        entity;
	JceTriggerHandle handle;        /* trigger registered in rt->trigger_world */
	char             save_id[64];   /* file basename for the snapshot */
	bool             one_shot;
	bool             require_interact;
	bool             fired;         /* one_shot guard */
} SavePointEntry;

/* Authored JceBehaviorTree mirrored into the runtime BT context
 * (P2-perception-bt-binding).  tree is the handle loaded from the
 * component's tree_path; bb is this agent's perception/working blackboard;
 * sight and hearing ranges come from the perception defaults (no authored
 * cone fields yet — see rt_spawn_gameplay).  tick_period/tick_accum drive
 * the per-agent tick cadence (period 0 = tick every gameplay frame). */
struct BtEntry {
	JceEntity        entity;
	JceBtTreeHandle  tree;
	JceBlackboard   *bb;            /* owned */
	float            sight_range;
	float            sight_half_angle;
	float            hearing_range;
	float            tick_period;   /* seconds between ticks (0 = every frame) */
	float            tick_accum;
	bool             active;
	/* Simulation-LOD cadence (consulted only when the entity carries a
	 * JceSimLodComponent gating BehaviorTree): a separate accumulator + tier so
	 * the tier rate can REPLACE the authored tick_hz cadence when present. */
	float            simlod_accum;
	int              simlod_prev_tier;
};

/* A script instance AND THE VM THAT ISSUED IT, as one value.
 *
 * A JceScriptInstance is a bare uint32 whose meaning is PRIVATE to the VM
 * that issued it — Lua's is a luaL_ref into that lua_State's registry,
 * Python's is an index into that shim's own array — and both number from 1,
 * so a Lua instance and a Python instance routinely have the SAME id.
 * Handing one VM's instance to another is therefore not an error return, it
 * is a dereference of whatever that id means over there.
 *
 * With one language per process the pairing was implicit: there was exactly
 * one `rt->script_vm` and it was right by construction.  Per-script selection
 * removes that, so the pairing is made STRUCTURAL instead of remembered:
 *
 *   - `rt->script_vm` no longer exists.  There is no "the runtime's VM" to
 *     pass, so the ~50 dispatch sites cannot reach for one — the ones that
 *     tried stopped compiling, which is how they were all found.
 *   - No runtime function takes a JceScript* and a JceScriptInstance as two
 *     parameters.  Every per-instance dispatch takes an RtScriptRef, and the
 *     only thing that BUILDS one is rt_script_instantiate(), which fills both
 *     halves from the same jce_script_instantiate call.
 *
 * So a mismatched pair has no expression in the runtime: constructing one
 * means writing both fields by hand from two different sources, and there is
 * exactly one place that writes them at all.
 * *Enforced by:* tests/application/test_jce_runtime_multilang.c ::
 * test_two_languages_in_one_scene_both_run — the second VM refuses and counts
 * any instance id it did not issue, and both VMs there issue id 1. */
typedef struct RtScriptRef {
	JceScript        *vm;    /* the handle that issued `inst`; NULL == none */
	JceScriptInstance inst;  /* meaningless without `vm`; 0 == invalid      */
} RtScriptRef;

/* Per-instance dispatch.  These exist so that no call site anywhere in the
 * runtime names a VM handle and an instance separately; they are the only
 * form in which the pair is consumed.  Every one tolerates a NULL/zero ref
 * exactly the way the public forwarder already tolerates a NULL handle. */
static inline void rt_script_ref_start(RtScriptRef r)
{ jce_script_call_start(r.vm, r.inst); }

static inline void rt_script_ref_update(RtScriptRef r, float dt)
{ jce_script_call_update(r.vm, r.inst, dt); }

static inline void rt_script_ref_release(RtScriptRef r)
{ jce_script_release(r.vm, r.inst); }

static inline void rt_script_ref_collision(RtScriptRef r, JceScriptEntity other)
{ jce_script_call_collision(r.vm, r.inst, other); }

static inline void rt_script_ref_message(RtScriptRef r, const char *msg,
                                         double number_arg, const char *str_arg)
{ jce_script_call_message(r.vm, r.inst, msg, number_arg, str_arg); }

static inline void rt_script_ref_anim_event(RtScriptRef r, uint32_t id,
                                            const char *name, float f0,
                                            float f1, int i0)
{ jce_script_call_anim_event(r.vm, r.inst, id, name, f0, f1, i0); }

static inline void rt_script_ref_rebind(RtScriptRef r, JceScriptModule mod)
{ jce_script_rebind_instance(r.vm, r.inst, mod); }

/* One gameplay-script instance bound to an entity (Phase 0 scripting
 * keystone).  Mirrors BtEntry: rt_spawn_gameplay loads the entity's
 * JceScriptComponent into the VM for that script's LANGUAGE and calls
 * on_start; rt_tick_gameplay calls on_update each frame. */
struct ScriptEntry {
	JceEntity         entity;
	RtScriptRef       ref;
	bool              active;
	char              script_path[256];  /* for hot-reload path matching */
	/* Simulation-LOD cadence (only consulted when the entity carries a
	 * JceSimLodComponent gating Script): accumulate real dt, fire on_update
	 * only once the active tier's period elapses, folding the accumulated dt
	 * so the script sees time-correct deltas at any rate.  prev_tier is fed
	 * back to jce_lod_pick for boundary hysteresis. */
	float             simlod_accum;
	int               simlod_prev_tier;
};

/* One live Gameplay Ability System bound to an entity (GAS consumption
 * last-mile).  rt_spawn_gameplay inits `gas` from the entity's authored
 * JceGameplayAbilitySystemComponent (attribute + ability tables);
 * rt_tick_gameplay calls jce_gas_tick(&gas, sim_dt) each frame; scripts /
 * game act on it through jce_runtime_entity_gas.  The component stays the
 * authored source of truth; this holds the transient live state. */
struct GasEntry {
	JceEntity                entity;
	JceGameplayAbilitySystem gas;
};

/* One live ragdoll bound to an entity (ragdoll scene-pass last-mile).
 * rt_spawn_gameplay loads the entity's SkeletalAnimator skeleton (via an owned
 * JceModel so the borrowed JceSkeleton outlives the ragdoll) and builds `rd`
 * in rt->physics from the entity's authored JceRagdollComponent.  Each fixed
 * step the runtime drives the bodies toward the source pose (sync_from, before
 * jce_physics_step) and publishes the resolved per-bone LOCAL pose back into
 * the scene relay (sync_to, after the step).  blend_weight mirrors the live
 * component value so a death trigger can collapse the ragdoll at runtime.
 *
 * OWNERSHIP / TEARDOWN: `rd` holds bodies in rt->physics; it MUST be destroyed
 * BEFORE jce_physics_destroy (rt_teardown_scene_state) or those body handles
 * are double-freed.  `model` is owned here and destroyed after `rd`. */
struct RagdollEntry {
	JceEntity   entity;
	JceModel   *model;        /* owned; keeps the borrowed JceSkeleton alive */
	JceRagdoll *rd;           /* owned; built in rt->physics                 */
	float       blend_weight; /* mirror of the live component value          */
};

/* One live raycast vehicle bound to an entity (VEHICLE last-mile).
 * rt_try_spawn_vehicle creates the chassis + wheels via the engine vehicle
 * API (jce_physics_vehicle_*) from the entity's authored JceVehicleComponent
 * and its wheel-collider child entities.  Each tick the runtime maps player
 * input (or leaves it to the script API) into jce_physics_vehicle_set_input,
 * and POST-step writes the chassis transform back to the entity and each
 * wheel's transform back to its child entity (so wheel meshes roll/steer).
 *
 * OWNERSHIP / TEARDOWN: `veh` owns a chassis body inside rt->physics; like the
 * character capsule + ragdolls it MUST be destroyed BEFORE jce_physics_destroy
 * (rt_teardown_scene_state) or the chassis handle would be double-freed.
 *
 * wheel_entities[i] is the child entity whose Transform receives wheel i's
 * pose (0 = synthesized wheel with no render target). */
typedef struct {
	JceEntity        entity;
	JceVehicleHandle veh;                              /* owned in rt->physics */
	JceEntity        wheel_entities[JCE_VEHICLE_MAX_WHEELS];
	uint32_t         wheel_count;
	int              input_mode;                       /* JCE_VEHICLE_INPUT_*  */
	int              drive_mode;                        /* JCE_VEHICLE_DRIVE_*  */
} VehicleEntry;

/* One live volumetric / pressure soft body bound to an entity (SOFT-BODY
 * last-mile).  rt_try_spawn_softbody creates the body in the SHARED secondary
 * soft world via the public soft-body API from the entity's authored
 * JceSoftBodyComponent; POST-step the runtime writes the body's centroid back
 * to the entity Transform so it visibly settles.  The handle lives in the soft
 * world (jce_cloth.cpp), NOT in rt->physics, so it is torn down via
 * jce_softbody_destroy (independent of jce_physics_destroy). */
typedef struct {
	JceEntity         entity;
	JceSoftBodyHandle handle;   /* owned in the shared soft world */
} SoftBodyEntry;

/* One live configurable joint bound to an entity (CONFIGURABLE-JOINT last-mile).
 * rt_spawn_configurable_joint builds a per-axis 6DOF constraint in rt->physics
 * via jce_physics_configurable_joint_create from the entity's authored
 * JceConfigurableJointComponent.  Each fixed tick the break monitor compares the
 * constraint's last-step applied IMPULSE against (break_force * fixed_dt) — since
 * impulse = force * dt — and destroys it on break (then drops this entry so the
 * freed handle is never re-queried).  The constraint lives in rt->physics, so it
 * is freed by jce_physics_destroy on teardown; on a clean break we destroy it
 * early via jce_physics_constraint_destroy.  Empty (no authored joint) -> the
 * monitor early-outs on cfg_joint_count, so the frame path is unchanged. */
typedef struct {
	JceEntity           entity;
	JceConstraintHandle handle;     /* owned in rt->physics */
	float               break_force;  /* N (<=0 = unbreakable) */
	float               break_torque; /* N·m (followup: separable angular impulse) */
} ConfigJointEntry;

/* One live 2D joint bound to an entity (JOINT-2D last-mile).  rt_spawn_joint2d
 * builds a Box2D distance/hinge/spring joint in rt->physics2d via
 * jce_physics2d_joint_create from the entity's authored JceJoint2DComponent
 * (body_a = the entity's own 2D body; body_b = connected_body's 2D body, or the
 * world when 0).  The joint lives in rt->physics2d (which auto-destroys all its
 * joints on jce_physics2d_destroy); we still track it so teardown can destroy it
 * explicitly BEFORE the world dies (uniform ordering with the 3D cfg joints) and
 * the next scene starts with an empty registry.  Absent / disabled component ->
 * no entry, so a scene with no 2D joints is path-unchanged. */
typedef struct {
	JceEntity           entity;
	JceConstraintHandle handle;     /* owned in rt->physics2d */
} Joint2DEntry;

/* Worker args for an async audio-source decode.  Heap-allocated and owned
 * by the worker for its full run, so it stays valid even if the pending
 * array reallocates (slot pointers must NOT be handed to the worker). */
typedef struct {
	const JcePakArchive *pak;
	char                 path[256];
	JceAudioCpu         *cpu;    /* worker writes */
} RtAudioDecodeArgs;

/* In-flight async decode of a play_on_awake audio source.  The worker
 * decodes the clip to CPU PCM; jce_runtime_step uploads + plays it (the
 * sound starts a frame or two late instead of stalling scene load). */
typedef struct {
	JceEntity          entity;
	JceAsyncTask      *task;
	RtAudioDecodeArgs *args;   /* stable heap; holds CPU result */
} RtPendingAudio;

struct JceTerrainCollisionStream;

struct JceRuntime {
	/* Paged terrain collision for a TILED/PROCEDURAL terrain, which has no
	 * monolithic height grid and therefore got no collider at all before this
	 * existed.  NULL for a monolithic terrain, which still spawns one body. */
	struct JceTerrainCollisionStream *terrain_stream;
	/* The scene's terrain grid, borrowed from its cache. Named for the
	 * collision stream that first needed it, but recorded for ANY reader
	 * that has to ask the terrain a question -- the water disturbance
	 * layer reads it for bathymetry, and it must not care whether the
	 * terrain was authored tiled. */
	struct JceTerrain                *terrain_stream_src;   /* borrowed */
	/* The disturbance grid's bed has been read from the terrain. One-shot:
	 * sampling resolution^2 terrain heights every tick would be paid every
	 * frame for an answer that almost never changes, and a caller that does
	 * move the bed has jce_water_ripple_set_depth. */
	bool                              water_bathymetry_done;
	JceScene        *scene;       /* not owned */
	JcePakArchive   *pak;         /* not owned */
	JceAudio        *audio;       /* not owned */

	uint32_t      (*audio_load_fn)(void *, JceAudio *, const char *);
	void            *user_data;
	/* Optional host asset-path resolver (editor in-tree loose assets) — maps a
	 * scene-relative model/collider path to a readable host path before a
	 * direct jce_fs load.  See JceRuntimeDesc.resolve_path_fn. */
	bool          (*resolve_path_fn)(void *, const char *, char *, int);

	JcePhysicsWorld *physics;     /* owned (NULL if !enable_physics) */
	JcePhysics2D    *physics2d;   /* owned (NULL if !enable_physics) */

	BodyEntry       *bodies;
	int              body_count;
	int              body_cap;

	/* Physics draw-distance: deferred small static colliders (see DDEntry). */
	DDEntry         *dd;
	int              dd_count;
	int              dd_cap;

	Body2DEntry     *bodies2d;
	int              body2d_count;
	int              body2d_cap;

	JceCharacterHandle character;
	JceEntity          character_entity;
	/* Character render/sync state. get_position() returns the capsule CENTER;
	 * we render the FEET (character_half_height below) and interpolate between
	 * fixed ticks (char_prev/cur_pos) exactly like dynamic bodies. char_last_*
	 * is the last pose we wrote, to detect a gizmo edit during Play. */
	float              character_half_height;
	jce_vec3           char_prev_pos;   /* capsule center, prev fixed tick */
	jce_vec3           char_cur_pos;    /* capsule center, current fixed tick */
	jce_vec3           char_last_pos;   /* last feet pos written (edit detect) */
	jce_quat           char_last_rot;
	/* Movement feel, cached from the authored CharacterController at spawn. */
	float              char_move_speed;     /* m/s */
	float              char_sprint_mult;
	float              char_turn_speed;     /* rad/s */
	/* Control state: current visual yaw (smoothed toward move direction),
	 * coyote-time / jump-buffer countdowns, and the previous jump_held for
	 * the variable-height release edge. */
	float              char_yaw;
	bool               char_yaw_valid;
	float              char_coyote_t;
	float              char_jump_buf_t;
	bool               char_jump_was_held;

	VoiceEntry      *voices;
	int              voice_count;
	int              voice_cap;

	/* Pending async audio-source decodes (play_on_awake). */
	RtPendingAudio  *pending_audio;
	int              pending_audio_count;
	int              pending_audio_cap;

	/* Smoothed per-source occlusion state.  Created lazily on first 3D
	 * audio update when both physics and spatial voices exist; keyed by
	 * voice handle so attenuation/low-pass ramp over frames instead of
	 * popping as the listener->source path is (un)blocked. */
	JceAudioOcclusionTracker *occ_tracker;

	/* ── Audio mixer buses (P1-audio-mixer-reverb) ───────────────────
	 * Pure-CPU bus tree (solo/mute/volume + aux sends + sidechain ducking +
	 * named snapshots) consumed from audio_mixer.json at Play start
	 * (jce_audio_mixer_apply_config); per-bus insert-effect chains are also
	 * attached to the device by bus name.  resolve_volume_ducked is pushed onto
	 * each bus group every frame so the editor's sliders + authored routing
	 * drive live playback.  NULL when audio is disabled. */
	JceAudioMixer   *mixer;          /* owned */

	/* ── Adaptive music director (FEATURE 5.3) ───────────────────────
	 * One JceMusicDirector built from the scene's MusicTrack component (when
	 * one is present with play_on_awake) at scene spawn, ticked each frame in
	 * the audio-update path, and torn down with the rest of the scene state.
	 * NULL when the scene authored no music track.  Driven by intensity /
	 * transition requests from gameplay or the Lua bindings. */
	JceMusicDirector *music;         /* owned */

	/* ── Reverb zones (P1-audio-mixer-reverb) ────────────────────────
	 * Built from scene AudioReverbZone components; sampled at the listener
	 * each frame and the blended preset driven into the global reverb DSP.
	 * NULL when the scene authored no reverb zones. */
	JceReverbZones  *reverb_zones;   /* owned */

	JceRuntimeInput  input;
	const JceInputActions *actions;  /* borrowed: live action map for script name queries */

	/* Fixed-timestep accumulator driving physics on a stable cadence,
	 * decoupled from the variable render dt.  Its fixed_dt is kept in sync
	 * with the engine-wide jce_fixed_clock_default() (P1-fixed-clock-unify):
	 * seeded at create() and re-adopted each jce_runtime_step so that
	 * jce_engine_set_fixed_hz() governs physics and the two clocks can't
	 * desync.  An explicit JceRuntimeDesc.fixed_timestep instead retunes the
	 * engine clock to match.  max_frame_dt clamps the per-frame catch-up to
	 * ~RT_MAX_FIXED_STEPS ticks to dodge the spiral of death. */
	JceFixedClock    clock;
	bool             have_prev;   /* prev_* seeded — gate interpolation. */

	/* ── Floating-origin large-world rebase (opt-in, default OFF) ──────
	 * Tracks the double-precision world origin.  Only consulted when the
	 * scene sets rendering_settings.floating_origin_enabled; otherwise
	 * rt_apply_floating_origin never runs and the frame path is byte-
	 * identical.  See jce_world_origin.h + rt_apply_floating_origin. */
	JceWorldOrigin   world_origin;

	/* Game-facing contact listener (BEGIN/STAY/END).  Registered lazily on
	 * first set so the manifold-diff cost stays zero until a game subscribes. */
	jce_contact_listener_fn contact_cb;
	void                   *contact_ud;
	bool                    contact_registered;
	/* Physics contact → script on_collision bridge: registered once when both
	 * physics and the script VM exist (separate slot from contact_cb above). */
	bool                    script_collision_registered;

	/* ── Gameplay subsystems (P0-master-bridge) ──────────────────────
	 * Instantiated in create() from scene POD components, ticked in
	 * step()'s variable-update region.  All NULL/empty when the scene has
	 * no matching authored components. */
	JceTriggerWorld *trigger_world;   /* owned (NULL until first trigger) */
	JceObserverHandle trigger_player; /* observer tracking the player/cam */
	bool              trigger_player_valid;

	TriggerEntry    *triggers;
	int              trigger_count;
	int              trigger_cap;

	SpawnEntry      *spawns;
	int              spawn_count;
	int              spawn_cap;
	uint64_t         spawn_cookie_seq; /* monotonic cursor (ped-sampler RNG) */
	JceEntity        cur_spawn_mgr;    /* transient: manager being updated (for spawn cb) */

	/* Global actor budget (large-world #2): caps total live spawn-manager actors
	 * (peds/vehicles) across ALL managers + the per-frame spawn rate, so a crowded
	 * world can't instantiate unbounded actors regardless of per-manager caps.
	 * budget==0 ⇒ unlimited (byte-identical to legacy); quota==0 ⇒ unlimited rate. */
	uint32_t         actor_budget;        /* max live actors (0 = unlimited)      */
	uint32_t         actor_count;         /* current live spawn-manager actors    */
	uint32_t         actor_spawn_quota;   /* max spawns per frame (0 = unlimited) */
	uint32_t         actor_spawned_frame; /* spawns committed this frame (reset)  */

	WeaponEntry     *weapons;
	int              weapon_count;
	int              weapon_cap;

	/* ── Behavior trees + perception (P2-perception-bt-binding) ───────
	 * Single runtime-owned JceBtContext shared by every agent's tree.  At
	 * create() rt_spawn_gameplay loads each authored JceBehaviorTree's
	 * tree_path into this context and records a BtEntry (handle + per-agent
	 * blackboard).  rt_tick_gameplay runs perception (sight-cone + LOS
	 * raycast through physics + hearing) into each agent's blackboard, then
	 * ticks its tree on its cadence.  The bundled blackboard-reading actions
	 * read bt_active_bb, which is set to the ticking agent's blackboard just
	 * before each jce_bt_tick.  Owned. */
	JceBtContext    *bt_ctx;
	bool             bt_actions_registered;
	const JceBlackboard *bt_active_bb;   /* current agent's BB during a tick */
	JceEntity        bt_active_entity;   /* entity of the agent being ticked  */

	struct BtEntry  *bts;
	int              bt_count;
	int              bt_cap;

	/* ── Gameplay scripting (Phase 0 keystone) ───────────────────────
	 * ONE VM PER LANGUAGE, created on demand.  rt_spawn_gameplay resolves
	 * each authored JceScriptComponent's script_path to a language
	 * (jce_script_vm_language_for_path), gets or creates that language's VM,
	 * instantiates into it and records a ScriptEntry carrying BOTH halves;
	 * rt_tick_gameplay calls on_update on every active instance through its
	 * own VM.  Scene access (move/read an entity) is provided to scripts via
	 * the JceScriptHost callbacks below, so the script layer never depends on
	 * scene/ECS.
	 *
	 * LAZY ON PURPOSE: a build that links the Python backend but whose scene
	 * authors no .py never calls Py_InitializeFromConfig.  Creating every
	 * registered language up front would make linking a backend cost a JVM or
	 * an interpreter even for a project that does not use it.
	 *
	 * `script_host` is built ONCE (it is ~74 function pointers) and handed to
	 * every language's create_sized, so all languages see the same host —
	 * which is what makes a cross-language differential compare like for
	 * like.  `script_enabled` is false only when the JCE_SCRIPT_LANGUAGE
	 * override names a language this executable cannot run; see
	 * rt_script_install_vm. */
	JceScriptHost       script_host;
	bool                script_enabled;
	struct RtScriptLang {
		JceScript *vm;                              /* owned */
		char       language[JCE_SCRIPT_VM_LANGUAGE_MAX];
	}                   script_langs[JCE_SCRIPT_VM_MAX];
	int                 script_lang_count;
	struct ScriptEntry *scripts;
	int                 script_count;
	int                 script_cap;

	/* ── Gameplay Ability Systems (GAS consumption last-mile) ─────────
	 * One live JceGameplayAbilitySystem per entity that authored a
	 * JceGameplayAbilitySystemComponent.  Stood up in rt_spawn_gameplay
	 * from the authored attribute/ability tables, ticked in
	 * rt_tick_gameplay, and reachable from scripts/game via
	 * jce_runtime_entity_gas.  Empty (no authored component) -> a cheap
	 * no-op, byte-identical to before. */
	struct GasEntry    *gas_entries;
	int                 gas_count;
	int                 gas_cap;

	/* ── Ragdolls (skeleton-driven physics, scene-pass last-mile) ─────
	 * One live JceRagdoll per entity that authored an (enabled) JceRagdoll
	 * component and carries a skeletal animator.  Stood up in
	 * rt_spawn_gameplay, driven pre-step (rt_ragdoll_sync_from) and published
	 * post-step (rt_ragdoll_sync_to) into the scene relay.  Empty (no authored
	 * ragdoll) -> every hot-loop hook is gated on ragdoll_count, so the frame
	 * path is byte-identical to before. */
	struct RagdollEntry *ragdoll_entries;
	int                  ragdoll_count;
	int                  ragdoll_cap;

	/* Deferred prefab spawns (jce.spawn): rt_script_spawn instantiates the
	 * prefab synchronously (returns the root entity) but queues the new
	 * entities' physics + gameplay wiring here, flushed after rt_tick_gameplay
	 * so rt_spawn_gameplay's append to scripts[] can't realloc mid-iteration. */
	JceEntity          *pending_spawns;
	int                 pending_spawn_count;
	int                 pending_spawn_cap;
	bool                in_scene_walk;   /* true during the create() spawn walks */

	/* ── Deferred fracture body-swaps (DESTRUCTION/FRACTURE) ──────────
	 * jce_runtime_fracture_entity (called by gameplay or by the contact
	 * break-trigger) QUEUES the entity here rather than swapping bodies
	 * immediately: the body destroy + fragment spawn must happen POST-step,
	 * never inside a contact callback / mid-solve (Bullet would corrupt).
	 * Flushed once per frame after the fixed-step loop (rt_flush_pending_
	 * fractures), mirroring the jce.spawn / ragdoll deferred pattern.  Empty
	 * (no fracturable entity ever broke) -> the flush is a no-op and the
	 * frame is byte-identical. */
	JceEntity          *pending_fractures;
	int                 pending_fracture_count;
	int                 pending_fracture_cap;

	/* ── Raycast vehicles (VEHICLE last-mile) ─────────────────────────
	 * One live JceVehicle per entity that authored an (enabled) JceVehicle
	 * component.  Stood up in rt_try_spawn_vehicle (from the spawn walk),
	 * driven per fixed tick (rt_drive_vehicles maps player input) and
	 * published POST-step (rt_sync_vehicles writes chassis + wheel poses).
	 * Empty (no authored vehicle) -> every hook is gated on vehicle_count, so
	 * the frame path is byte-identical to before. */
	VehicleEntry       *vehicles;
	int                 vehicle_count;
	int                 vehicle_cap;

	/* ── Volumetric / pressure soft bodies (SOFT-BODY last-mile) ───────
	 * One live soft body per entity that authored an (enabled) JceSoftBody
	 * component.  Stood up in rt_try_spawn_softbody (from the spawn walk) and
	 * published POST-step (centroid -> entity Transform).  soft_statics_mirrored
	 * gates the one-time copy of the scene's static box colliders into the soft
	 * world (so the bodies rest on the ground).  Empty (no authored soft body)
	 * -> every hook is gated on softbody_count, so the frame path is unchanged. */
	SoftBodyEntry      *softbodies;
	int                 softbody_count;
	int                 softbody_cap;
	bool                soft_statics_mirrored;

	/* ── Configurable joints (CONFIGURABLE-JOINT last-mile) ────────────
	 * One live per-axis 6DOF joint per entity that authored an (enabled)
	 * JceConfigurableJointComponent.  Stood up in the post-spawn joint pass
	 * (rt_spawn_configurable_joint) and monitored for break each fixed tick.
	 * The constraints live in rt->physics; this list is only for the break
	 * monitor + early-destroy.  Empty -> the monitor is a gated no-op. */
	ConfigJointEntry   *cfg_joints;
	int                 cfg_joint_count;
	int                 cfg_joint_cap;

	/* ── 2D joints (JOINT-2D last-mile) ────────────────────────────────
	 * One live Box2D joint per entity that authored an (enabled)
	 * JceJoint2DComponent.  Stood up in the post-spawn joint pass
	 * (rt_spawn_joint2d) once every 2D body exists.  The joints live in
	 * rt->physics2d; this list is only so teardown can destroy them before
	 * the 2D world dies.  Empty -> no 2D joint work happens at all. */
	Joint2DEntry       *joints2d;
	int                 joint2d_count;
	int                 joint2d_cap;

	/* Lua script hot-reload (editor dev): poll each authored script's host file
	 * for mtime changes and recompile+rebind live instances in place (self
	 * state preserved).  Inert in shipped builds (PAK scripts have no host file
	 * → jce_file_watcher_add fails silently). */
	JceFileWatcher     *script_watcher;
	int                 script_reload_frame;

	/* ── Time control (Phase 0.2) ── Unity-style global time scale + pause.
	 * The sim (physics / scene / sequencer / particles / gameplay+scripts)
	 * advances by dt*time_scale, or freezes when paused (== scale 0). Audio
	 * and the editor/UI are driven by the host on the real (unscaled) dt.
	 * Defaults to 1.0; the host seeds it from JceProjectTime.time_scale and
	 * sets it at runtime for hitstop / bullet-time / pause-menu. */
	float               time_scale;
	bool                paused;

	/* ── Console cvar bridge (gap 9.1 last-mile) ─────────────────────
	 * Real engine cvars (registered in the process-global jce_console at
	 * create()) that DRIVE runtime state, so the in-game console / config
	 * file / remote tool can actually control the sim — not just store a
	 * value.  rt_apply_cvars (top of jce_runtime_step) reads each cvar and,
	 * when its value CHANGED since the last frame it applied, writes it to the
	 * matching runtime setter (one-directional: cvar -> runtime).  The
	 * change-detection makes the sync deterministic AND keeps the feature
	 * purely additive: an untouched cvar is never re-applied, so a runtime
	 * whose cvars nobody mutates behaves byte-identically to before this
	 * bridge existed (code/scripts that set time_scale/paused/volume directly
	 * are not clobbered by an unchanged cvar).  The cvars are seeded from this
	 * runtime's current state at create() so create() stays authoritative for
	 * the initial value (it seeds time_scale from JceProjectTime).  Pointers
	 * (not names) are cached so the per-step apply does no string lookup; they
	 * stay valid for process life (the cvar table never moves a cvar). */
	JceCvar            *cv_time_scale;     /* float, mirrors rt->time_scale   */
	JceCvar            *cv_paused;         /* bool,  mirrors rt->paused        */
	JceCvar            *cv_master_volume;  /* float, drives audio master gain  */
	float               cv_last_time_scale;    /* last value applied from cvar */
	bool                cv_last_paused;
	float               cv_last_master_volume;

	/* ── Water buoyancy (gap 2.3, slice 3) ───────────────────────────
	 * Phase clock for Gerstner height sampling in the buoyancy pass, advanced
	 * by fixed_dt per executed fixed tick (NOT the renderer's separate visual
	 * water_time) so the surface a body floats on is deterministic and tied to
	 * the fixed cadence + time_scale.  Only entities with a JceBuoyancyComponent
	 * are affected; bodies without one are provably untouched (zero regression). */
	double              buoyancy_time;

	/* ── Save / snapshot (P2-save-snapshot) ──────────────────────────
	 * A snapshot registry stood up at create() with the scene/ECS provider
	 * registered, so a play session can be persisted + restored.  Authored
	 * SavePoint components are mirrored as sphere triggers in trigger_world;
	 * a player overlap writes "<saves_dir>/<save_id>.jsnp".  saves_dir is
	 * empty when no save directory was supplied (auto-save disabled, but the
	 * registry is still usable via jce_runtime_save_registry). */
	JceSnapshotRegistry *save_registry;   /* owned */
	JceSaveMigrationRegistry *save_migrations; /* owned; upgrades older saves */
	char                 saves_dir[512];

	SavePointEntry  *save_points;
	int              save_point_count;
	int              save_point_cap;

	/* ── Navigation (P1-navmesh-chain) ───────────────────────────────
	 * Loaded from JceRuntimeDesc.navmesh_path (a .navmesh.bin baked by
	 * the editor).  nav_recast owns the Detour navmesh; nav_agents binds
	 * to it via jce_recast_path_fn and is ticked each gameplay frame.
	 * Both NULL when no navmesh path was supplied or it failed to load.
	 * The agent set is populated from authored NavAgent scene components
	 * when present; with none it is an empty (no-op) set whose path query
	 * is still proven by the load+find_path self-test logged at create. */
	JceRecastNavMesh *nav_recast;     /* owned */
	JceNavAgentSet   *nav_agents;     /* owned */

	NavAgentEntry    *nav_entries;
	int               nav_entry_count;
	int               nav_entry_cap;

	/* True once the runtime detected an already-running net session and is
	 * pumping jce_session_tick() in the fixed loop. */
	bool             net_session_driven;

	/* ── Networking bridge (P1-networking-full) ──────────────────────
	 * When a session is live at create() the runtime walks authored
	 * JceNetworkObject (+ JceNetTransform) entities, adopts them into the
	 * replication table (server only) and registers their transforms with
	 * the snapshot-interp module.  net_obj_count is the number bridged;
	 * net_bridged gates the per-frame transform fixed/render step. */
	bool             net_bridged;
	int              net_obj_count;

	/* ── Client-side prediction (rollback/replay) ────────────────────
	 * Wires jce_net_prediction (the generic predict ring + reconcile core)
	 * into the fixed loop for ONE locally-owned, server-authoritative entity
	 * (the predicted player).  predict_buf is NULL by default: with no
	 * predicted entity established the apply-input + reconcile hooks are
	 * provably no-ops and the fixed loop is byte-identical to before.
	 *
	 * The step function is the PURE deterministic kinematic integrator from
	 * jce_predict_locomotion (production Bullet resim is non-deterministic =
	 * documented follow-up).  predict_params holds its tuning (seeded to sane
	 * defaults at create, refreshed from the CharacterController feel when the
	 * predicted entity is established). */
	JcePredictionBuffer *predict_buf;     /* owned (NULL = prediction off)  */
	uint64_t             predict_entity;  /* 0 = none                       */
	JcePredictLocoParams predict_params;

	/* ── Scene / level transition (FEATURE 9.4) ──────────────────────
	 * Internal fade-out -> load -> fade-in state machine, advanced by
	 * jce_runtime_step.  jce_runtime_request_scene latches the target path
	 * and arms FADE_OUT; the LOAD step releases this scene's tracked runtime
	 * state (rt_teardown_scene_state), loads the target JSON into rt->scene
	 * (the SAME object the caller renders), and re-runs the spawn walks
	 * (rt_spawn_scene_state).  trans_alpha (0..1) is exposed as the fade-quad
	 * opacity.  fade_secs is the per-direction fade duration.  The desc fields
	 * captured at create() (pak / audio_load / mixer / navmesh / locale) are
	 * reused for every loaded scene; see rt->desc_* below. */
	int              trans_state;     /* JceRtTransitionState */
	char             trans_path[512]; /* target scene path (host-fs or PAK) */
	float            trans_alpha;     /* 0 clear .. 1 black */
	float            trans_timer;     /* seconds elapsed in the active phase */
	float            trans_fade_secs; /* per-direction fade duration */

	/* Desc fields needed to re-init subsystems for a newly loaded scene
	 * (captured once at create() so a transition reuses the same wiring). */
	const char      *desc_mixer_config_path;  /* not owned (caller-stable) */
	char             desc_navmesh_path[512];
	bool             enable_physics;
	float            desc_gravity[3];   /* resolved gravity vector (X,Y,Z) */
	float            desc_fixed_timestep;
	int32_t          desc_solver_iterations;
	float            desc_sleep_threshold;
	bool             desc_disable_auto_physics;
	float            desc_max_frame_dt;
	float            desc_gravity2d[2];  /* resolved 2D gravity (X,Y) */
};

/* Scene-transition phases (FEATURE 9.4). */
typedef enum {
	JCE_RT_TRANSITION_IDLE = 0,
	JCE_RT_TRANSITION_FADE_OUT,
	JCE_RT_TRANSITION_LOAD,
	JCE_RT_TRANSITION_FADE_IN
} JceRtTransitionState;

/* Default per-direction fade duration (seconds) when a transition is armed. */
#define RT_TRANSITION_FADE_SECS 0.25f

/* Per-frame ceiling on fixed physics ticks.  At 1/60 fixed_dt this lets
 * the sim catch up from a ~83 ms stall; beyond that we drop simulated
 * time (clamped inside the fixed clock) rather than spiral. */
#define RT_MAX_FIXED_STEPS  5

/* Primary-camera scan context, filled by rt_pick_primary_cam (own: core
 * jce_runtime.c) and consumed by the floating-origin rebase and the audio 3D
 * listener update — shared because the scan helper crosses the module
 * boundary. */
typedef struct {
	JceScene *scene;
	jce_vec3  pos;
	bool      found;
	/* Camera world orientation (only meaningful when found).  forward = the
	 * camera's -Z basis, up = its +Y basis, both pulled from its world
	 * matrix so a rotated/parented camera pans audio correctly.  Default
	 * (-Z / +Y) when the scene has no primary camera. */
	jce_vec3  forward;
	jce_vec3  up;
} CamScanCtx;

/* ── Shared internal runtime helpers (cross-module) ──────────── */

/* Array growers (external linkage).  RT_GROW_FN generates one grower body per
 * (field, cap-field, seed) triple; RT_GROW_DECL declares them.  The macro lives
 * here so any rt_*.c module can both DEFINE the growers it owns (e.g. the audio
 * module owns rt_grow_pending_audio) and CALL the growers core owns.  RT_GROW_
 * DECL keeps the declaration list in lock-step with the RT_GROW_FN bodies. */
#define RT_GROW_FN(fn, field, cap, seed) \
	bool fn(JceRuntime *rt) { \
		int new_cap = rt->cap ? rt->cap * 2 : (seed); \
		void *p = jce_realloc(rt->field, (size_t)new_cap * sizeof(*rt->field)); \
		if (!p) return false; \
		rt->field = p; rt->cap = new_cap; return true; }
#define RT_GROW_DECL(fn) bool fn(JceRuntime *rt)
RT_GROW_DECL(rt_grow_bodies);
RT_GROW_DECL(rt_grow_dd);
RT_GROW_DECL(rt_grow_voices);
RT_GROW_DECL(rt_grow_bodies2d);
RT_GROW_DECL(rt_grow_triggers);
RT_GROW_DECL(rt_grow_spawns);
RT_GROW_DECL(rt_grow_weapons);
RT_GROW_DECL(rt_grow_nav_entries);
RT_GROW_DECL(rt_grow_save_points);
RT_GROW_DECL(rt_grow_bts);
RT_GROW_DECL(rt_grow_scripts);
RT_GROW_DECL(rt_grow_gas);
RT_GROW_DECL(rt_grow_ragdoll);
RT_GROW_DECL(rt_grow_pending_spawns);
RT_GROW_DECL(rt_grow_pending_fractures);
RT_GROW_DECL(rt_grow_vehicles);
RT_GROW_DECL(rt_grow_softbodies);
RT_GROW_DECL(rt_grow_cfg_joints);
RT_GROW_DECL(rt_grow_joints2d);
RT_GROW_DECL(rt_grow_pending_audio);

/* Core lookups + the entity→body resolver (own: core jce_runtime.c). */
JceGameplayAbilitySystem *rt_gas_for_entity(JceRuntime *rt, JceEntity e);
VehicleEntry             *rt_vehicle_for_entity(JceRuntime *rt, JceEntity e);
JceBodyHandle             rt_body_for_entity(const JceRuntime *rt, JceEntity e);

/* Scripted RPC host callback (own: core jce_runtime.c networking section),
 * referenced by the script module's host-table install. */
bool rt_script_rpc_send(void *user, JceScriptEntity e, const char *event,
                        int target, const char *payload);

/* Script + behavior-tree module (own: jce_rt_script.c): the Lua host-table
 * install, the asset host-path/PAK fallback readers, prefab spawn, the trigger/
 * save sinks the core gameplay walk fires, the script hot-reload callback, and
 * the bundled BT perception adapters. */
void        rt_script_install_vm(JceRuntime *rt);

/* Instantiate `path` in the VM for ITS language, creating that VM on first
 * use.  The returned ref carries the handle that issued the instance, so the
 * two can never be separated; {NULL, 0} on any failure, every one of which is
 * logged with which of the two distinguishable causes it was (see
 * rt_script_language_for in jce_rt_script.c). */
RtScriptRef rt_script_instantiate(JceRuntime *rt, const char *path,
                                  JceEntity owner);

/* The already-created VM for `path`'s language, or NULL — WITHOUT creating
 * one.  Hot-reload uses this: a language with no live VM has no instance to
 * rebind, and standing an interpreter up to discover that would be absurd. */
JceScript  *rt_script_vm_for_path_existing(JceRuntime *rt, const char *path);

/* Destroy every per-language VM (teardown).  Safe to call twice. */
void        rt_script_destroy_vms(JceRuntime *rt);

/* Global (non-instance) handler dispatch — a UIButton's on_click, a sequencer
 * EVENT key.  These name a global function, not an instance, so there is no
 * ref to route them with: every live language is asked in creation order and
 * the first that HANDLED it wins.  See the comment on the definitions. */
bool        rt_script_call_named(JceRuntime *rt, const char *fn_name,
                                 JceScriptEntity arg_entity);
bool        rt_script_call_named_num(JceRuntime *rt, const char *fn_name,
                                     JceScriptEntity arg_entity, double value);
bool        rt_script_call_named_str(JceRuntime *rt, const char *fn_name,
                                     JceScriptEntity arg_entity,
                                     const char *str);

const char *rt_resolve_host_path(JceRuntime *rt, const char *path,
                                 char *buf, size_t cap);
void       *rt_read_asset_with_fallback(JceRuntime *rt, const char *path,
                                        uint64_t *out_size);
JceEntity   rt_spawn_prefab_at(JceRuntime *rt, const char *prefab_path,
                               float x, float y, float z);
void        rt_script_collision_cb(const JceContactEvent *ev, void *ud);
void        rt_on_script_changed(const char *path, void *user);
bool        rt_perform_save(JceRuntime *rt, const char *save_id);
bool        rt_bt_los_blocked(jce_vec3 from, jce_vec3 to, void *userdata);
JceBtStatus rt_bt_move_to(float gx, float gy, float gz, void *ud);
void        rt_bt_register_default_actions(JceRuntime *rt);

/* Scene helpers shared with the audio (and later) modules (own: core
 * jce_runtime.c). */
jce_vec3 rt_world_position(JceScene *scene, JceEntity e);
void     rt_pick_primary_cam(JceScene *s, JceEntity e, void *ud);

/* ── Simulation-LOD tick gating (own: core jce_runtime.c) ─────────────
 * Resolve the per-frame cadence for one gameplay subsystem of one entity from
 * its (optional) JceSimLodComponent.  Classifies the entity by distance to the
 * viewer into NEAR/MID/FAR with jce_lod hysteresis (prev_tier in/out), then
 * returns the tier's tick PERIOD in seconds for the given gate bit:
 *   period  < 0  -> tier is PAUSED (skip the subsystem entirely this frame),
 *   period == 0  -> tick every frame (no component, disabled, untiered gate,
 *                   or near_hz==0),
 *   period  > 0  -> tick once that many seconds have accumulated.
 * `gate_bit` is one of JCE_SIMLOD_GATE_*.  When the entity has no SimLod
 * component, or it is disabled, or this gate bit is clear, returns 0 (full
 * rate) so untiered entities are byte-identical to before. */
float rt_sim_lod_period(JceRuntime *rt, JceEntity e, jce_vec3 viewer,
                        uint32_t gate_bit, int *prev_tier);

/* Audio module (own: jce_rt_audio.c): mixer bus setup, reverb zones, navmesh
 * load, and the per-frame spatial-voice / listener 3D-audio update.  The
 * AudioSource→bus name resolver is also defined here but called from the core
 * audio-spawn path. */
void        rt_init_mixer(JceRuntime *rt, const char *config_path);
const char *rt_bus_for_source(const JceRuntime *rt,
                              const JceAudioSourceComponent *as, bool spatial);
void        rt_build_reverb_zones(JceRuntime *rt);
void        rt_init_navmesh(JceRuntime *rt, const char *navmesh_path);
void        rt_update_audio_3d(JceRuntime *rt, float dt);
/* Adaptive music director (own: jce_rt_audio.c).  rt_spawn_music builds
 * rt->music from an entity's MusicTrack component during the scene-spawn
 * walk; rt_tick_music advances it each frame from the audio update path. */
void        rt_spawn_music(JceRuntime *rt, JceScene *scene, JceEntity e);
void        rt_tick_music(JceRuntime *rt, float dt);
/* Canonical synchronous load path used by script one-shots and host-resolved
 * editor clips.  Honors JceRuntimeDesc.audio_load_fn before the PAK fallback. */
JceSound    rt_load_sound(JceRuntime *rt, const char *path);
/* Pending async audio-source decode (own: jce_rt_audio.c).  rt_finish_audio_
 * source + rt_spawn_audio_async are called from the core scene-spawn walk;
 * rt_audio_poll is pumped from jce_runtime_step. */
void        rt_finish_audio_source(JceRuntime *rt, JceScene *scene,
                                   JceEntity e, JceSound snd,
                                   const JceAudioSourceComponent *as);
void        rt_spawn_audio_async(JceRuntime *rt, JceEntity e,
                                 const JceAudioSourceComponent *as);
void        rt_audio_poll(JceRuntime *rt);

/* Physics body-spawn module (own: jce_rt_physics.c): the per-entity collider/
 * body materialisation helpers the core scene-walk driver (rt_spawn_entity)
 * dispatches, plus rt_spawn_entity_body (also reused by the draw-distance +
 * fracture passes) and rt_track_body (reused by the fracture spawn). */
void rt_track_body(JceRuntime *rt, JceEntity e, JceBodyHandle body,
                   const JceTransform *tf, uint8_t kind);
bool rt_try_spawn_compound(JceRuntime *rt, JceScene *scene,
                           JceEntity e, const JceTransform *tf);
bool rt_try_spawn_mesh(JceRuntime *rt, JceScene *scene,
                       JceEntity e, const JceTransform *tf);
bool rt_try_spawn_terrain(JceRuntime *rt, JceScene *scene, JceEntity e,
                          const JceTransform *tf);
bool rt_try_spawn_vehicle(JceRuntime *rt, JceScene *scene, JceEntity e,
                          const JceTransform *tf);
bool rt_try_spawn_softbody(JceRuntime *rt, JceScene *scene, JceEntity e,
                           const JceTransform *tf);
void rt_spawn_body2d(JceRuntime *rt, JceScene *scene,
                     JceEntity e, const JceTransform *tf);
void rt_spawn_tilemap_collider2d(JceRuntime *rt, JceScene *scene,
                                 JceEntity e, const JceTransform *tf);
bool rt_spawn_entity_body(JceRuntime *rt, JceScene *scene, JceEntity e,
                          bool allow_defer);

/* Fracture / destruction + physics draw-distance module (own:
 * jce_rt_fracture.c).  Both entry points are pumped from jce_runtime_step. */
void rt_drive_draw_distance(JceRuntime *rt);
void rt_flush_pending_fractures(JceRuntime *rt);

/* Is `name` a script method a REMOTE peer may invoke?  Explicit opt-in by
 * name: only "rpc_<something>" qualifies.  Shared by the send and receive
 * halves of the scripted-RPC channel so the two cannot disagree, and
 * declared here (rather than staying static) so the policy is testable
 * without standing up a network session — this is a security boundary, and
 * an untested security boundary is a hope.  See jce_runtime.c for why the
 * whole method surface used to be reachable. */
bool rt_script_rpc_name_allowed(const char *name);

#endif /* JCE_RT_INTERNAL_H */
