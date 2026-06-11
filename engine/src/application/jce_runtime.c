/*
 * jce_runtime.c  Layer 6 — Application.
 *
 * Authored-scene → live simulation driver shared by the editor's Play
 * button and deployed game executables.  See jce_runtime.h for contract.
 *
 * Walks the JceScene once at create() time, materialising:
 *   - one JcePhysicsWorld with a body for every (RigidBody +
 *     Box/SphereCollider) pair and a JceCharacterHandle for the first
 *     CharacterController found,
 *   - a JceVoice per AudioSource flagged play_on_awake.
 *
 * Per-step:
 *   1. character controller move + jump from the latest input
 *   2. jce_physics_step
 *   3. write back transforms
 *   4. jce_scene_update
 *   5. update audio listener (tracks primary camera) + spatial voices
 *
 * Implementation is intentionally allocation-heavy on create() rather
 * than per-frame: bodies/voices are sized to the actual scene contents,
 * not a fixed cap.
 */

#include <jce/application/jce_runtime.h>

#include <jce/middleware/audio/jce_audio.h>
#include <jce/middleware/audio/jce_audio_occlusion.h>
#include <jce/middleware/audio/jce_audio_mixer.h>
#include <jce/middleware/audio/jce_reverb_zones.h>
#include <jce/middleware/physics/jce_physics.h>
#include <jce/middleware/physics/jce_physics2d.h>
#include <jce/middleware/physics/jce_physics_types.h>
#include <jce/middleware/physics/jce_physics_layers.h>
#include <jce/middleware/physics/jce_physics_material.h>
#include <jce/middleware/physics/jce_physics_debug.h>
#include <jce/middleware/physics/jce_collider_cook.h>
#include <jce/middleware/physics/jce_collider_asset.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_sequencer.h>
#include <jce/middleware/scene/jce_tilemap.h>
#include <jce/middleware/world/jce_trigger_volume.h>
#include <jce/middleware/world/jce_spawn_manager.h>
#include <jce/middleware/world/jce_weapon.h>
#include <jce/middleware/ai/jce_bt.h>
#include <jce/middleware/ai/jce_perception.h>
#include <jce/middleware/ai/jce_nav_agent.h>
#include <jce/middleware/ai/jce_navmesh_recast.h>
#include <jce/middleware/save/jce_snapshot.h>
#include <jce/middleware/save/jce_save_providers.h>
#include <jce/middleware/net/jce_session.h>
#include <jce/middleware/net/jce_replication.h>
#include <jce/middleware/net/jce_net_transform.h>
#include <jce/middleware/ui/jce_localization.h>
#include <jce/os/platform/jce_host_locale.h>
#include <jce/resource/jce_model_importer.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_fixed_clock.h>
#include <jce/os/core/jce_thread.h>   /* async audio-source decode */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
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
	uint8_t       kind;        /* JceBodyType: static bodies skip write-back. */
} BodyEntry;

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
};

/* Worker args for an async audio-source decode.  Heap-allocated and owned
 * by the worker for its full run, so it stays valid even if the pending
 * array reallocates (slot pointers must NOT be handed to the worker). */
typedef struct {
	const JcePakArchive *pak;
	char                 path[256];
	JceAudioCpu         *cpu;    /* worker writes */
	JceAtomicI32        *done;   /* 0 working, 1 finished */
} RtAudioDecodeArgs;

/* In-flight async decode of a play_on_awake audio source.  The worker
 * decodes the clip to CPU PCM; jce_runtime_step uploads + plays it (the
 * sound starts a frame or two late instead of stalling scene load). */
typedef struct {
	JceEntity          entity;
	JceThread         *thr;
	RtAudioDecodeArgs *args;   /* stable heap; holds done + cpu */
} RtPendingAudio;

struct JceRuntime {
	JceScene        *scene;       /* not owned */
	JcePakArchive   *pak;         /* not owned */
	JceAudio        *audio;       /* not owned */

	uint32_t      (*audio_load_fn)(void *, JceAudio *, const char *);
	void            *user_data;

	JcePhysicsWorld *physics;     /* owned (NULL if !enable_physics) */
	JcePhysics2D    *physics2d;   /* owned (NULL if !enable_physics) */

	BodyEntry       *bodies;
	int              body_count;
	int              body_cap;

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
	 * Pure-CPU bus tree (solo/mute/volume) seeded from audio_mixer.json,
	 * mirrored onto ma_sound_group buses in rt->audio.  resolve_volume is
	 * pushed onto each bus group every frame so the editor's Music/SFX/
	 * Voice sliders drive live playback.  NULL when audio is disabled. */
	JceAudioMixer   *mixer;          /* owned */

	/* ── Reverb zones (P1-audio-mixer-reverb) ────────────────────────
	 * Built from scene AudioReverbZone components; sampled at the listener
	 * each frame and the blended preset driven into the global reverb DSP.
	 * NULL when the scene authored no reverb zones. */
	JceReverbZones  *reverb_zones;   /* owned */

	JceRuntimeInput  input;

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

	/* Game-facing contact listener (BEGIN/STAY/END).  Registered lazily on
	 * first set so the manifold-diff cost stays zero until a game subscribes. */
	jce_contact_listener_fn contact_cb;
	void                   *contact_ud;
	bool                    contact_registered;

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
	uint64_t         spawn_cookie_seq; /* stable monotonic cookie source */

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

	struct BtEntry  *bts;
	int              bt_count;
	int              bt_cap;

	/* ── Save / snapshot (P2-save-snapshot) ──────────────────────────
	 * A snapshot registry stood up at create() with the scene/ECS provider
	 * registered, so a play session can be persisted + restored.  Authored
	 * SavePoint components are mirrored as sphere triggers in trigger_world;
	 * a player overlap writes "<saves_dir>/<save_id>.jsnp".  saves_dir is
	 * empty when no save directory was supplied (auto-save disabled, but the
	 * registry is still usable via jce_runtime_save_registry). */
	JceSnapshotRegistry *save_registry;   /* owned */
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
};

/* Per-frame ceiling on fixed physics ticks.  At 1/60 fixed_dt this lets
 * the sim catch up from a ~83 ms stall; beyond that we drop simulated
 * time (clamped inside the fixed clock) rather than spiral. */
#define RT_MAX_FIXED_STEPS  5

/* Forward decl: defined alongside the audio-mixer helpers below, but used by
 * the AudioSource spawn path above them. */
static const char *rt_bus_for_source(const JceRuntime *rt,
                                     const JceAudioSourceComponent *as,
                                     bool spatial);

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Compose the full parent-chain world matrix and return its translation
 * column.  Unlike a plain local-position sum, this honours parent rotation
 * and scale, so a source parented to a rotating/moving rig is placed at its
 * true world location.  Reuses the shared engine world-matrix path so it
 * stays consistent with rendering/picking. */
static jce_vec3 rt_world_position(JceScene *scene, JceEntity e)
{
	if (e == 0 || !jce_scene_has_transform(scene, e))
		return jce_v3(0.0f, 0.0f, 0.0f);
	jce_mat4 w = jce_scene_get_world_matrix(scene, e);
	return jce_v3(w.raw[3][0], w.raw[3][1], w.raw[3][2]);
}

static bool rt_grow_bodies(JceRuntime *rt)
{
	int new_cap = rt->body_cap ? rt->body_cap * 2 : 16;
	BodyEntry *p = (BodyEntry *)jce_realloc(rt->bodies,
	                                         (size_t)new_cap * sizeof(*p));
	if (!p) return false;
	rt->bodies   = p;
	rt->body_cap = new_cap;
	return true;
}

static bool rt_grow_voices(JceRuntime *rt)
{
	int new_cap = rt->voice_cap ? rt->voice_cap * 2 : 8;
	VoiceEntry *p = (VoiceEntry *)jce_realloc(rt->voices,
	                                           (size_t)new_cap * sizeof(*p));
	if (!p) return false;
	rt->voices   = p;
	rt->voice_cap = new_cap;
	return true;
}

static bool rt_grow_bodies2d(JceRuntime *rt)
{
	int new_cap = rt->body2d_cap ? rt->body2d_cap * 2 : 16;
	Body2DEntry *p = (Body2DEntry *)jce_realloc(rt->bodies2d,
	                                            (size_t)new_cap * sizeof(*p));
	if (!p) return false;
	rt->bodies2d   = p;
	rt->body2d_cap = new_cap;
	return true;
}

static bool rt_grow_triggers(JceRuntime *rt)
{
	int new_cap = rt->trigger_cap ? rt->trigger_cap * 2 : 8;
	TriggerEntry *p = (TriggerEntry *)jce_realloc(rt->triggers,
	                                              (size_t)new_cap * sizeof(*p));
	if (!p) return false;
	rt->triggers    = p;
	rt->trigger_cap = new_cap;
	return true;
}

static bool rt_grow_spawns(JceRuntime *rt)
{
	int new_cap = rt->spawn_cap ? rt->spawn_cap * 2 : 4;
	SpawnEntry *p = (SpawnEntry *)jce_realloc(rt->spawns,
	                                          (size_t)new_cap * sizeof(*p));
	if (!p) return false;
	rt->spawns    = p;
	rt->spawn_cap = new_cap;
	return true;
}

static bool rt_grow_weapons(JceRuntime *rt)
{
	int new_cap = rt->weapon_cap ? rt->weapon_cap * 2 : 4;
	WeaponEntry *p = (WeaponEntry *)jce_realloc(rt->weapons,
	                                            (size_t)new_cap * sizeof(*p));
	if (!p) return false;
	rt->weapons    = p;
	rt->weapon_cap = new_cap;
	return true;
}

static bool rt_grow_nav_entries(JceRuntime *rt)
{
	int new_cap = rt->nav_entry_cap ? rt->nav_entry_cap * 2 : 4;
	NavAgentEntry *p = (NavAgentEntry *)jce_realloc(rt->nav_entries,
	                                                (size_t)new_cap * sizeof(*p));
	if (!p) return false;
	rt->nav_entries   = p;
	rt->nav_entry_cap = new_cap;
	return true;
}

static bool rt_grow_save_points(JceRuntime *rt)
{
	int new_cap = rt->save_point_cap ? rt->save_point_cap * 2 : 4;
	SavePointEntry *p = (SavePointEntry *)jce_realloc(rt->save_points,
	                                                  (size_t)new_cap * sizeof(*p));
	if (!p) return false;
	rt->save_points    = p;
	rt->save_point_cap = new_cap;
	return true;
}

static bool rt_grow_bts(JceRuntime *rt)
{
	int new_cap = rt->bt_cap ? rt->bt_cap * 2 : 4;
	struct BtEntry *p = (struct BtEntry *)jce_realloc(rt->bts,
	                                                  (size_t)new_cap * sizeof(*p));
	if (!p) return false;
	rt->bts    = p;
	rt->bt_cap = new_cap;
	return true;
}

/* Write a session snapshot to "<saves_dir>/<save_id>.jsnp" through the
 * runtime registry, creating the saves directory on demand (P2-save-snapshot).
 * No-op (returns false) when no saves directory was configured. */
static bool rt_perform_save(JceRuntime *rt, const char *save_id)
{
	if (!rt || !rt->save_registry) return false;
	if (rt->saves_dir[0] == '\0') {
		LOG_WARN(LOG_TAG, "save point fired but no saves_dir configured");
		return false;
	}
	if (!save_id || !save_id[0]) save_id = "checkpoint";

	if (!jce_fs_host_exists_dir(rt->saves_dir) &&
	    !jce_fs_host_create_directory(rt->saves_dir)) {
		LOG_ERROR(LOG_TAG, "save: cannot create directory '%s'", rt->saves_dir);
		return false;
	}

	char path[640];
	int n = snprintf(path, sizeof path, "%s/%s.jsnp", rt->saves_dir, save_id);
	if (n <= 0 || (size_t)n >= sizeof path) return false;

	bool ok = jce_snapshot_save_to_file(rt->save_registry, path);
	if (ok) LOG_INFO(LOG_TAG, "save point: wrote snapshot '%s'", path);
	else    LOG_ERROR(LOG_TAG, "save point: write failed '%s'", path);
	return ok;
}

/* ── Gameplay-bridge sinks (P0-master-bridge) ────────────────────────
 *
 * The trigger world fires enter/stay/exit through this sink.  We log
 * transitions (ENTER/EXIT) at debug level so designers can verify zones
 * react in Play; STAY is intentionally not logged (it would spam).  Games
 * that need real reactions subscribe to their own sink via the engine
 * trigger API once they hold the JceTriggerWorld (exposed in a follow-up).
 *
 * A SavePoint volume is the one zone the runtime acts on directly: on ENTER
 * it writes a snapshot (P2-save-snapshot), unless the point is one_shot and
 * already fired, or require_interact (left to game input — not auto-fired).
 */
static void rt_trigger_event(const JceTriggerEvent *ev, void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!ev) return;
	if (ev->type == JCE_TRIGGER_EVENT_ENTER) {
		LOG_INFO(LOG_TAG, "trigger ENTER: zone entity=%llu observer=%llu",
		         (unsigned long long)ev->trigger_user,
		         (unsigned long long)ev->observer_user);
		/* SavePoint overlap → snapshot (skip require_interact points). */
		if (rt) {
			for (int i = 0; i < rt->save_point_count; ++i) {
				SavePointEntry *sp = &rt->save_points[i];
				if ((uint64_t)sp->entity != ev->trigger_user) continue;
				if (sp->require_interact) break;   /* game-driven, not here */
				if (sp->one_shot && sp->fired) break;
				if (rt_perform_save(rt, sp->save_id)) sp->fired = true;
				break;
			}
		}
	} else if (ev->type == JCE_TRIGGER_EVENT_EXIT) {
		LOG_INFO(LOG_TAG, "trigger EXIT: zone entity=%llu observer=%llu",
		         (unsigned long long)ev->trigger_user,
		         (unsigned long long)ev->observer_user);
	}
}

/* Spawn-manager create/destroy callbacks.  The runtime does not author
 * game entities itself (that is a game/prefab decision), so these issue a
 * monotonic cookie on create and accept the despawn — keeping the spawn
 * state machine (cadence, soft-cap, ring eviction) running so designers
 * can validate pacing in Play.  A game overrides this by driving its own
 * JceSpawnManager. */
static uint64_t rt_spawn_create(const JceSpawnRequest *req, void *user)
{
	(void)req;
	uint64_t *counter = (uint64_t *)user;
	return counter ? ++(*counter) : 1u;
}

static void rt_spawn_destroy(uint64_t cookie, void *user)
{
	(void)cookie; (void)user;
}

/* ── Behavior-tree perception + actions (P2-perception-bt-binding) ─────
 *
 * Line-of-sight adapter: the perception module asks "is the segment
 * from→to BLOCKED?".  We raycast the physics world (closest hit, no
 * triggers) and report blocked when something is hit short of the target.
 * A small epsilon keeps the target's own collider (right at `to`) from
 * counting as an occluder. */
static bool rt_bt_los_blocked(jce_vec3 from, jce_vec3 to, void *userdata)
{
	JceRuntime *rt = (JceRuntime *)userdata;
	if (!rt || !rt->physics) return false;   /* no physics → assume clear */
	jce_vec3 seg = jce_v3_sub(to, from);
	float dist = jce_v3_len(seg);
	if (dist <= 1e-4f) return false;
	jce_vec3 dir = jce_v3_scale(seg, 1.0f / dist);
	JceQueryFilter filter = jce_query_filter_default();   /* skip triggers */
	JceRaycastResult r = jce_physics_raycast_filtered(rt->physics, from, dir,
	                                                  dist, filter);
	if (!r.hit) return false;
	/* Hit something before reaching the target (minus a small skin) → blocked. */
	return r.distance < (dist - 0.1f);
}

/* The bundled BT actions operate on rt->bt_active_bb (the blackboard of the
 * agent currently being ticked).  These are deliberately generic primitives
 * so an authored tree can react to perception without any game C code:
 *
 *   IsTargetVisible — SUCCESS when perception saw a target this tick.
 *   HasTarget       — SUCCESS when a target entity is known (seen now OR a
 *                     last-known position was recorded).
 *   HasHeardSound   — SUCCESS when a sound was heard this tick.
 *   IsTargetInRange — SUCCESS when target.distance <= "attack.range"
 *                     (default 2m) — a melee/attack gate.
 *
 * Game-specific actions (MoveTo, Attack, …) stay in game code: it registers
 * them on jce_runtime_bt_context() before Play, and they read the same
 * blackboard via jce_runtime_bt_blackboard(). */
static JceBtStatus rt_bt_action_is_visible(const char *name, void *ud)
{
	(void)name;
	JceRuntime *rt = (JceRuntime *)ud;
	if (!rt || !rt->bt_active_bb) return JCE_BT_FAILURE;
	return jce_blackboard_get_bool(rt->bt_active_bb, "target.visible", false)
	       ? JCE_BT_SUCCESS : JCE_BT_FAILURE;
}

static JceBtStatus rt_bt_action_has_target(const char *name, void *ud)
{
	(void)name;
	JceRuntime *rt = (JceRuntime *)ud;
	if (!rt || !rt->bt_active_bb) return JCE_BT_FAILURE;
	if (jce_blackboard_get_entity(rt->bt_active_bb, "target.entity", 0) != 0)
		return JCE_BT_SUCCESS;
	return jce_blackboard_has(rt->bt_active_bb, "target.last_known_position")
	       ? JCE_BT_SUCCESS : JCE_BT_FAILURE;
}

static JceBtStatus rt_bt_action_has_heard(const char *name, void *ud)
{
	(void)name;
	JceRuntime *rt = (JceRuntime *)ud;
	if (!rt || !rt->bt_active_bb) return JCE_BT_FAILURE;
	return jce_blackboard_get_bool(rt->bt_active_bb, "sound.heard", false)
	       ? JCE_BT_SUCCESS : JCE_BT_FAILURE;
}

static JceBtStatus rt_bt_action_in_range(const char *name, void *ud)
{
	(void)name;
	JceRuntime *rt = (JceRuntime *)ud;
	if (!rt || !rt->bt_active_bb) return JCE_BT_FAILURE;
	if (!jce_blackboard_get_bool(rt->bt_active_bb, "target.visible", false))
		return JCE_BT_FAILURE;
	float range = jce_blackboard_get_float(rt->bt_active_bb, "attack.range", 2.0f);
	float dist  = jce_blackboard_get_float(rt->bt_active_bb, "target.distance", 1e9f);
	return (dist <= range) ? JCE_BT_SUCCESS : JCE_BT_FAILURE;
}

/* Register the bundled perception-reading actions on the BT context once. */
static void rt_bt_register_default_actions(JceRuntime *rt)
{
	if (!rt->bt_ctx || rt->bt_actions_registered) return;
	jce_bt_register_action(rt->bt_ctx, "IsTargetVisible", rt_bt_action_is_visible, rt);
	jce_bt_register_action(rt->bt_ctx, "HasTarget",       rt_bt_action_has_target, rt);
	jce_bt_register_action(rt->bt_ctx, "HasHeardSound",   rt_bt_action_has_heard,  rt);
	jce_bt_register_action(rt->bt_ctx, "IsTargetInRange", rt_bt_action_in_range,   rt);
	rt->bt_actions_registered = true;
}

/* ── Scene walk: spawn physics + audio for each entity ───────────── */

/*
 * Apply authored per-body properties that must be set AFTER body creation:
 *   - collision layer  -> broadphase group/mask via the layer matrix
 *   - per-body gravity -> world gravity * gravity_scale (0 when !use_gravity)
 *   - physics material -> friction/restitution from a .physmat.json asset
 *
 * `rb` may be NULL (e.g. a static compound with no Rigidbody). `physmat_path`
 * is the collider/body material override (may be NULL/empty).
 */
static void rt_apply_body_extras(JceRuntime *rt, JceEntity e, JceBodyHandle body,
                                 const JceRigidBodyComponent *rb,
                                 const char *physmat_path)
{
	if (!rt->physics || !jce_body_valid(body)) return;

	/* Tag the body with its entity so contact events carry entity_a/_b. */
	jce_physics_body_set_entity(rt->physics, body, (uint64_t)e);

	/* Collision layer (default layer 0 when no rigidbody). */
	jce_physics_body_set_layer(rt->physics, body,
	                           rb ? rb->physics_layer : 0u);

	/* Per-body gravity.  use_gravity=false -> factor 0 (floats).
	 * Guard an uninitialised gravity_scale (0 while gravity is enabled
	 * is contradictory -> treat as normal 1.0 so bodies don't float). */
	if (rb) {
		float gf = rb->use_gravity ? rb->gravity_scale : 0.0f;
		if (rb->use_gravity && rb->gravity_scale == 0.0f) gf = 1.0f;
		if (gf != 1.0f)
			jce_physics_body_set_gravity_factor(rt->physics, body, gf);
	}

	/* Physics material override (host-FS .physmat.json). */
	if (physmat_path && physmat_path[0]) {
		JcePhysicsMaterial pm;
		jce_physics_material_init_default(&pm);
		if (jce_physics_material_load(physmat_path, &pm))
			jce_physics_body_set_material(rt->physics, body, &pm);
		else
			LOG_WARN(LOG_TAG, "physmat: cannot load '%s'", physmat_path);
	}
}

/* Record a spawned body + seed its TRS sync cache from the entity transform. */
static void rt_track_body(JceRuntime *rt, JceEntity e, JceBodyHandle body,
                          const JceTransform *tf, uint8_t kind)
{
	BodyEntry *be = &rt->bodies[rt->body_count];
	be->entity      = e;
	be->body        = body;
	be->last_pos    = tf->position;
	be->last_rot    = tf->rotation;
	be->last_scale  = tf->scale;
	be->spawn_scale = tf->scale;
	/* Seed both interpolation endpoints to the spawn pose so the first
	 * frames before any fixed tick blend to a no-op. */
	be->prev_pos    = tf->position;
	be->prev_rot    = tf->rotation;
	be->cur_pos     = tf->position;
	be->cur_rot     = tf->rotation;
	be->kind        = kind;
	rt->body_count++;
}

/*
 * Try to load a precomputed (offline-cooked) compound-collider blob that
 * sits beside the model as "<model_path>.jcol".  This lets a model whose
 * compound collider was baked by `jce_cook --collider` skip the expensive
 * live VHACD / triangle-mesh cook at every scene load.  Looks in the pak
 * first (deployed builds), then the host filesystem (editor).  On success
 * `out` receives an owned cooked tree (free with jce_collider_cooked_free)
 * and the function returns true; on any miss it returns false and the
 * caller falls back to the live cook.
 */
static bool rt_try_load_cached_collider(JceRuntime *rt, const char *model_path,
                                        JceCookedCollider *out)
{
	if (!model_path || !model_path[0] || !out) return false;

	char blob_path[1024];
	int n = snprintf(blob_path, sizeof blob_path, "%s.jcol", model_path);
	if (n <= 0 || (size_t)n >= sizeof blob_path) return false;

	bool ok = false;

	/* 1. Pak-resident blob (deployed game). */
	if (rt->pak) {
		const JcePakAsset *asset = jce_pak_find(rt->pak, blob_path);
		if (asset && asset->original_size > 0) {
			void *buf = jce_malloc((size_t)asset->original_size);
			if (buf) {
				size_t got = jce_pak_decompress(asset, buf,
				                                (size_t)asset->original_size);
				if (got > 0)
					ok = jce_collider_deserialize(buf, (uint32_t)got, out);
				jce_free(buf);
			}
		}
	}

	/* 2. Host-filesystem sibling blob (editor / loose build). */
	if (!ok) {
		uint64_t sz = 0;
		void *buf = jce_fs_host_read_all(blob_path, &sz);
		if (buf) {
			if (sz > 0 && sz <= 0xFFFFFFFFull)
				ok = jce_collider_deserialize(buf, (uint32_t)sz, out);
			jce_fs_buffer_free(buf);
		}
	}

	if (ok)
		LOG_INFO(LOG_TAG, "compound collider: loaded cached blob %s", blob_path);
	return ok;
}

/*
 * Cook + instantiate a body for entity `e` from a compound-collider
 * description.  Shared by the real Compound Collider component and the
 * Mesh Collider (which synthesises a single-shape description).
 * `allow_blob_cache` gates the offline ".jcol" blob lookup — the blob is
 * cooked with the compound's own settings (AUTO + static), so callers
 * whose cook settings differ (e.g. a convex mesh collider) must skip it.
 *
 * Note: the entity's TRS scale is NOT baked into the cooked shapes
 * (pre-existing jce_collider_instantiate behavior, kept for parity).
 *
 * Returns true if a body was spawned (caller then skips the regular
 * rigid-body path so the entity does not get a second body).
 */
static bool rt_spawn_cooked_body(JceRuntime *rt, JceScene *scene, JceEntity e,
                                 const JceTransform *tf,
                                 const JceCompoundColliderComponent *cc,
                                 bool allow_blob_cache)
{
	if (!cc || cc->model_path[0] == '\0') return false;

	/* Prefer a precomputed (offline-cooked) blob beside the model so we do
	 * not re-cook colliders live at every scene load.  Fall through to the
	 * live cook below on a cache miss. */
	JceCookedCollider cooked;
	bool have_cooked = allow_blob_cache &&
	                   rt_try_load_cached_collider(rt, cc->model_path, &cooked);

	if (!have_cooked) {

		/* Load parts from the pak (deployed) or the host filesystem (editor). */
		JceModelParts parts;
		memset(&parts, 0, sizeof parts);
		bool loaded = false;
		if (rt->pak) {
			const JcePakAsset *asset = jce_pak_find(rt->pak, cc->model_path);
			if (asset) {
				void *buf = jce_malloc((size_t)asset->original_size);
				if (buf) {
					size_t n = jce_pak_decompress(asset, buf,
					                              (size_t)asset->original_size);
					if (n > 0) {
						const char *ext = strrchr(cc->model_path, '.');
						loaded = jce_model_importer_load_parts_memory(
							buf, n, ext ? ext : "", &parts);
					}
					jce_free(buf);
				}
			}
		}
		if (!loaded)
			loaded = jce_model_importer_load_parts_file(cc->model_path, &parts);
		if (!loaded) {
			LOG_WARN(LOG_TAG, "compound collider: cannot load %s", cc->model_path);
			return false;
		}

		JceColliderPart *cparts =
			(JceColliderPart *)jce_malloc((size_t)parts.count * sizeof(*cparts));
		if (!cparts) { jce_model_importer_free_parts(&parts); return false; }
		for (uint32_t i = 0; i < parts.count; i++) {
			cparts[i].name         = parts.parts[i].name;
			cparts[i].vertices     = parts.parts[i].positions;
			cparts[i].vertex_count = parts.parts[i].vertex_count;
			cparts[i].indices      = parts.parts[i].indices;
			cparts[i].index_count  = parts.parts[i].index_count;
			memcpy(cparts[i].transform, parts.parts[i].transform,
			       sizeof cparts[i].transform);
		}

		JceColliderCookConfig cfg = jce_collider_cook_config_default();
		cfg.mode          = (JceColliderMode)cc->mode;
		cfg.split         = (JceColliderSplitMode)cc->split;
		cfg.is_static     = cc->is_static;
		cfg.detect_naming = cc->detect_naming;
		if (cc->vhacd_resolution)         cfg.vhacd_resolution = cc->vhacd_resolution;
		if (cc->vhacd_max_hulls)          cfg.vhacd_max_hulls = cc->vhacd_max_hulls;
		if (cc->vhacd_max_verts_per_hull) cfg.vhacd_max_verts_per_hull = cc->vhacd_max_verts_per_hull;

		bool cooked_ok = jce_collider_cook(cparts, parts.count, &cfg, &cooked);
		jce_free(cparts);
		jce_model_importer_free_parts(&parts);
		if (!cooked_ok) {
			LOG_WARN(LOG_TAG, "compound collider: cook failed for %s", cc->model_path);
			return false;
		}
	} /* !have_cooked */

	JceColliderInstanceDesc id;
	memset(&id, 0, sizeof id);
	id.position    = tf->position;
	id.rotation    = tf->rotation;
	id.friction    = cc->friction > 0.0f ? cc->friction : 0.5f;
	id.restitution = cc->restitution;
	id.is_trigger  = cc->is_trigger;

	JceRigidBodyComponent *rb = jce_scene_get_rigidbody(scene, e);
	if (rb) {
		id.mass            = rb->mass;
		id.linear_damping  = rb->drag;
		id.angular_damping = rb->angular_drag;
		if (rb->is_kinematic)      id.type = JCE_BODY_KINEMATIC;
		else if (rb->mass <= 0.0f) id.type = JCE_BODY_STATIC;
		else                       id.type = JCE_BODY_DYNAMIC;
	} else {
		id.type = JCE_BODY_STATIC;
	}

	JceBodyHandle body = jce_collider_instantiate(rt->physics, &cooked, &id);
	jce_collider_cooked_free(&cooked);
	if (!jce_body_valid(body)) return false;

	/* Layer / gravity / material — material override prefers the compound's
	 * own slot, else the Rigidbody's. */
	rt_apply_body_extras(rt, e, body, rb,
	                     cc->physmat_path[0] ? cc->physmat_path
	                     : (rb ? rb->physmat_path : NULL));

	if (rt->body_count >= rt->body_cap && !rt_grow_bodies(rt))
		return true;   /* spawned but cannot track — still skip box path */
	rt_track_body(rt, e, body, tf, (uint8_t)id.type);
	return true;
}

/*
 * Try to materialise a per-object compound collider for entity `e`.
 * Loads the referenced model WITHOUT flattening its node hierarchy, cooks
 * each part into its own child shape, and instantiates the lot as a single
 * compound body — so a model holding N separated objects yields N child
 * colliders rather than one fat hull spanning the gaps between them.
 *
 * Returns true if a body was spawned (caller then skips the regular
 * rigid-body path so the entity does not get a second body).
 */
static bool rt_try_spawn_compound(JceRuntime *rt, JceScene *scene,
                                  JceEntity e, const JceTransform *tf)
{
	JceCompoundColliderComponent *cc = jce_scene_get_compound_collider(scene, e);
	if (!cc || cc->model_path[0] == '\0') return false;
	return rt_spawn_cooked_body(rt, scene, e, tf, cc, true);
}

/*
 * Try to materialise a Mesh Collider for entity `e` (Unity MeshCollider
 * semantics: ONE shape cooked from the whole referenced mesh).  Reuses the
 * compound cook/instantiate path via a synthetic single-shape description:
 * exact triangle mesh for static bodies, convex hull when `convex` is set
 * or the body is dynamic (Bullet triangle meshes are static-only).
 *
 * Returns true if a body was spawned (caller then skips the regular
 * rigid-body path so the entity does not get a second body).
 */
static bool rt_try_spawn_mesh(JceRuntime *rt, JceScene *scene,
                              JceEntity e, const JceTransform *tf)
{
	JceMeshColliderComponent *mc = jce_scene_get_mesh_collider(scene, e);
	if (!mc || mc->mesh_path[0] == '\0') return false;
	if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_MESH_COLLIDER))
		return false;

	JceRigidBodyComponent *rb = jce_scene_get_rigidbody(scene, e);
	bool dynamic = rb && !rb->is_kinematic && rb->mass > 0.0f;
	bool convex  = mc->convex;
	if (dynamic && !convex) {
		LOG_WARN(LOG_TAG, "mesh collider on dynamic rigidbody requires convex; "
		         "cooking convex hull instead (entity %llu, %s)",
		         (unsigned long long)e, mc->mesh_path);
		convex = true;
	}

	/* Synthetic compound description: one shape for the whole model. */
	JceCompoundColliderComponent cc;
	memset(&cc, 0, sizeof cc);
	memcpy(cc.model_path, mc->mesh_path, sizeof cc.model_path);
	cc.mode          = (uint8_t)(convex ? JCE_COLLIDER_MODE_CONVEX_HULL
	                                    : JCE_COLLIDER_MODE_TRIANGLE_MESH);
	cc.split         = (uint8_t)JCE_COLLIDER_SPLIT_WHOLE;
	cc.is_static     = !dynamic;
	cc.detect_naming = false;
	cc.is_trigger    = mc->is_trigger;
	cc.friction      = mc->friction;
	cc.restitution   = mc->restitution;

	/* The offline ".jcol" blob beside the model is cooked AUTO + static —
	 * only shape-compatible with the static triangle-mesh case here. */
	bool allow_blob_cache = !convex && !dynamic;
	return rt_spawn_cooked_body(rt, scene, e, tf, &cc, allow_blob_cache);
}

/*
 * Spawn a 2D rigid body for entity `e` from its RigidBody2DComponent
 * (+ optional Collider2DComponent) into the Box2D world.  The simulation
 * runs in the XY plane: the entity's Transform x/y seed the body position,
 * the Z-rotation angle seeds the body angle, and the body's half-extents
 * come from the collider (scaled by the entity's XY scale) — or a unit box
 * when no collider is authored.
 *
 * The Collider2D shape enum (JCE_COLLIDER_2D_*) differs from the wrapper's
 * JceShape2DType: EDGE maps to SEGMENT, and POLYGON has no wrapper shape so
 * it falls back to BOX (noted as a limitation).
 */
static void rt_spawn_body2d(JceRuntime *rt, JceScene *scene,
                            JceEntity e, const JceTransform *tf)
{
	JceRigidBody2DComponent *rb = jce_scene_get_rigidbody2d(scene, e);
	if (!rb) return;

	JceBody2DDesc bd;
	memset(&bd, 0, sizeof bd);

	/* XY plane: take x/y from the transform, drop z. */
	bd.position.x = tf->position.x;
	bd.position.y = tf->position.y;

	/* Recover the Z-rotation angle (radians) from the transform quaternion.
	 * For a pure Z rotation q = (0,0,sin(a/2),cos(a/2)) this is exact;
	 * atan2 keeps it well-behaved for small off-axis tilts. */
	{
		jce_quat q = tf->rotation;
		bd.angle = atan2f(2.0f * (q.w * q.z + q.x * q.y),
		                  1.0f - 2.0f * (q.y * q.y + q.z * q.z));
	}

	bd.mass            = rb->mass;
	bd.friction        = rb->friction > 0.0f ? rb->friction : 0.5f;
	bd.restitution     = rb->restitution;
	bd.fixed_rotation  = rb->fixed_rotation;

	/* Body type: kinematic flag / zero-mass static / dynamic. */
	if (rb->body_type == JCE_BODY_KINEMATIC) bd.type = JCE_BODY_KINEMATIC;
	else if (rb->body_type == JCE_BODY_STATIC || rb->mass <= 0.0f)
		bd.type = JCE_BODY_STATIC;
	else                                     bd.type = JCE_BODY_DYNAMIC;

	/* 2D body: x/y projection of the abs-sanitized scale. */
	jce_vec3 s2 = jce_v3_abs_safe_scale(tf->scale);
	float sx = s2.x;
	float sy = s2.y;
	float smax = sx > sy ? sx : sy;

	/* Shape + extents from the optional Collider2D; default to a unit box. */
	JceCollider2DComponent *col = jce_scene_get_collider2d(scene, e);
	if (col) {
		bd.position.x += col->offset[0];
		bd.position.y += col->offset[1];
		bd.friction    = col->friction > 0.0f ? col->friction : bd.friction;
		bd.restitution = col->restitution;
		switch (col->shape) {
			case JCE_COLLIDER_2D_CIRCLE: {
				float r = col->radius > 0.0f ? col->radius : 0.5f;
				bd.shape = JCE_SHAPE2D_CIRCLE;
				bd.half_extents.x = r * smax;
				bd.half_extents.y = 0.0f;
				break;
			}
			case JCE_COLLIDER_2D_CAPSULE: {
				float r  = col->radius > 0.0f ? col->radius : 0.25f;
				/* size.y is the full length; wrapper wants half_length. */
				float hl = 0.5f * (col->size[1] > 0.0f ? col->size[1] : 1.0f);
				bd.shape = JCE_SHAPE2D_CAPSULE;
				bd.half_extents.x = r * smax;       /* radius */
				bd.half_extents.y = hl * sy;        /* half length */
				break;
			}
			case JCE_COLLIDER_2D_EDGE: {
				/* No multi-point edge support in the wrapper — approximate
				 * with a single horizontal segment spanning size.x. */
				bd.shape = JCE_SHAPE2D_SEGMENT;
				bd.half_extents.x = 0.5f * (col->size[0] > 0.0f ? col->size[0] : 1.0f) * sx;
				bd.half_extents.y = 0.0f;
				break;
			}
			case JCE_COLLIDER_2D_POLYGON:
				/* Wrapper has no arbitrary-polygon shape — fall back to the
				 * collider's bounding box (limitation, noted). */
				/* fall through */
			case JCE_COLLIDER_2D_BOX:
			default: {
				bd.shape = JCE_SHAPE2D_BOX;
				bd.half_extents.x = 0.5f * (col->size[0] > 0.0f ? col->size[0] : 1.0f) * sx;
				bd.half_extents.y = 0.5f * (col->size[1] > 0.0f ? col->size[1] : 1.0f) * sy;
				break;
			}
		}
	} else {
		/* No collider authored — placeholder unit box from XY scale. */
		bd.shape = JCE_SHAPE2D_BOX;
		bd.half_extents.x = 0.5f * sx;
		bd.half_extents.y = 0.5f * sy;
	}

	if (bd.half_extents.x <= 0.0f) bd.half_extents.x = 0.5f;
	if (bd.shape != JCE_SHAPE2D_CIRCLE && bd.shape != JCE_SHAPE2D_SEGMENT &&
	    bd.half_extents.y <= 0.0f)
		bd.half_extents.y = 0.5f;

	JceBodyHandle body = jce_physics2d_body_create(rt->physics2d, &bd);
	if (!jce_body_valid(body)) return;

	/* Persist the body index back into the component (mirrors 3D contract). */
	rb->body_handle_idx = body.idx;

	if (rt->body2d_count >= rt->body2d_cap && !rt_grow_bodies2d(rt))
		return;
	Body2DEntry *be = &rt->bodies2d[rt->body2d_count];
	be->entity = e;
	be->body   = body;
	be->kind   = (uint8_t)bd.type;
	rt->body2d_count++;
}

/*
 * Spawn ONE static 2D body for entity `e` from its TilemapCollider2D +
 * Tilemap components: the .tilemap.json's solid cells (any non-zero id)
 * are greedy-merged into axis-aligned rectangles and attached as box
 * shapes to a single static Box2D body, so a large map costs a handful
 * of shapes instead of one per cell.
 *
 * Cell convention (mirrors the renderer): cell (col,row) spans entity-
 * local [col,col+1] x [-(row+1),-row], scaled by the entity's XY scale.
 *
 * `used_by_composite` is intentionally ignored: the greedy merge above
 * already IS the composite — there is no separate CompositeCollider2D
 * component to defer shape ownership to.
 */
static void rt_spawn_tilemap_collider2d(JceRuntime *rt, JceScene *scene,
                                        JceEntity e, const JceTransform *tf)
{
	JceTilemapCollider2DComponent *col =
		jce_scene_get_tilemap_collider2d(scene, e);
	JceTilemapComponent *tm = jce_scene_get_tilemap(scene, e);
	if (!col || !tm || !tm->tilemap_path[0]) return;

	/* The tilemap stays STATIC even when a dynamic RigidBody2D coexists
	 * on the entity (moving tilemap colliders are out of scope). */
	JceRigidBody2DComponent *rb = jce_scene_get_rigidbody2d(scene, e);
	if (rb && jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_RIGIDBODY_2D) &&
	    rb->body_type != JCE_BODY_STATIC && rb->body_type != JCE_BODY_KINEMATIC &&
	    rb->mass > 0.0f)
		LOG_WARN(LOG_TAG, "tilemap collider on entity %llu coexists with a "
		         "DYNAMIC RigidBody2D; the tilemap body stays static",
		         (unsigned long long)e);

	/* PAK-first (deployed bundles), then the loose file (editor Play). */
	JceTilemapAsset *map = rt->pak
		? jce_tilemap_load_from_pak(rt->pak, tm->tilemap_path) : NULL;
	if (!map) map = jce_tilemap_load_file(tm->tilemap_path);
	if (!map) {
		LOG_WARN(LOG_TAG, "tilemap collider: load failed '%s' (entity %llu)",
		         tm->tilemap_path, (unsigned long long)e);
		return;
	}

	JceTilemapSolidRect rects[JCE_TILEMAP_COL_MAX_RECTS];
	uint32_t total = jce_tilemap_solid_rects(map, rects,
	                                         JCE_TILEMAP_COL_MAX_RECTS);
	uint32_t n = total;
	if (n > JCE_TILEMAP_COL_MAX_RECTS) {
		LOG_WARN(LOG_TAG, "tilemap collider '%s': %u merged rects exceed "
		         "the %d cap; truncating", tm->tilemap_path, total,
		         JCE_TILEMAP_COL_MAX_RECTS);
		n = JCE_TILEMAP_COL_MAX_RECTS;
	}
	if (n == 0) {
		jce_tilemap_unload(map);
		return;
	}

	/* Same Z-angle quaternion recovery as rt_spawn_body2d. */
	float angle;
	{
		jce_quat q = tf->rotation;
		angle = atan2f(2.0f * (q.w * q.z + q.x * q.y),
		               1.0f - 2.0f * (q.y * q.y + q.z * q.z));
	}
	jce_vec2 pos;
	pos.x = tf->position.x + col->offset[0];
	pos.y = tf->position.y + col->offset[1];

	JceBodyHandle body = jce_physics2d_body_create_empty(rt->physics2d, pos,
	                                                     angle, JCE_BODY_STATIC);
	if (!jce_body_valid(body)) {
		jce_tilemap_unload(map);
		return;
	}

	jce_vec3 s2 = jce_v3_abs_safe_scale(tf->scale);
	float friction    = (float)col->friction_x100 / 100.0f;
	float restitution = (float)col->bounciness_x100 / 100.0f;

	for (uint32_t i = 0; i < n; i++) {
		jce_vec2 center, half;
		center.x =  ((float)rects[i].x + (float)rects[i].w * 0.5f) * s2.x;
		center.y = -((float)rects[i].y + (float)rects[i].h * 0.5f) * s2.y;
		half.x   = (float)rects[i].w * 0.5f * s2.x;
		half.y   = (float)rects[i].h * 0.5f * s2.y;
		jce_physics2d_body_add_box(rt->physics2d, body, center, half,
		                           friction, restitution, col->trigger);
	}

	/* Track like rt_spawn_body2d (static → no transform write-back). */
	if (rt->body2d_count < rt->body2d_cap || rt_grow_bodies2d(rt)) {
		Body2DEntry *be = &rt->bodies2d[rt->body2d_count];
		be->entity = e;
		be->body   = body;
		be->kind   = (uint8_t)JCE_BODY_STATIC;
		rt->body2d_count++;
	}

	LOG_INFO(LOG_TAG, "tilemap collider: %u box shapes for '%s' (entity %llu)",
	         n, tm->tilemap_path, (unsigned long long)e);
	jce_tilemap_unload(map);
}

/* Play a resolved sound for an audio source + track its voice.  Shared by
 * the synchronous (callback) path and the async upload path. */
static void rt_finish_audio_source(JceRuntime *rt, JceScene *scene,
                                   JceEntity e, JceSound snd,
                                   const JceAudioSourceComponent *as)
{
    if (snd == JCE_SOUND_INVALID || !as) return;

    float vol   = as->volume > 0.0f ? as->volume : 1.0f;
    float pitch = as->pitch  > 0.0f ? as->pitch  : 1.0f;
    JceVoice v = jce_audio_play(rt->audio, snd, as->loop, vol, pitch);
    bool spatial = (as->spatial_blend > 0.5f);
    if (spatial) {
        jce_audio_voice_set_3d(rt->audio, v, true);
        jce_vec3 wp = rt_world_position(scene, e);
        jce_audio_voice_set_position(rt->audio, v, wp.x, wp.y, wp.z);
        jce_audio_voice_set_attenuation(rt->audio, v,
                                        JCE_AUDIO_ATTEN_INVERSE,
                                        1.0f, 25.0f, 1.0f);
    } else {
        jce_audio_voice_set_3d(rt->audio, v, false);
    }
    const char *bus = rt_bus_for_source(rt, as, spatial);
    if (bus) jce_audio_voice_set_bus(rt->audio, v, bus);

    if (rt->voice_count >= rt->voice_cap && !rt_grow_voices(rt))
        return;
    rt->voices[rt->voice_count].entity      = e;
    rt->voices[rt->voice_count].sound       = snd;
    rt->voices[rt->voice_count].voice       = v;
    rt->voices[rt->voice_count].spatial     = spatial;
    rt->voices[rt->voice_count].base_volume = vol;
    rt->voices[rt->voice_count].bus[0]      = '\0';
    if (bus) {
        size_t bl = strlen(bus);
        if (bl >= sizeof(rt->voices[rt->voice_count].bus))
            bl = sizeof(rt->voices[rt->voice_count].bus) - 1;
        memcpy(rt->voices[rt->voice_count].bus, bus, bl);
        rt->voices[rt->voice_count].bus[bl] = '\0';
    }
    rt->voice_count++;
}

/* WORKER: decode a play_on_awake clip to CPU PCM (PAK + miniaudio). */
static void rt_audio_decode_run(void *arg)
{
    RtAudioDecodeArgs *a = (RtAudioDecodeArgs *)arg;
    a->cpu = jce_audio_decode_cpu(a->pak, a->path);
    jce_atomic_i32_store(a->done, 1);
}

static bool rt_grow_pending_audio(JceRuntime *rt)
{
    int new_cap = rt->pending_audio_cap ? rt->pending_audio_cap * 2 : 8;
    RtPendingAudio *grown = (RtPendingAudio *)jce_realloc(
        rt->pending_audio, (size_t)new_cap * sizeof(RtPendingAudio));
    if (!grown) return false;
    rt->pending_audio     = grown;
    rt->pending_audio_cap = new_cap;
    return true;
}

/* Kick an async decode of `as->clip_path` for entity `e` (default loader
 * path only).  The worker owns `args` (stable heap) for its full run; the
 * pending slot only references it, so the slot array may realloc freely. */
static void rt_spawn_audio_async(JceRuntime *rt, JceEntity e,
                                 const JceAudioSourceComponent *as)
{
    if (rt->pending_audio_count >= rt->pending_audio_cap &&
        !rt_grow_pending_audio(rt)) {
        /* Out of queue memory — fall back to a synchronous load. */
        JceSound snd = jce_audio_load(rt->audio, rt->pak, as->clip_path);
        rt_finish_audio_source(rt, rt->scene, e, snd, as);
        return;
    }

    RtAudioDecodeArgs *args = (RtAudioDecodeArgs *)jce_malloc(sizeof(*args));
    if (!args) return;
    args->pak = rt->pak;
    snprintf(args->path, sizeof(args->path), "%s", as->clip_path);
    args->cpu  = NULL;
    args->done = jce_atomic_i32_create(0);

    JceThread *thr = jce_thread_create(rt_audio_decode_run, args, "jce_rt_audio");
    if (!thr) {
        /* No worker thread: decode + play inline, then drop the job. */
        rt_audio_decode_run(args);
        JceSound snd = jce_audio_upload_cpu(rt->audio, args->cpu);
        rt_finish_audio_source(rt, rt->scene, e, snd, as);
        if (args->done) jce_atomic_i32_destroy(args->done);
        jce_free(args);
        return;
    }

    RtPendingAudio *p = &rt->pending_audio[rt->pending_audio_count++];
    p->entity = e;
    p->thr    = thr;
    p->args   = args;
}

/* MAIN thread, per-frame: upload + play any finished async audio decodes. */
static void rt_audio_poll(JceRuntime *rt)
{
    if (!rt || rt->pending_audio_count == 0) return;
    int w = 0;
    for (int i = 0; i < rt->pending_audio_count; ++i) {
        RtPendingAudio *p = &rt->pending_audio[i];
        if (!p->args || jce_atomic_i32_load(p->args->done) == 0) {
            rt->pending_audio[w++] = *p;   /* keep (still running) */
            continue;
        }
        if (p->thr) { jce_thread_join(p->thr); p->thr = NULL; }

        /* Re-fetch the component at play time (the entity may have moved /
         * been disabled in the 1-2 frames since spawn). */
        JceAudioSourceComponent *as =
            jce_scene_get_audio_source(rt->scene, p->entity);
        if (as &&
            jce_scene_component_enabled(rt->scene, p->entity,
                                        JCE_COMP_FLAG_AUDIO_SOURCE)) {
            JceSound snd = jce_audio_upload_cpu(rt->audio, p->args->cpu);
            p->args->cpu = NULL;   /* consumed by upload */
            rt_finish_audio_source(rt, rt->scene, p->entity, snd, as);
        } else {
            jce_audio_cpu_free(p->args->cpu);   /* source gone — drop it */
            p->args->cpu = NULL;
        }
        jce_atomic_i32_destroy(p->args->done);
        jce_free(p->args);
        /* slot dropped (not copied to w) */
    }
    rt->pending_audio_count = w;
}

static void rt_spawn_entity(JceScene *scene, JceEntity e, void *ud)
{
	JceRuntime *rt = (JceRuntime *)ud;
	if (!jce_scene_has_transform(scene, e)) return;
	JceTransform *tf = jce_scene_get_transform(scene, e);
	if (!tf) return;

	/* ── Character controller (only one supported per scene) ── */
	if (rt->physics && !jce_character_valid(rt->character)) {
		JceCharacterControllerComponent *cc =
			jce_scene_get_character_controller(scene, e);
		if (cc && jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_CHARACTER_CONTROLLER)) {
			JceCharacterDesc cd;
			memset(&cd, 0, sizeof cd);
			cd.position      = tf->position;
			cd.radius        = cc->radius      > 0.0f ? cc->radius      : 0.35f;
			cd.height        = cc->height      > 0.0f ? cc->height      : 1.8f;
			cd.step_height   = cc->step_offset > 0.0f ? cc->step_offset : 0.35f;
			cd.max_slope_deg = cc->slope_limit > 0.0f ? cc->slope_limit : 50.0f;
			cd.gravity       = 9.81f;
			cd.jump_speed    = cc->jump_speed  > 0.0f ? cc->jump_speed  : 5.0f;
			cd.accel         = cc->accel;        /* 0 → physics default */
			cd.air_control   = cc->air_control;
			/* The scene Transform is the FEET (feet-pivoted meshes), but the
			 * Bullet capsule is centered on its origin — spawn the CENTER half
			 * a height above the feet so it rests instead of sinking on frame 0. */
			cd.position.y   += 0.5f * cd.height;
			rt->character = jce_physics_character_create(rt->physics, &cd);
			if (jce_character_valid(rt->character)) {
				rt->character_entity      = e;
				rt->character_half_height = 0.5f * cd.height;
				rt->char_cur_pos          = cd.position;   /* capsule center */
				rt->char_prev_pos         = cd.position;
				rt->char_last_pos         = tf->position;  /* feet */
				rt->char_last_rot         = tf->rotation;
				rt->char_move_speed  = cc->move_speed  > 0.0f ? cc->move_speed  : 4.0f;
				rt->char_sprint_mult = cc->sprint_mult > 0.0f ? cc->sprint_mult : 1.8f;
				rt->char_turn_speed  = (cc->turn_speed_deg > 0.0f
				                        ? cc->turn_speed_deg : 720.0f) * JCE_DEG2RAD;
				rt->char_yaw_valid   = false;
				rt->char_coyote_t    = 0.0f;
				rt->char_jump_buf_t  = 0.0f;
				rt->char_jump_was_held = false;
			}
			/* Character takes priority — skip rigid body for this entity. */
			goto try_audio;
		}
	}

	/* ── Compound collider (per-object cooked) takes priority ── */
	if (rt->physics && rt_try_spawn_compound(rt, scene, e, tf)) {
		if (jce_scene_has_mesh_collider(scene, e))
			LOG_WARN(LOG_TAG, "entity %llu has both Compound and Mesh "
			         "colliders; compound wins, mesh collider ignored",
			         (unsigned long long)e);
		goto try_audio;
	}

	/* ── Mesh collider (single cooked hull / triangle mesh) ── */
	if (rt->physics && rt_try_spawn_mesh(rt, scene, e, tf))
		goto try_audio;

	/* ── Rigid body ── */
	if (rt->physics) {
		JceRigidBodyComponent *rb = jce_scene_get_rigidbody(scene, e);
		if (rb && jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_RIGIDBODY)) {
			JceBodyDesc bd;
			memset(&bd, 0, sizeof bd);
			bd.position        = tf->position;
			bd.rotation        = tf->rotation;
			bd.mass            = rb->mass;
			bd.linear_damping  = rb->drag;
			bd.angular_damping = rb->angular_drag;
			bd.friction        = rb->friction    > 0.0f ? rb->friction    : 0.5f;
			bd.restitution     = rb->restitution;

			JceBoxColliderComponent     *box = jce_scene_get_box_collider(scene, e);
			JceSphereColliderComponent  *sph = jce_scene_get_sphere_collider(scene, e);
			JceCapsuleColliderComponent *cap = jce_scene_get_capsule_collider(scene, e);
			if (cap && !jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_CAPSULE_COLLIDER))
				cap = NULL;
			if (box) {
				bd.shape = JCE_SHAPE_BOX;
				bd.half_extents.x = 0.5f * box->size[0] * tf->scale.x;
				bd.half_extents.y = 0.5f * box->size[1] * tf->scale.y;
				bd.half_extents.z = 0.5f * box->size[2] * tf->scale.z;
				if (bd.half_extents.x <= 0.0f) bd.half_extents.x = 0.5f;
				if (bd.half_extents.y <= 0.0f) bd.half_extents.y = 0.5f;
				if (bd.half_extents.z <= 0.0f) bd.half_extents.z = 0.5f;
				/* Local center: scale by TRS scale, then rotate by TRS rotation
				 * (matches the editor collider overlay; was a raw world add). */
				{
					jce_vec3 ofs = jce_v3(box->center[0] * tf->scale.x,
					                      box->center[1] * tf->scale.y,
					                      box->center[2] * tf->scale.z);
					ofs = jce_q_rotate(tf->rotation, ofs);
					bd.position = jce_v3_add(bd.position, ofs);
				}
				bd.is_trigger = box->is_trigger;
			} else if (sph) {
				bd.shape = JCE_SHAPE_SPHERE;
				float smax = tf->scale.x;
				if (tf->scale.y > smax) smax = tf->scale.y;
				if (tf->scale.z > smax) smax = tf->scale.z;
				float r = sph->radius > 0.0f ? sph->radius : 0.5f;
				bd.half_extents.x = r * smax;
				{
					jce_vec3 sofs = jce_v3(sph->center[0] * tf->scale.x,
					                       sph->center[1] * tf->scale.y,
					                       sph->center[2] * tf->scale.z);
					sofs = jce_q_rotate(tf->rotation, sofs);
					bd.position = jce_v3_add(bd.position, sofs);
				}
				bd.is_trigger = sph->is_trigger;
			} else if (cap) {
				/* Bullet capsules are Y-aligned: half_extents = (radius,
				 * cylinder half-height, 0).  Mirror the editor collider
				 * overlay (jce_scene_render_draw.cpp capsule block) so
				 * draw == physics: radius scales by max(|sx|,|sz|), total
				 * height by |sy|, hemispheres carved out of the authored
				 * total height.  `axis` is intentionally ignored — the
				 * overlay draws Y-aligned too, and JceBodyDesc has no
				 * per-shape axis (only whole-body rotation). */
				bd.shape = JCE_SHAPE_CAPSULE;
				jce_vec3 cs = jce_v3_abs_safe_scale(tf->scale);
				float cr_scale = cs.x > cs.z ? cs.x : cs.z;
				float cr = (cap->radius > 0.0f ? cap->radius : 0.3f) * cr_scale;
				float ch = (cap->height > 0.0f ? cap->height : 1.0f) * cs.y;
				float chh = 0.5f * (ch - 2.0f * cr);
				if (chh < 0.0f) chh = 0.0f;
				bd.half_extents.x = cr;
				bd.half_extents.y = chh;
				/* Local center: scale by TRS scale, then rotate by TRS
				 * rotation (same as the box/sphere branches above). */
				{
					jce_vec3 cofs = jce_v3(cap->center[0] * tf->scale.x,
					                       cap->center[1] * tf->scale.y,
					                       cap->center[2] * tf->scale.z);
					cofs = jce_q_rotate(tf->rotation, cofs);
					bd.position = jce_v3_add(bd.position, cofs);
				}
				bd.is_trigger = cap->is_trigger;
			} else {
				/* No collider authored — placeholder box from transform
				 * scale so dropped objects still collide. */
				bd.shape = JCE_SHAPE_BOX;
				bd.half_extents.x = 0.5f * tf->scale.x;
				bd.half_extents.y = 0.5f * tf->scale.y;
				bd.half_extents.z = 0.5f * tf->scale.z;
				if (bd.half_extents.x <= 0.0f) bd.half_extents.x = 0.5f;
				if (bd.half_extents.y <= 0.0f) bd.half_extents.y = 0.5f;
				if (bd.half_extents.z <= 0.0f) bd.half_extents.z = 0.5f;
			}

			if (rb->is_kinematic)       bd.type = JCE_BODY_KINEMATIC;
			else if (rb->mass <= 0.0f)  bd.type = JCE_BODY_STATIC;
			else                        bd.type = JCE_BODY_DYNAMIC;

			JceBodyHandle body = jce_physics_body_create(rt->physics, &bd);
			if (jce_body_valid(body)) {
				if (rb->ccd_mode != JCE_CCD_DISCRETE) {
					jce_physics_body_set_ccd_mode(rt->physics, body,
					                              (JceCcdMode)rb->ccd_mode);
					if (rb->ccd_threshold > 0.0f)
						jce_physics_body_set_ccd_motion_threshold(
							rt->physics, body, rb->ccd_threshold);
					if (rb->ccd_sphere_radius > 0.0f)
						jce_physics_body_set_ccd_swept_sphere_radius(
							rt->physics, body, rb->ccd_sphere_radius);
				}
				rt_apply_body_extras(rt, e, body, rb, rb->physmat_path);
				if (rt->body_count >= rt->body_cap && !rt_grow_bodies(rt))
					goto try_audio;
				rt_track_body(rt, e, body, tf, (uint8_t)bd.type);
			}
		}
	}

	/* ── 2D rigid body (Box2D, independent world) ── */
	if (rt->physics2d &&
	    jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_RIGIDBODY_2D))
		rt_spawn_body2d(rt, scene, e, tf);

	/* ── Tilemap 2D collider (one static body, greedy-merged boxes) ── */
	if (rt->physics2d &&
	    jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_TILEMAP_COLLIDER_2D) &&
	    jce_scene_has_tilemap(scene, e))
		rt_spawn_tilemap_collider2d(rt, scene, e, tf);

try_audio:
	/* ── Audio source ── */
	if (rt->audio && (rt->pak || rt->audio_load_fn)) {
		JceAudioSourceComponent *as = jce_scene_get_audio_source(scene, e);
		if (as && as->play_on_awake && as->clip_path[0] &&
		    jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_AUDIO_SOURCE)) {
			if (rt->audio_load_fn) {
				/* Custom loader hook (editor): keep the synchronous handoff. */
				JceSound snd = rt->audio_load_fn(rt->user_data, rt->audio,
				                                 as->clip_path);
				if (snd != JCE_SOUND_INVALID)
					rt_finish_audio_source(rt, scene, e, snd, as);
				else
					jce_log_write(JCE_LOG_LEVEL_WARN, LOG_TAG, __FILE__, __LINE__,
					              "audio_source: failed to load '%s'", as->clip_path);
			} else {
				/* Default PAK loader: decode off-thread so a scene full of
				 * play_on_awake clips doesn't stall scene-load.  The sound
				 * starts a frame or two late (rt_audio_poll plays it). */
				rt_spawn_audio_async(rt, e, as);
			}
		}
	}
}

/* ── Gameplay bridge (second walk: triggers / spawners / weapons) ─────
 *
 * Mirror authored gameplay POD components into their runtime subsystems.
 * Runs after the physics/audio walk so it can rely on transforms being
 * present.  Each subsystem is created lazily on first matching component
 * so a scene with none of them allocates nothing. */
static void rt_spawn_gameplay(JceScene *scene, JceEntity e, void *ud)
{
	JceRuntime *rt = (JceRuntime *)ud;
	if (!jce_scene_has_transform(scene, e)) return;

	/* ── Trigger volume ── */
	JceTriggerVolumeComponent *tvc = jce_scene_get_trigger_volume(scene, e);
	if (tvc && jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_TRIGGER_VOLUME)) {
		if (!rt->trigger_world) {
			rt->trigger_world = jce_trigger_world_create();
			if (rt->trigger_world)
				jce_trigger_world_set_event_fn(rt->trigger_world,
				                               rt_trigger_event, rt);
		}
		if (rt->trigger_world) {
			jce_vec3 wp = rt_world_position(scene, e);
			JceTriggerDesc d;
			memset(&d, 0, sizeof d);
			d.shape = (tvc->shape == JCE_TRIGGER_VOL_SPHERE) ? JCE_TRIGGER_SPHERE
			        : (tvc->shape == JCE_TRIGGER_VOL_OBB)    ? JCE_TRIGGER_OBB
			                                                 : JCE_TRIGGER_AABB;
			d.center = jce_v3(wp.x + tvc->center[0],
			                  wp.y + tvc->center[1],
			                  wp.z + tvc->center[2]);
			d.half_extents = jce_v3(tvc->half_extents[0],
			                        tvc->half_extents[1],
			                        tvc->half_extents[2]);
			d.axis_x = jce_v3(tvc->axis_x[0], tvc->axis_x[1], tvc->axis_x[2]);
			d.axis_y = jce_v3(tvc->axis_y[0], tvc->axis_y[1], tvc->axis_y[2]);
			d.axis_z = jce_v3(tvc->axis_z[0], tvc->axis_z[1], tvc->axis_z[2]);
			JceTriggerHandle h = jce_trigger_add(rt->trigger_world, &d,
			                                     (uint64_t)e);
			jce_trigger_set_enabled(rt->trigger_world, h, tvc->enabled);
			if (jce_trigger_valid(h) &&
			    (rt->trigger_count < rt->trigger_cap || rt_grow_triggers(rt))) {
				rt->triggers[rt->trigger_count].entity = e;
				rt->triggers[rt->trigger_count].handle = h;
				rt->trigger_count++;
			}
		}
	}

	/* ── Spawn manager ── */
	JceSpawnManagerComponent *smc = jce_scene_get_spawn_manager(scene, e);
	if (smc && smc->enabled) {
		if (rt->spawn_count < rt->spawn_cap || rt_grow_spawns(rt)) {
			SpawnEntry *se = &rt->spawns[rt->spawn_count];
			se->entity     = e;
			JceSpawnManagerDesc sd;
			memset(&sd, 0, sizeof sd);
			sd.max_peds         = (uint32_t)(smc->max_peds > 0 ? smc->max_peds : 0);
			sd.max_vehicles     = (uint32_t)(smc->max_vehicles > 0 ? smc->max_vehicles : 0);
			sd.min_spawn_radius = smc->min_spawn_radius;
			sd.max_spawn_radius = smc->max_spawn_radius > 0.0f ? smc->max_spawn_radius : 50.0f;
			sd.despawn_pad      = smc->despawn_pad;
			sd.spawn_interval   = smc->spawn_interval > 0.0f ? smc->spawn_interval : 1.0f;
			sd.road_network     = NULL;   /* vehicles need a road graph — skipped */
			sd.on_create        = rt_spawn_create;
			sd.on_destroy       = rt_spawn_destroy;
			sd.ped_sampler      = NULL;   /* no navmesh sampler in runtime — peds skipped */
			sd.user             = &rt->spawn_cookie_seq;
			sd.rng_seed         = smc->rng_seed;
			if (smc->ped_archetype_count > 0) {
				sd.ped_archetypes      = smc->ped_archetypes;
				sd.ped_archetype_count = (uint32_t)smc->ped_archetype_count;
			}
			if (smc->vehicle_archetype_count > 0) {
				sd.vehicle_archetypes      = smc->vehicle_archetypes;
				sd.vehicle_archetype_count = (uint32_t)smc->vehicle_archetype_count;
			}
			se->mgr = jce_spawn_manager_create(&sd);
			if (se->mgr) rt->spawn_count++;
		}
	}

	/* ── Weapon ── */
	JceWeaponComponent *wc = jce_scene_get_weapon(scene, e);
	if (wc) {
		if (rt->weapon_count < rt->weapon_cap || rt_grow_weapons(rt)) {
			WeaponEntry *we = &rt->weapons[rt->weapon_count];
			we->entity = e;
			we->rng    = (uint64_t)e * 0x9E3779B97F4A7C15ull + 1u;
			JceWeaponArchetype *a = &we->arch;
			memset(a, 0, sizeof *a);
			a->name             = wc->name[0] ? wc->name : "weapon";
			a->kind             = (wc->kind == JCE_WEAPON_COMP_PROJECTILE)
			                      ? JCE_WEAPON_KIND_PROJECTILE
			                      : JCE_WEAPON_KIND_HITSCAN;
			a->damage           = wc->damage;
			a->range            = wc->range;
			a->rpm              = wc->rpm > 0.0f ? wc->rpm : 600.0f;
			a->clip_size        = wc->clip_size > 0 ? wc->clip_size : 1;
			a->reserve_max      = wc->reserve_max;
			a->reload_seconds   = wc->reload_seconds;
			a->spread_deg       = wc->spread_deg;
			a->recoil_per_shot  = wc->recoil_per_shot;
			a->recoil_recovery  = wc->recoil_recovery;
			a->pellets          = wc->pellets > 0 ? wc->pellets : 1;
			a->projectile_speed = wc->projectile_speed;
			a->full_auto        = wc->full_auto;
			jce_weapon_instance_init(&we->inst, a);
			rt->weapon_count++;
		}
	}

	/* ── Save point (P2-save-snapshot) ──
	 * Mirror an authored SavePoint as a sphere trigger in the SAME trigger
	 * world rt_tick_gameplay already drives, so the player observer's overlap
	 * (rt_trigger_event ENTER) writes a snapshot.  Reuses the lazy trigger-
	 * world creation from the trigger-volume block above. */
	JceSavePointComponent *spc = jce_scene_get_save_point(scene, e);
	if (spc) {
		if (!rt->trigger_world) {
			rt->trigger_world = jce_trigger_world_create();
			if (rt->trigger_world)
				jce_trigger_world_set_event_fn(rt->trigger_world,
				                               rt_trigger_event, rt);
		}
		if (rt->trigger_world &&
		    (rt->save_point_count < rt->save_point_cap ||
		     rt_grow_save_points(rt))) {
			jce_vec3 wp = rt_world_position(scene, e);
			float r = spc->radius > 0.0f ? spc->radius : 1.5f;
			JceTriggerDesc d;
			memset(&d, 0, sizeof d);
			d.shape          = JCE_TRIGGER_SPHERE;
			d.center         = wp;
			d.half_extents.x = r;   /* sphere: half_extents.x = radius */
			JceTriggerHandle h = jce_trigger_add(rt->trigger_world, &d,
			                                     (uint64_t)e);
			if (jce_trigger_valid(h)) {
				SavePointEntry *sp = &rt->save_points[rt->save_point_count];
				sp->entity   = e;
				sp->handle   = h;
				sp->one_shot = spc->one_shot;
				sp->require_interact = spc->require_interact;
				sp->fired    = false;
				size_t idn = strlen(spc->save_id);
				if (idn >= sizeof sp->save_id) idn = sizeof sp->save_id - 1;
				memcpy(sp->save_id, spc->save_id, idn);
				sp->save_id[idn] = '\0';
				rt->save_point_count++;
			}
		}
	}

	/* ── Behavior tree (P2-perception-bt-binding) ──
	 * Load the authored tree file into the runtime BT context and record a
	 * BtEntry with a fresh per-agent blackboard.  Perception writes stimuli
	 * into that blackboard each frame and rt_tick_gameplay ticks the tree.
	 * Tree load is host-filesystem (jce_bt_load_tree_file); a pak-resident
	 * tree would need the buffer path — noted as a follow-up. */
	JceBehaviorTree *btc = jce_scene_get_behavior_tree(scene, e);
	if (btc && btc->active && btc->tree_path[0] && rt->bt_ctx &&
	    jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_BEHAVIOR_TREE)) {
		rt_bt_register_default_actions(rt);
		JceBtTreeHandle h = jce_bt_load_tree_file(rt->bt_ctx, btc->tree_path);
		if (jce_bt_tree_valid(h) &&
		    (rt->bt_count < rt->bt_cap || rt_grow_bts(rt))) {
			struct BtEntry *be = &rt->bts[rt->bt_count];
			memset(be, 0, sizeof *be);
			be->entity           = e;
			be->tree             = h;
			be->bb               = jce_blackboard_create();
			/* Per-agent sight/hearing authored on the BehaviorTree component;
			 * a value <=0 falls back to the engine default (also what scenes
			 * authored before these fields existed will have). */
			be->sight_range      = (btc->sight_range      > 0.0f) ? btc->sight_range      : 25.0f;
			be->sight_half_angle = (btc->sight_half_angle > 0.0f) ? btc->sight_half_angle : 1.0472f; /* 60° */
			be->hearing_range    = (btc->hearing_range    > 0.0f) ? btc->hearing_range    : 15.0f;
			be->tick_period      = (btc->tick_hz > 0.0f) ? (1.0f / btc->tick_hz) : 0.0f;
			be->tick_accum       = 0.0f;
			be->active           = true;
			/* Mirror the assigned handle back onto the component so the
			 * inspector / save reflects the loaded tree. */
			btc->tree_handle_idx = h.idx;
			rt->bt_count++;
		} else if (!jce_bt_tree_valid(h)) {
			LOG_WARN(LOG_TAG, "behavior tree: failed to load '%s' for entity %llu",
			         btc->tree_path, (unsigned long long)e);
		}
	}

	/* ── Nav agent (P1-navmesh-chain) ──
	 * Mirror an authored NavAgent into the runtime agent set (stood up by
	 * rt_init_navmesh BEFORE this walk).  The component stays the authored
	 * source of truth; the entry caches the engine handle plus the last goal
	 * issued so rt_tick_gameplay can honour live edits and auto_repath. */
	JceNavAgentComponent *nac = jce_scene_get_nav_agent(scene, e);
	if (nac && nac->enabled && rt->nav_agents) {
		if (rt->nav_entry_count < rt->nav_entry_cap || rt_grow_nav_entries(rt)) {
			jce_vec3 wp = rt_world_position(scene, e);
			JceNavAgentDesc d = {
				.pos_x           = wp.x,
				.pos_z           = wp.z,
				.radius          = nac->radius          > 0.0f ? nac->radius          : 0.4f,
				.max_speed       = nac->max_speed       > 0.0f ? nac->max_speed       : 3.0f,
				.max_accel       = nac->max_accel       > 0.0f ? nac->max_accel       : 12.0f,
				.arrive_radius   = nac->arrive_radius   > 0.0f ? nac->arrive_radius   : 1.5f,
				.waypoint_radius = nac->waypoint_radius > 0.0f ? nac->waypoint_radius : 0.5f,
			};
			JceNavAgentHandle h = jce_nav_agent_add(rt->nav_agents, &d);
			if (jce_nav_agent_valid(h)) {
				NavAgentEntry *ne = &rt->nav_entries[rt->nav_entry_count];
				ne->entity = e;
				ne->handle = h;
				/* Initial destination: a referenced target entity's world
				 * position when set, otherwise the authored target point. */
				float gx, gz;
				if (nac->target_entity != 0) {
					jce_vec3 tp = rt_world_position(scene,
					                                (JceEntity)nac->target_entity);
					gx = tp.x; gz = tp.z;
				} else {
					gx = nac->target[0]; gz = nac->target[2];
				}
				ne->has_dest    = jce_nav_agent_set_destination(rt->nav_agents,
				                                                h, gx, gz);
				ne->last_goal_x = gx;
				ne->last_goal_z = gz;
				rt->nav_entry_count++;
			}
		}
	}
}

/* ── Networking bridge (P1-networking-full) ──────────────────────────
 *
 * Walk authored JceNetworkObject entities and wire them into the net
 * runtime: adopt the existing entity into the replication NetObject table
 * (server only — clients learn objects from the spawn stream), then, if a
 * JceNetTransform override is present, register the object's transform
 * with the snapshot-interp module using the authored sync rate / interp /
 * tolerance / authority.  Runs only when a session is live; a no-op for
 * single-player / session-less play. */
static void rt_spawn_net(JceScene *scene, JceEntity e, void *ud)
{
	JceRuntime *rt = (JceRuntime *)ud;
	if (!jce_scene_has_transform(scene, e)) return;

	JceNetworkObjectComponent *no = jce_scene_get_network_object(scene, e);
	if (!no) return;

	/* Server adopts the authored entity → assigns a net id + queues a
	 * spawn broadcast.  Clients do not adopt: they receive the spawn from
	 * the server and bind their own local entity. */
	JceNetObjectId id = JCE_NET_OBJECT_INVALID;
	if (jce_session_is_server()) {
		JceClientId owner = (JceClientId)no->owner;
		id = jce_net_object_adopt((uint64_t)e, owner, no->flags, NULL);
		if (id != JCE_NET_OBJECT_INVALID) {
			/* Mirror the assigned id back onto the authored component so
			 * gameplay systems can read net_id without a net round-trip. */
			no->net_id   = id;
			no->is_owner = (owner == jce_net_local_client_id());
			rt->net_obj_count++;
		}
	}

	/* Transform replication config from the authored override (if any). */
	JceNetTransformComponent *nt = jce_scene_get_net_transform(scene, e);
	if (nt && id != JCE_NET_OBJECT_INVALID) {
		JceNetTransformConfig cfg;
		jce_net_transform_get_default_config(&cfg);
		if (nt->sync_rate_hz) cfg.snapshot_hz = (uint32_t)nt->sync_rate_hz;
		if (nt->interp_ms)    cfg.interp_delay_ms = (uint32_t)nt->interp_ms;
		if (nt->tolerance > 0.0f)
			cfg.divergence_snap_distance = nt->tolerance;
		cfg.authority = (nt->authority_mode == 1)
		                ? JCE_NET_AUTH_OWNER : JCE_NET_AUTH_SERVER;
		jce_net_transform_register(id, &cfg);
	}
}

/* Bring the networking bridge up at create() time when a session exists.
 * Binds the ECS world + scene into the net subsystems and walks net
 * objects.  Idempotent: no-op without a live session. */
static void rt_init_net_bridge(JceRuntime *rt)
{
	if (jce_session_mode() == JCE_SESSION_MODE_NONE) return;
	if (!rt->scene) return;

	/* Plumb the flecs world into replication + bind the scene into the
	 * transform module so it can read / write entity transforms. */
	jce_net_replication_set_world(jce_scene_get_world(rt->scene));
	jce_net_transform_set_scene(rt->scene);

	jce_scene_each_entity(rt->scene, rt_spawn_net, rt);
	rt->net_bridged = true;

	jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
	              "runtime: net bridge -> role=%s objects=%d xforms=%u interest=%.1fm",
	              jce_session_is_server() ? "server" : "client",
	              rt->net_obj_count,
	              jce_net_transform_registered_count(),
	              (double)jce_net_replication_get_interest_radius());
}

/* ── Joints / constraints (second pass — needs both bodies spawned) ── */

static JceBodyHandle rt_body_for_entity(const JceRuntime *rt, JceEntity e)
{
	for (int i = 0; i < rt->body_count; ++i)
		if (rt->bodies[i].entity == e) return rt->bodies[i].body;
	return JCE_BODY_INVALID;
}

static void rt_spawn_joint(JceScene *scene, JceEntity e, void *ud)
{
	JceRuntime *rt = (JceRuntime *)ud;
	if (!rt->physics) return;

	JceConstraintComponent *cn = jce_scene_get_constraint(scene, e);
	if (!cn) return;
	if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_CONSTRAINT)) return;

	/* The constrained entity itself must have a body. */
	JceBodyHandle a = rt_body_for_entity(rt, e);
	if (!jce_body_valid(a)) return;

	/* target_entity 0 -> anchor to the world; otherwise resolve its body. */
	JceBodyHandle b = JCE_BODY_INVALID;
	if (cn->target_entity != 0) {
		b = rt_body_for_entity(rt, (JceEntity)cn->target_entity);
		if (!jce_body_valid(b)) {
			LOG_WARN(LOG_TAG, "constraint: target entity %u has no body",
			         cn->target_entity);
			return;
		}
	}

	JceConstraintDesc cd;
	memset(&cd, 0, sizeof cd);
	cd.type              = (JceConstraintType)cn->constraint_type;
	cd.body_a            = a;
	cd.body_b            = b;   /* JCE_BODY_INVALID => world anchor */
	cd.pivot_a           = jce_v3(cn->pivot_a[0], cn->pivot_a[1], cn->pivot_a[2]);
	cd.pivot_b           = jce_v3(cn->pivot_b[0], cn->pivot_b[1], cn->pivot_b[2]);
	cd.axis              = jce_v3(cn->axis[0], cn->axis[1], cn->axis[2]);
	cd.lower_limit       = cn->lower_limit;
	cd.upper_limit       = cn->upper_limit;
	cd.disable_collision = cn->disable_collision;

	/* The Bullet world owns the constraint and frees it on destroy. */
	jce_physics_constraint_create(rt->physics, &cd);
}

/* ── Per-frame ────────────────────────────────────────────────────── */

/* First clip whose name contains `want` (case-insensitive), or -1. */
static int rt_clip_by_name(const JceSkeletalAnimatorComponent *sa, const char *want)
{
	if (!sa) return -1;
	for (int i = 0; i < sa->clip_count && i < 8; i++) {
		for (const char *p = sa->clip_names[i]; *p; p++) {
			int k = 0;
			while (want[k] &&
			       tolower((unsigned char)p[k]) == tolower((unsigned char)want[k]))
				k++;
			if (!want[k]) return i;
		}
	}
	return -1;
}

static void rt_drive_character(JceRuntime *rt, float dt)
{
	if (!rt->physics || !jce_character_valid(rt->character)) return;
	if (dt <= 0.0f) dt = 1.0f / 60.0f;

	/* Direction (clamped to unit length) × authored speed: movement feel
	 * comes from the CharacterController component, not the caller.
	 * speed_mult stays an extra gameplay multiplier (crouch, slow zones). */
	float dx = rt->input.walk_x, dz = rt->input.walk_z;
	float dl = sqrtf(dx * dx + dz * dz);
	if (dl > 1.0f) { dx /= dl; dz /= dl; dl = 1.0f; }
	float m = rt->input.speed_mult > 0.0f ? rt->input.speed_mult : 1.0f;
	if (rt->input.sprint) m *= rt->char_sprint_mult;
	float    speed = rt->char_move_speed * m;
	jce_vec3 walk  = jce_v3(dx * speed, 0.0f, dz * speed);
	jce_physics_character_move(rt->physics, rt->character, walk, dt);

	/* Jump with buffering + coyote time: a press up to JUMP_BUFFER s early
	 * still fires on landing, and stepping off a ledge keeps a JUMP_COYOTE
	 * grace window — both standard platformer forgiveness mechanics. */
	const float JUMP_BUFFER = 0.12f, JUMP_COYOTE = 0.12f;
	bool grounded = jce_physics_character_is_grounded(rt->physics, rt->character);
	rt->char_coyote_t = grounded ? JUMP_COYOTE : rt->char_coyote_t - dt;
	if (rt->input.jump_pressed) {
		rt->char_jump_buf_t    = JUMP_BUFFER;
		rt->input.jump_pressed = false;
	} else if (rt->char_jump_buf_t > 0.0f) {
		rt->char_jump_buf_t -= dt;
	}
	bool jumped = false;
	if (rt->char_jump_buf_t > 0.0f && (grounded || rt->char_coyote_t > 0.0f)) {
		/* Only consume the buffer / pulse the anim trigger when the jump
		 * actually fired (the physics layer refuses mid-ascent repeats). */
		if (jce_physics_character_jump(rt->physics, rt->character)) {
			rt->char_jump_buf_t = 0.0f;
			rt->char_coyote_t   = 0.0f;
			jumped = true;
		}
	}
	/* Variable jump height: releasing jump while ascending cuts the rest
	 * of the rise, so taps hop and holds clear the full arc. */
	if (rt->char_jump_was_held && !rt->input.jump_held && !grounded)
		jce_physics_character_cut_jump(rt->physics, rt->character, 0.45f);
	rt->char_jump_was_held = rt->input.jump_held;

	/* Generic locomotion (engine-level, so it works in Play AND shipped):
	 * turn the character toward its move direction and feed the live
	 * physics state to whatever animation driver the entity carries. */
	if (rt->scene && rt->character_entity != 0) {
		JceTransform *tf = jce_scene_get_transform(rt->scene, rt->character_entity);
		jce_vec3 vel = jce_v3(0.0f, 0.0f, 0.0f);
		jce_physics_character_get_velocity(rt->physics, rt->character, &vel);
		float plan_speed = sqrtf(vel.x * vel.x + vel.z * vel.z);

		/* Smooth shortest-arc turn at the authored turn speed (the old
		 * instant snap reads as robotic with a real model). */
		if (tf && dl > 0.0001f) {
			if (!rt->char_yaw_valid) {
				jce_vec3 fwd = jce_q_rotate(tf->rotation, jce_v3(0.0f, 0.0f, 1.0f));
				rt->char_yaw       = atan2f(fwd.x, fwd.z);
				rt->char_yaw_valid = true;
			}
			float target = atan2f(dx, dz);
			float diff   = target - rt->char_yaw;
			while (diff >  JCE_PI) diff -= 2.0f * JCE_PI;
			while (diff < -JCE_PI) diff += 2.0f * JCE_PI;
			float max_turn = rt->char_turn_speed * dt;
			if (diff >  max_turn) diff =  max_turn;
			if (diff < -max_turn) diff = -max_turn;
			rt->char_yaw += diff;
			tf->rotation = jce_q_from_axis_angle(jce_v3(0.0f, 1.0f, 0.0f),
			                                     rt->char_yaw);
			rt->char_last_rot = tf->rotation;  /* our own yaw, not a gizmo edit */
		}
		if (jce_scene_has_skeletal_animator(rt->scene, rt->character_entity)) {
			JceSkeletalAnimatorComponent *sa =
				jce_scene_get_skeletal_animator(rt->scene, rt->character_entity);
			/* Live locomotion state for the renderer's SM / blend-tree
			 * driver (consumed when auto_speed is set): physics velocity is
			 * steadier than the transform-derived estimate, and grounded /
			 * vertical-velocity / jump unlock Jump+Fall+Land SM states. */
			sa->loco_valid    = true;
			sa->loco_grounded = grounded;
			sa->loco_speed    = plan_speed;
			sa->loco_vert_vel = vel.y;
			if (jumped) sa->loco_jump = true;   /* one-shot, consumer clears */
			/* If an animation state machine is bound it OWNS active_clip
			 * (driven by the engine "Speed" param in the renderer). Setting
			 * the clip by name here too would fight the SM every frame and
			 * make it flicker — only name-drive when there is no SM. */
			if (!sa->sm_path[0]) {
				int idx = -1;
				if (!grounded && rt->char_coyote_t <= 0.0f) {
					/* Airborne: prefer a jump/fall clip when the model has
					 * one; otherwise keep the current ground clip. */
					idx = rt_clip_by_name(sa, vel.y > 0.5f ? "jump" : "fall");
					if (idx < 0) idx = rt_clip_by_name(sa, "jump");
				} else {
					const char *want =
					    (plan_speed <= 0.15f) ? "idle"
					  : (plan_speed > rt->char_move_speed * 1.15f) ? "run"
					                                               : "walk";
					idx = rt_clip_by_name(sa, want);
					if (idx < 0 && plan_speed > 0.15f)
						idx = rt_clip_by_name(sa, "walk");
				}
				if (idx >= 0) sa->active_clip = idx;
			}
			sa->playing = true;
		}
	}
}

/* Detect entity transforms edited outside physics (editor gizmo, gameplay
 * scripts) since the last write-back and teleport / re-scale the body to
 * match — Unity-style runtime TRS editing. */
static int rt_v3_changed(jce_vec3 a, jce_vec3 b)
{
	const float E = 1e-5f;
	return fabsf(a.x - b.x) > E || fabsf(a.y - b.y) > E || fabsf(a.z - b.z) > E;
}

static int rt_q_changed(jce_quat a, jce_quat b)
{
	const float E = 1e-5f;
	return fabsf(a.x - b.x) > E || fabsf(a.y - b.y) > E ||
	       fabsf(a.z - b.z) > E || fabsf(a.w - b.w) > E;
}

static void rt_push_external_transforms(JceRuntime *rt)
{
	if (!rt->physics || !rt->scene) return;
	for (int i = 0; i < rt->body_count; ++i) {
		BodyEntry *be = &rt->bodies[i];
		JceTransform *tc = jce_scene_get_transform(rt->scene, be->entity);
		if (!tc) continue;

		if (rt_v3_changed(tc->position, be->last_pos) ||
		    rt_q_changed(tc->rotation, be->last_rot)) {
			jce_physics_body_set_transform(rt->physics, be->body,
			                               tc->position, tc->rotation);
			be->last_pos = tc->position;
			be->last_rot = tc->rotation;
			/* Teleport: collapse both interpolation endpoints onto the new
			 * pose so the next sync blends to a no-op instead of sliding
			 * the body in from its pre-edit physics position. */
			be->prev_pos = tc->position;
			be->prev_rot = tc->rotation;
			be->cur_pos  = tc->position;
			be->cur_rot  = tc->rotation;
		}
		if (rt_v3_changed(tc->scale, be->last_scale)) {
			jce_vec3 ratio;
			ratio.x = be->spawn_scale.x != 0.0f ? tc->scale.x / be->spawn_scale.x : 1.0f;
			ratio.y = be->spawn_scale.y != 0.0f ? tc->scale.y / be->spawn_scale.y : 1.0f;
			ratio.z = be->spawn_scale.z != 0.0f ? tc->scale.z / be->spawn_scale.z : 1.0f;
			jce_physics_body_set_scale(rt->physics, be->body, ratio);
			be->last_scale = tc->scale;
		}
	}

	/* Character (kept out of bodies[]): honor a gizmo TRS edit during Play by
	 * warping the kinematic capsule to the edited feet pose (feet -> center). */
	if (jce_character_valid(rt->character) && rt->character_entity != 0) {
		JceTransform *tc = jce_scene_get_transform(rt->scene, rt->character_entity);
		if (tc && (rt_v3_changed(tc->position, rt->char_last_pos) ||
		           rt_q_changed(tc->rotation, rt->char_last_rot))) {
			jce_vec3 center = tc->position;
			center.y += rt->character_half_height;   /* feet -> capsule center */
			jce_physics_character_set_position(rt->physics, rt->character, center);
			rt->char_prev_pos = center;              /* collapse interpolation */
			rt->char_cur_pos  = center;
			rt->char_last_pos = tc->position;        /* feet */
			rt->char_last_rot = tc->rotation;
		}
	}
}

/* Snapshot the post-step physics pose of every dynamic body into the
 * interpolation history (prev <- cur, cur <- live pose).  Called once per
 * executed fixed tick so prev/cur bracket exactly one fixed_dt of motion. */
static void rt_capture_fixed_pose(JceRuntime *rt)
{
	if (!rt->physics) return;
	for (int i = 0; i < rt->body_count; ++i) {
		if (rt->bodies[i].kind == (uint8_t)JCE_BODY_STATIC) continue;
		BodyEntry *be = &rt->bodies[i];
		be->prev_pos = be->cur_pos;
		be->prev_rot = be->cur_rot;
		jce_physics_body_get_transform(rt->physics, be->body,
		                               &be->cur_pos, &be->cur_rot);
	}
	/* Same fixed-tick history for the character so it renders smoothly. */
	if (jce_character_valid(rt->character) && rt->character_entity != 0) {
		rt->char_prev_pos = rt->char_cur_pos;
		jce_physics_character_get_position(rt->physics, rt->character,
		                                   &rt->char_cur_pos);
	}
}

/* Write each dynamic body's render pose into its scene Transform.  When
 * interpolation history is available (have_prev) the pose is the blend of
 * the last two fixed ticks by `alpha` (residual accumulator / fixed_dt),
 * so rendering stays smooth between fixed ticks; otherwise the raw current
 * pose is used.  The character controller is interpolated the same way, then
 * projected from the capsule CENTER down to the FEET (the entity Transform /
 * feet-pivoted character meshes live at the feet). */
static void rt_sync_transforms(JceRuntime *rt, float alpha)
{
	if (!rt->physics || !rt->scene) return;
	if (alpha < 0.0f) alpha = 0.0f;
	if (alpha > 1.0f) alpha = 1.0f;

	for (int i = 0; i < rt->body_count; ++i) {
		/* Static bodies never move during the step — skip the per-frame
		 * write. They are still repositioned through
		 * rt_push_external_transforms when the editor/script moves them. */
		if (rt->bodies[i].kind == (uint8_t)JCE_BODY_STATIC) continue;
		BodyEntry *be = &rt->bodies[i];
		jce_vec3 p; jce_quat q;
		if (rt->have_prev) {
			p = jce_v3_lerp(be->prev_pos, be->cur_pos, alpha);
			q = jce_q_slerp(be->prev_rot, be->cur_rot, alpha);
		} else {
			p = be->cur_pos;
			q = be->cur_rot;
		}
		JceTransform *tc = jce_scene_get_transform(rt->scene, be->entity);
		if (tc) {
			tc->position = p; tc->rotation = q;
			/* Refresh the cache so the interpolated pose we just wrote isn't
			 * mistaken for an external edit next frame. */
			be->last_pos = p;
			be->last_rot = q;
		}
	}

	if (jce_character_valid(rt->character) && rt->character_entity != 0) {
		jce_vec3 cp = rt->have_prev
			? jce_v3_lerp(rt->char_prev_pos, rt->char_cur_pos, alpha)
			: rt->char_cur_pos;
		cp.y -= rt->character_half_height;   /* capsule center -> feet */
		JceTransform *tc = jce_scene_get_transform(rt->scene,
		                                            rt->character_entity);
		if (tc) {
			tc->position = cp;
			rt->char_last_pos = cp;          /* next frame's edit-detect no-op */
		}
	}
}

/* Write each 2D body's simulated pose into its scene Transform: position x/y
 * land in the XY plane (z preserved), and the body angle becomes a Z-rotation
 * quaternion.  Static bodies don't move during the step, so skip them. */
static void rt_sync_transforms2d(JceRuntime *rt)
{
	if (!rt->physics2d || !rt->scene) return;
	for (int i = 0; i < rt->body2d_count; ++i) {
		if (rt->bodies2d[i].kind == (uint8_t)JCE_BODY_STATIC) continue;
		Body2DEntry *be = &rt->bodies2d[i];
		jce_vec2 p; float angle = 0.0f;
		jce_physics2d_body_get_transform(rt->physics2d, be->body, &p, &angle);
		JceTransform *tc = jce_scene_get_transform(rt->scene, be->entity);
		if (!tc) continue;
		tc->position.x = p.x;
		tc->position.y = p.y;
		/* z preserved (2D bodies live in the XY plane). */
		tc->rotation = jce_q_from_axis_angle(jce_v3(0.0f, 0.0f, 1.0f), angle);
	}
}

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

static void rt_pick_primary_cam(JceScene *s, JceEntity e, void *ud)
{
	CamScanCtx *ctx = (CamScanCtx *)ud;
	if (ctx->found) return;
	JceCameraComponent *cam = jce_scene_get_camera(s, e);
	if (!cam || !cam->is_primary) return;
	jce_mat4 w = jce_scene_get_world_matrix(s, e);
	ctx->pos = jce_v3(w.raw[3][0], w.raw[3][1], w.raw[3][2]);
	/* Columns 0/1/2 are the world X/Y/Z basis (possibly scaled); normalise
	 * to get pure orientation.  Engine convention: look down -Z, up = +Y. */
	jce_vec3 zaxis = jce_v3_normalize(jce_v3(w.raw[2][0], w.raw[2][1], w.raw[2][2]));
	ctx->forward = jce_v3_scale(zaxis, -1.0f);
	ctx->up      = jce_v3_normalize(jce_v3(w.raw[1][0], w.raw[1][1], w.raw[1][2]));
	ctx->found   = true;
}

/* Occlusion raycast adapter: returns the segment fraction at first physics
 * hit (1.0 = unobstructed). No material DB → mid absorption. */
static float rt_occlusion_raycast(void *ud, jce_vec3 origin, jce_vec3 dir,
                                  float max_distance, float *out_material)
{
	JceRuntime *rt = (JceRuntime *)ud;
	if (out_material) *out_material = 0.5f;
	if (!rt || !rt->physics || max_distance <= 0.0f) return 1.0f;
	JceRaycastResult r = jce_physics_raycast(rt->physics, origin, dir, max_distance);
	if (!r.hit) return 1.0f;
	float frac = r.distance / max_distance;
	return frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
}

/* ── Audio mixer buses (P1-audio-mixer-reverb) ──────────────────────── */

/* Seed the default bus layout matching the editor's mixer panel. */
static void rt_mixer_seed_default(JceAudioMixer *m)
{
	jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 0.8f);
	jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "SFX",   1.0f);
	jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Voice", 1.0f);
	jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "UI",    1.0f);
}

/* Minimal scanner over audio_mixer.json (same format the editor writes):
 * one { "id", "parent", "name", "volume", "muted", "solo" } record per bus.
 * Returns true if at least one record parsed; false if unreadable/empty. */
static bool rt_mixer_load_json(JceAudioMixer *m, const char *path)
{
	if (!path || !path[0]) return false;
	uint64_t size = 0;
	char *raw = (char *)jce_fs_host_read_all(path, &size);
	if (!raw) return false;
	if (size > (1u << 20)) { jce_fs_buffer_free(raw); return false; }

	const char *p = raw;
	bool any = false;
	while (p && *p) {
		const char *id_key = strstr(p, "\"id\"");
		if (!id_key) break;
		unsigned id = 0, parent = 0, mutedv = 0, solov = 0;
		float    vol = 1.0f;
		char     name[32] = {0};

		if (sscanf(id_key, "\"id\" : %u", &id) != 1)
			sscanf(id_key, "\"id\":%u", &id);

		const char *par_key = strstr(id_key, "\"parent\"");
		if (par_key && sscanf(par_key, "\"parent\" : %u", &parent) != 1)
			sscanf(par_key, "\"parent\":%u", &parent);

		const char *name_key = strstr(id_key, "\"name\"");
		if (name_key) {
			const char *q1 = strchr(name_key + 6, '"');
			const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
			if (q1 && q2) {
				size_t nl = (size_t)(q2 - q1 - 1);
				if (nl >= sizeof(name)) nl = sizeof(name) - 1;
				memcpy(name, q1 + 1, nl);
				name[nl] = 0;
			}
		}
		const char *vol_key = strstr(id_key, "\"volume\"");
		if (vol_key) sscanf(vol_key, "\"volume\" : %f", &vol);
		const char *mut_key = strstr(id_key, "\"muted\"");
		if (mut_key) sscanf(mut_key, "\"muted\" : %u", &mutedv);
		const char *sol_key = strstr(id_key, "\"solo\"");
		if (sol_key) sscanf(sol_key, "\"solo\" : %u", &solov);

		if (id == JCE_AUDIO_BUS_MASTER) {
			jce_audio_mixer_set_volume(m, JCE_AUDIO_BUS_MASTER, vol);
			jce_audio_mixer_set_muted (m, JCE_AUDIO_BUS_MASTER, mutedv != 0);
			jce_audio_mixer_set_solo  (m, JCE_AUDIO_BUS_MASTER, solov  != 0);
			any = true;
		} else if (id != 0 && name[0]) {
			JceAudioBusId par_id =
				(parent != 0) ? (JceAudioBusId)parent : JCE_AUDIO_BUS_MASTER;
			JceAudioBusId added = jce_audio_mixer_add_bus(m, par_id, name, vol);
			if (added != JCE_AUDIO_BUS_INVALID) {
				jce_audio_mixer_set_muted(m, added, mutedv != 0);
				jce_audio_mixer_set_solo (m, added, solov  != 0);
				any = true;
			}
		}
		p = (sol_key ? sol_key : (vol_key ? vol_key : id_key)) + 1;
	}
	jce_fs_buffer_free(raw);
	return any;
}

/* Stand up the runtime mixer + mirror its buses onto the audio device as
 * ma_sound_group buses.  Called once at create() when audio exists. */
static void rt_init_mixer(JceRuntime *rt, const char *config_path)
{
	if (!rt->audio) return;
	rt->mixer = jce_audio_mixer_create();
	if (!rt->mixer) return;

	if (!rt_mixer_load_json(rt->mixer, config_path))
		rt_mixer_seed_default(rt->mixer);

	/* Mirror every non-Master bus onto the audio device. */
	JceAudioBusId ids[64];
	uint32_t n = jce_audio_mixer_list_buses(rt->mixer, ids, 64);
	for (uint32_t i = 0; i < n; ++i) {
		if (ids[i] == JCE_AUDIO_BUS_MASTER) continue;
		const char *nm = jce_audio_mixer_get_name(rt->mixer, ids[i]);
		if (nm && nm[0]) jce_audio_bus_create(rt->audio, nm);
	}
}

/* Pick the mixer bus for an AudioSource.  No bus field exists on the
 * component, so derive one by role: looping non-spatial = Music (BGM),
 * everything else = SFX, falling back to whatever buses the project
 * actually defined. */
static const char *rt_bus_for_source(const JceRuntime *rt,
                                     const JceAudioSourceComponent *as,
                                     bool spatial)
{
	const char *want = (!spatial && as->loop) ? "Music" : "SFX";
	if (rt->mixer && jce_audio_mixer_find_bus(rt->mixer, want)
	        != JCE_AUDIO_BUS_INVALID)
		return want;
	if (rt->mixer && jce_audio_mixer_find_bus(rt->mixer, "SFX")
	        != JCE_AUDIO_BUS_INVALID)
		return "SFX";
	return NULL;   /* route direct to Master */
}

/* Push the resolved (solo/mute/volume) gain of every bus onto its matching
 * audio-device group each frame, so live mixer edits drive playback.  Master
 * maps to the engine master volume. */
static void rt_apply_mixer(JceRuntime *rt)
{
	if (!rt->audio || !rt->mixer) return;
	JceAudioBusId ids[64];
	uint32_t n = jce_audio_mixer_list_buses(rt->mixer, ids, 64);
	for (uint32_t i = 0; i < n; ++i) {
		const char *nm = jce_audio_mixer_get_name(rt->mixer, ids[i]);
		if (!nm || !nm[0]) continue;
		float gain = jce_audio_mixer_resolve_volume(rt->mixer, ids[i]);
		jce_audio_bus_set_volume(rt->audio, nm, gain);
	}
}

/* ── Reverb zones (P1-audio-mixer-reverb) ───────────────────────────── */

/* Map the Unity-style component preset selector onto a generic DSP preset. */
static JceReverbPreset rt_reverb_preset_for(int preset)
{
	switch (preset) {
	case JCE_REVERB_ZONE_PRESET_OFF: {
		JceReverbPreset p = jce_reverb_preset_outdoor();
		p.wet_mix = 0.0f;
		return p;
	}
	case JCE_REVERB_ZONE_PRESET_ROOM:
	case JCE_REVERB_ZONE_PRESET_LIVING_ROOM:
	case JCE_REVERB_ZONE_PRESET_BATHROOM:
	case JCE_REVERB_ZONE_PRESET_PADDED_CELL:
		return jce_reverb_preset_room();
	case JCE_REVERB_ZONE_PRESET_AUDITORIUM:
	case JCE_REVERB_ZONE_PRESET_CONCERT_HALL:
	case JCE_REVERB_ZONE_PRESET_ARENA:
	case JCE_REVERB_ZONE_PRESET_HANGAR:
	case JCE_REVERB_ZONE_PRESET_STONE_ROOM:
		return jce_reverb_preset_hall();
	case JCE_REVERB_ZONE_PRESET_CAVE:
	case JCE_REVERB_ZONE_PRESET_SEWER_PIPE:
	case JCE_REVERB_ZONE_PRESET_QUARRY:
		return jce_reverb_preset_cave();
	case JCE_REVERB_ZONE_PRESET_UNDERWATER:
		return jce_reverb_preset_underwater();
	case JCE_REVERB_ZONE_PRESET_GENERIC:
	case JCE_REVERB_ZONE_PRESET_FOREST:
	case JCE_REVERB_ZONE_PRESET_CITY:
	case JCE_REVERB_ZONE_PRESET_MOUNTAINS:
	case JCE_REVERB_ZONE_PRESET_PLAIN:
	case JCE_REVERB_ZONE_PRESET_PARKINGLOT:
	default:
		return jce_reverb_preset_outdoor();
	}
}

/* Walk the scene for AudioReverbZone components and build the runtime zone
 * set.  Each zone becomes a sphere at the entity's world position: full
 * strength within min_distance, blending out to max_distance. */
static void rt_reverb_zone_collect(JceScene *scene, JceEntity e, void *ud)
{
	JceRuntime *rt = (JceRuntime *)ud;
	JceAudioReverbZoneComponent *rz = jce_scene_get_audio_reverb_zone(scene, e);
	if (!rz) return;
	if (!rt->reverb_zones) {
		rt->reverb_zones = jce_reverb_zones_create(16);
		if (!rt->reverb_zones) return;
	}
	jce_vec3 wp = rt_world_position(scene, e);

	JceReverbZoneDesc d;
	memset(&d, 0, sizeof d);
	d.shape          = JCE_REVERB_SHAPE_SPHERE;
	d.center         = wp;
	float maxd       = rz->max_distance > 0.0f ? rz->max_distance : 10.0f;
	float mind       = rz->min_distance > 0.0f ? rz->min_distance : 0.0f;
	if (mind > maxd) mind = maxd;
	d.extents        = jce_v3(mind, 0.0f, 0.0f);     /* full-strength radius */
	d.falloff_radius = (maxd - mind) > 0.0f ? (maxd - mind) : 1.0f;
	d.priority       = 0;
	d.preset         = rt_reverb_preset_for(rz->preset);
	jce_reverb_zones_add(rt->reverb_zones, &d);
}

static void rt_build_reverb_zones(JceRuntime *rt)
{
	if (!rt->audio || !rt->scene) return;
	jce_scene_each_entity(rt->scene, rt_reverb_zone_collect, rt);
}

/* ── Navigation (P1-navmesh-chain) ───────────────────────────────────
 *
 * Load the editor-baked Detour navmesh and stand up a nav-agent set
 * bound to it via jce_recast_path_fn, so JceNavAgent destinations
 * resolve through jce_recast_find_path.  No-op when no path is supplied
 * or it fails to load.
 *
 * Runs BEFORE the rt_spawn_gameplay walk so authored NavAgent components
 * can register into the set as they are visited; rt_tick_gameplay then
 * syncs/steps the agents and writes positions back to the transforms.
 * The load -> query chain is also proven by a snap+find_path self-test
 * logged at create. */
static void rt_init_navmesh(JceRuntime *rt, const char *navmesh_path)
{
	if (!navmesh_path || !navmesh_path[0]) return;

	rt->nav_recast = jce_recast_load_file(navmesh_path);
	if (!rt->nav_recast) {
		jce_log_write(JCE_LOG_LEVEL_WARN, LOG_TAG, __FILE__, __LINE__,
		              "navmesh: failed to load '%s' (navigation disabled)",
		              navmesh_path);
		return;
	}

	/* Agent set with no bound grid navmesh — paths come from the Recast
	 * backend via the path-fn below. */
	rt->nav_agents = jce_nav_agent_set_create(NULL, 256u);
	if (rt->nav_agents)
		jce_nav_agent_set_path_fn(rt->nav_agents, jce_recast_path_fn,
		                          rt->nav_recast);

	/* Self-test: snap two points onto the mesh and prove a query path,
	 * so a broken load surfaces immediately in the log rather than as a
	 * silent no-path at runtime. */
	JceRecastStats st;
	jce_recast_get_stats(rt->nav_recast, &st);
	float sx, sy, sz;
	int   probe = 0;
	if (jce_recast_snap_to_navmesh(rt->nav_recast, 0.0f, 0.0f,
	                               &sx, &sy, &sz)) {
		float wp[2 * 8];
		probe = jce_recast_find_path(rt->nav_recast, sx, sz, sx, sz, wp, 8);
	}
	jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
	              "navmesh: loaded '%s' (%d polys, %d verts); self-test path=%d",
	              navmesh_path, st.polygon_count, st.vertex_count, probe);
}

/* Attach the listener to the primary camera's world position; push live
 * world positions to every spatial voice so distance attenuation tracks
 * scene movement; and attenuate each spatial voice by physics occlusion
 * (sound is quieter when a collider blocks the listener→source path). */
static void rt_update_audio_3d(JceRuntime *rt)
{
	if (!rt->audio || !rt->scene) return;

	/* Push live bus gains every frame so mixer edits affect playback. */
	rt_apply_mixer(rt);

	JceAudioListener L;
	memset(&L, 0, sizeof L);
	/* Engine convention: look down -Z, up = +Y.  Overwritten below by the
	 * primary camera's true world orientation when one exists. */
	L.forward[2] = -1.0f;
	L.up[1]      =  1.0f;

	CamScanCtx ctx = { rt->scene, { 0.0f, 0.0f, 0.0f }, false };
	jce_scene_each_entity(rt->scene, rt_pick_primary_cam, &ctx);
	if (ctx.found) {
		L.position[0] = ctx.pos.x;
		L.position[1] = ctx.pos.y;
		L.position[2] = ctx.pos.z;
		/* Drive panning/Doppler from where the camera actually looks, so
		 * turning the view re-spatialises the field (was hardwired -Z/+Y). */
		L.forward[0] = ctx.forward.x;
		L.forward[1] = ctx.forward.y;
		L.forward[2] = ctx.forward.z;
		L.up[0]      = ctx.up.x;
		L.up[1]      = ctx.up.y;
		L.up[2]      = ctx.up.z;
	}
	jce_audio_set_listener(rt->audio, &L);

	/* Position update + gather spatial sources for the occlusion solve. */
	enum { RT_MAX_OCC = 64 };
	JceAudioOcclusionQuery occ_q[RT_MAX_OCC];
	uint64_t               occ_ids[RT_MAX_OCC];
	int                    occ_vidx[RT_MAX_OCC];
	uint32_t               occ_n = 0;

	for (int i = 0; i < rt->voice_count; ++i) {
		if (!rt->voices[i].spatial) continue;
		jce_vec3 wp = rt_world_position(rt->scene, rt->voices[i].entity);
		jce_audio_voice_set_position(rt->audio, rt->voices[i].voice,
		                              wp.x, wp.y, wp.z);
		if (occ_n < RT_MAX_OCC) {
			occ_q[occ_n].source_position = wp;
			occ_ids[occ_n]  = (uint64_t)rt->voices[i].voice;
			occ_vidx[occ_n] = i;
			occ_n++;
		}
	}

	/* Occlusion: raycast listener->source against physics colliders, then
	   apply the result as a SEPARATE multiplicative gain over each voice's
	   authored base volume (so we never clobber the authored level), plus a
	   matching low-pass muffle.  The stateful tracker smooths per-source
	   attenuation across frames to avoid the pops the stateless solver gave
	   when a collider edge flickered in/out of the path. */
	if (rt->physics && occ_n > 0) {
		if (!rt->occ_tracker) {
			rt->occ_tracker = jce_audio_occlusion_tracker_create(RT_MAX_OCC);
			if (rt->occ_tracker) {
				JceAudioOcclusionParams op =
				    jce_audio_occlusion_default_params();
				jce_audio_occlusion_tracker_set_params(rt->occ_tracker, &op);
			}
		}
		jce_vec3 lp = jce_v3(L.position[0], L.position[1], L.position[2]);
		if (rt->occ_tracker) {
			jce_audio_occlusion_tracker_solve(rt->occ_tracker, lp, occ_ids,
			                                  occ_q, occ_n,
			                                  rt_occlusion_raycast, rt);
			jce_audio_occlusion_tracker_gc(rt->occ_tracker, occ_ids, occ_n);
		} else {
			/* Allocation failed: fall back to the stateless solve so audio
			 * still reacts to occlusion (just without temporal smoothing). */
			JceAudioOcclusionParams op =
			    jce_audio_occlusion_default_params();
			jce_audio_occlusion_solve(&op, lp, occ_q, occ_n,
			                          rt_occlusion_raycast, rt);
		}
		for (uint32_t k = 0; k < occ_n; ++k) {
			VoiceEntry *ve = &rt->voices[occ_vidx[k]];
			jce_audio_set_volume(rt->audio, ve->voice,
			                     ve->base_volume * occ_q[k].attenuation);
			jce_audio_set_lowpass(rt->audio, ve->voice, occ_q[k].lowpass_hz);
		}
	}

	/* ── Reverb zones: sample the blended preset at the listener and drive
	 * the global reverb DSP send wet/dry/decay.  Only active when the scene
	 * authored at least one AudioReverbZone (rt->reverb_zones non-NULL). */
	if (rt->reverb_zones && jce_reverb_zones_count(rt->reverb_zones) > 0) {
		jce_vec3 lp = jce_v3(L.position[0], L.position[1], L.position[2]);
		JceReverbPreset blend;
		jce_reverb_zones_sample(rt->reverb_zones, lp, &blend);

		JceAudioReverbParams rp;
		rp.wet_mix       = blend.wet_mix;
		rp.dry_mix       = blend.dry_mix;
		rp.decay_seconds = blend.decay_seconds;
		rp.room_size     = blend.room_size;
		rp.damping       = blend.damping;
		rp.diffusion     = blend.diffusion;
		rp.density       = blend.density;
		rp.pre_delay_ms  = blend.pre_delay_ms;
		rp.lowpass_hz    = blend.lowpass_hz;
		jce_audio_set_reverb(rt->audio, &rp);
	}
}

/* ── Gameplay tick (P0-master-bridge) ────────────────────────────────
 *
 * Resolve the gameplay "viewer/observer" point: the live character
 * controller position when one exists, otherwise the primary camera's
 * world position.  Used to drive trigger overlap + spawn density. */
static jce_vec3 rt_viewer_position(JceRuntime *rt)
{
	if (rt->physics && jce_character_valid(rt->character)) {
		jce_vec3 cp;
		jce_physics_character_get_position(rt->physics, rt->character, &cp);
		return cp;
	}
	CamScanCtx ctx = { rt->scene, { 0.0f, 0.0f, 0.0f }, false };
	if (rt->scene) jce_scene_each_entity(rt->scene, rt_pick_primary_cam, &ctx);
	return ctx.pos;
}

/* Advance trigger volumes, spawn managers, and weapons once per variable
 * frame.  Triggers re-test overlap against a single observer tracking the
 * viewer; spawn managers run their density/cadence state machine; weapons
 * drain fire/reload/recoil timers.  Each block is a no-op when the scene
 * authored no matching component (the subsystem stays NULL/empty). */
static void rt_tick_gameplay(JceRuntime *rt, float dt)
{
	jce_vec3 viewer = rt_viewer_position(rt);

	/* Trigger volumes: keep one observer at the viewer, re-test overlap. */
	if (rt->trigger_world) {
		if (!rt->trigger_player_valid) {
			rt->trigger_player = jce_observer_add(rt->trigger_world, viewer, 1u);
			rt->trigger_player_valid = jce_observer_valid(rt->trigger_player);
		} else {
			jce_observer_set_position(rt->trigger_world, rt->trigger_player,
			                          viewer);
		}
		jce_trigger_world_update(rt->trigger_world);
	}

	/* Spawn managers: viewer-relative density pop-in. */
	for (int i = 0; i < rt->spawn_count; ++i) {
		if (!rt->spawns[i].mgr) continue;
		jce_spawn_manager_set_viewer(rt->spawns[i].mgr, viewer);
		jce_spawn_manager_update(rt->spawns[i].mgr, dt);
	}

	/* Weapons: advance cooldown / reload / recoil-recovery timers.  Aim is
	 * the viewer looking forward; trigger is not held by the runtime, so no
	 * shots are produced here — games pull the trigger + consume shots via
	 * the scene component / their own JceWeaponInstance. */
	if (rt->weapon_count > 0) {
		jce_vec3 aim_dir = jce_v3(0.0f, 0.0f, -1.0f);
		for (int i = 0; i < rt->weapon_count; ++i) {
			WeaponEntry *we = &rt->weapons[i];
			JceWeaponShot shots[8];
			jce_weapon_update(&we->inst, &we->arch, viewer, aim_dir,
			                  (uint64_t)we->entity, (uint32_t)we->entity,
			                  dt, shots, 8, &we->rng);
		}
	}

	/* Nav-agents: sync authored components into the set, advance every agent
	 * along its path (seek/arrive through the Recast backend set at create()),
	 * then write the steered positions back to the entity transforms.  Empty
	 * set / navmesh-less scenes stay a cheap no-op. */
	if (rt->nav_agents && rt->nav_entry_count > 0 && rt->scene) {
		/* Pre-update sync: re-read each component every tick (flecs table
		 * moves invalidate cached component pointers); honour live enabled
		 * flips and re-issue the destination when auto_repath sees the goal
		 * move by more than the waypoint radius. */
		for (int i = 0; i < rt->nav_entry_count; ++i) {
			NavAgentEntry *ne = &rt->nav_entries[i];
			JceNavAgentComponent *nac = jce_scene_get_nav_agent(rt->scene,
			                                                    ne->entity);
			if (!nac) continue;
			if (!nac->enabled) {
				if (ne->has_dest) {
					jce_nav_agent_stop(rt->nav_agents, ne->handle);
					ne->has_dest = false;
				}
				continue;
			}
			float gx, gz;
			if (nac->target_entity != 0) {
				jce_vec3 tp = rt_world_position(rt->scene,
				                                (JceEntity)nac->target_entity);
				gx = tp.x; gz = tp.z;
			} else {
				gx = nac->target[0]; gz = nac->target[2];
			}
			float wr = nac->waypoint_radius > 0.0f ? nac->waypoint_radius : 0.5f;
			float dx = gx - ne->last_goal_x;
			float dz = gz - ne->last_goal_z;
			bool  moved = (dx * dx + dz * dz) > wr * wr;
			if (!ne->has_dest || (nac->auto_repath && moved)) {
				ne->has_dest    = jce_nav_agent_set_destination(rt->nav_agents,
				                                                ne->handle,
				                                                gx, gz);
				ne->last_goal_x = gx;
				ne->last_goal_z = gz;
			}
		}

		jce_nav_agent_set_update(rt->nav_agents, dt);

		/* Write-back: steered XZ into the entity transform; Y snaps to the
		 * navmesh surface when a polygon is near, else it is preserved.
		 * Entities driven by a dynamic rigid body are skipped — the physics
		 * write-back (rt_sync_transforms) owns their transform. */
		for (int i = 0; i < rt->nav_entry_count; ++i) {
			NavAgentEntry *ne = &rt->nav_entries[i];
			JceNavAgentComponent *nac = jce_scene_get_nav_agent(rt->scene,
			                                                    ne->entity);
			if (!nac || !nac->enabled) continue;
			bool body_driven = false;
			for (int j = 0; j < rt->body_count; ++j) {
				if (rt->bodies[j].entity == ne->entity &&
				    rt->bodies[j].kind != (uint8_t)JCE_BODY_STATIC) {
					body_driven = true;
					break;
				}
			}
			if (body_driven) continue;
			JceTransform *tc = jce_scene_get_transform(rt->scene, ne->entity);
			if (!tc) continue;
			float x, z;
			jce_nav_agent_get_position(rt->nav_agents, ne->handle, &x, &z);
			tc->position.x = x;
			tc->position.z = z;
			float sx, sy, sz;
			if (rt->nav_recast &&
			    jce_recast_snap_to_navmesh(rt->nav_recast, x, z, &sx, &sy, &sz))
				tc->position.y = sy;
		}
	} else if (rt->nav_agents && jce_nav_agent_set_count(rt->nav_agents) > 0) {
		/* Agents added directly through the API (no scene entries). */
		jce_nav_agent_set_update(rt->nav_agents, dt);
	}

	/* Behavior trees (P2-perception-bt-binding): for every agent that loaded
	 * a tree, run a perception pass (sight-cone + LOS raycast + hearing) for
	 * the player/viewer as the sole sensing candidate, writing stimuli into
	 * the agent's blackboard, then tick its tree on its cadence.  The bundled
	 * blackboard-reading actions consult rt->bt_active_bb during the tick.
	 * Empty (no authored BehaviorTree component) → cheap no-op. */
	if (rt->bt_count > 0) {
		/* Build the candidate set once: the player/viewer (loudness rises while
		 * moving so the hearing path is demonstrable) plus every active BT
		 * agent, so agents can perceive one another — not only the player. */
		enum { RT_MAX_PERCEPT = 64 };
		JcePerceptionTarget cand[RT_MAX_PERCEPT];
		int ncand = 0;
		{
			float walk = fabsf(rt->input.walk_x) + fabsf(rt->input.walk_z);
			cand[ncand].entity   = (uint64_t)rt->character_entity;
			cand[ncand].position = viewer;
			cand[ncand].loudness = (walk > 0.01f) ? 12.0f : 0.0f;
			ncand++;
		}
		for (int j = 0; j < rt->bt_count && ncand < RT_MAX_PERCEPT; ++j) {
			struct BtEntry *bj = &rt->bts[j];
			if (!bj->active) continue;
			cand[ncand].entity   = (uint64_t)bj->entity;
			cand[ncand].position = rt_world_position(rt->scene, bj->entity);
			cand[ncand].loudness = 0.0f;   /* agents are silent unless modelled */
			ncand++;
		}

		for (int i = 0; i < rt->bt_count; ++i) {
			struct BtEntry *be = &rt->bts[i];
			if (!be->active || !jce_bt_tree_valid(be->tree)) continue;

			/* Cadence gate: accumulate dt, only proceed once a tick period
			 * elapsed (period 0 = every frame). */
			if (be->tick_period > 0.0f) {
				be->tick_accum += dt;
				if (be->tick_accum < be->tick_period) continue;
				be->tick_accum -= be->tick_period;
			}

			/* Agent eye = its entity world position; forward = its -Z basis. */
			JcePerceptionAgent ag;
			memset(&ag, 0, sizeof ag);
			ag.eye_position     = rt_world_position(rt->scene, be->entity);
			if (rt->scene && jce_scene_has_transform(rt->scene, be->entity)) {
				jce_mat4 w = jce_scene_get_world_matrix(rt->scene, be->entity);
				ag.forward = jce_v3_normalize(jce_v3_scale(
					jce_v3(w.raw[2][0], w.raw[2][1], w.raw[2][2]), -1.0f));
			} else {
				ag.forward = jce_v3(0.0f, 0.0f, -1.0f);
			}
			ag.sight_range      = be->sight_range;
			ag.sight_half_angle = be->sight_half_angle;
			ag.hearing_range    = be->hearing_range;

			/* Targets = all candidates except this agent itself. */
			JcePerceptionTarget tlist[RT_MAX_PERCEPT];
			uint32_t tn = 0;
			for (int k = 0; k < ncand; ++k) {
				if (cand[k].entity == (uint64_t)be->entity) continue;
				tlist[tn++] = cand[k];
			}
			jce_perception_update(be->bb, &ag, tlist, tn,
			                      rt_bt_los_blocked, rt, NULL);

			/* Point the bundled actions at this agent's blackboard, then tick. */
			rt->bt_active_bb = be->bb;
			jce_bt_tick(rt->bt_ctx, be->tree);
			rt->bt_active_bb = NULL;
		}
	}
}

/* ── Public API ──────────────────────────────────────────────────── */

JCE_API JceRuntime *JCE_CALL jce_runtime_create(const JceRuntimeDesc *desc)
{
	if (!desc || !desc->scene) return NULL;

	JceRuntime *rt = (JceRuntime *)jce_malloc(sizeof *rt);
	if (!rt) return NULL;
	memset(rt, 0, sizeof *rt);

	rt->scene             = desc->scene;
	rt->pak               = desc->pak;
	rt->audio             = desc->audio;
	rt->audio_load_fn     = desc->audio_load_fn;
	rt->user_data         = desc->user_data;
	rt->character         = JCE_CHARACTER_INVALID;
	rt->character_entity  = 0;
	rt->input.speed_mult  = 1.0f;

	if (desc->enable_physics) {
		/* Single fixed cadence (P1-fixed-clock-unify): an explicit
		 * desc->fixed_timestep retunes the engine-wide clock so the
		 * JCE_PHASE_FIXED_UPDATE phase + net-transform tick conversion
		 * track physics; otherwise we ADOPT the engine clock's current
		 * fixed_dt (set via jce_engine_set_fixed_hz, default 1/60).  Either
		 * way the two clocks share one cadence and cannot desync. */
		JceFixedClock *gclock = jce_fixed_clock_default();
		float fixed_dt;
		if (desc->fixed_timestep > 0.0f) {
			fixed_dt = desc->fixed_timestep;
			gclock->fixed_dt = (double)fixed_dt;
		} else {
			fixed_dt = (gclock->fixed_dt > 0.0)
			           ? (float)gclock->fixed_dt : 1.0f / 60.0f;
		}

		JcePhysicsWorldDesc wd;
		memset(&wd, 0, sizeof wd);
		wd.gravity.x       = 0.0f;
		wd.gravity.y       = desc->gravity_y != 0.0f ? desc->gravity_y : -9.81f;
		wd.gravity.z       = 0.0f;
		wd.fixed_timestep  = fixed_dt;
		wd.split_impulse   = -1; /* leave Bullet default (ON) */
		rt->physics = jce_physics_create(&wd);
		if (!rt->physics) {
			jce_log_write(JCE_LOG_LEVEL_ERROR, LOG_TAG, __FILE__, __LINE__,
			              "%s", "failed to create physics world");
		}

		/* 2D physics world (Box2D) shares the same gravity_y and runs on
		 * the same fixed-step cadence as the 3D world. */
		JcePhysics2DDesc wd2;
		memset(&wd2, 0, sizeof wd2);
		wd2.gravity.x   = 0.0f;
		wd2.gravity.y   = desc->gravity_y != 0.0f ? desc->gravity_y : -9.81f;
		wd2.max_bodies  = 0; /* wrapper default (4096) */
		rt->physics2d = jce_physics2d_create(&wd2);
		if (!rt->physics2d) {
			jce_log_write(JCE_LOG_LEVEL_ERROR, LOG_TAG, __FILE__, __LINE__,
			              "%s", "failed to create 2D physics world");
		}

		/* Runtime-owned fixed-step accumulator: its CADENCE mirrors the
		 * engine-wide clock (above), but it keeps its OWN accumulator +
		 * tick_count because jce_runtime_step is a separate call site from
		 * jce_engine_iterate's FIXED_UPDATE block — sharing one accumulator
		 * would double-bank the frame dt.  jce_runtime_step re-adopts the
		 * engine clock's fixed_dt each frame so a mid-session
		 * jce_engine_set_fixed_hz() reaches physics.  Per-frame catch-up is
		 * clamped to RT_MAX_FIXED_STEPS ticks; we feed the physics world a
		 * single fixed_dt per tick (one internal substep). */
		jce_fixed_clock_init(&rt->clock, (double)fixed_dt,
		                     (double)fixed_dt * (double)RT_MAX_FIXED_STEPS);
	}

	/* Game-content localization (L10n): initialise the process-global
	 * jce_loc table ONLY when the caller hands us a source — a host dir of
	 * <locale>.json files and/or a PAK carrying "i18n/<locale>.json".
	 * Editor Play passes neither (pak=NULL, locales_dir=NULL), so the
	 * editor-owned jce_loc state (preview locale) is left untouched. */
	{
		bool has_loc_dir = desc->locales_dir && desc->locales_dir[0];
		if (has_loc_dir || desc->pak) {
			jce_loc_init(has_loc_dir ? desc->locales_dir : NULL);
			if (desc->pak)
				jce_loc_set_source_pak(desc->pak, "i18n");
			char auto_tag[32];
			const char *tag = (desc->locale && desc->locale[0])
			                  ? desc->locale : NULL;
			if (!tag &&
			    jce_host_preferred_locale(auto_tag, sizeof auto_tag))
				tag = auto_tag;
			jce_loc_set_locale(tag ? tag : "en");
		}
	}

	/* Audio mixer: stand up the bus tree (from audio_mixer.json or default)
	 * and mirror it onto the audio device BEFORE the spawn walk so each
	 * AudioSource voice can be routed to its bus as it is created. */
	rt_init_mixer(rt, desc->mixer_config_path);

	/* One pass over the scene to instantiate bodies, character, and
	 * voices.  Components missing from the scene are silently skipped. */
	jce_scene_each_entity(rt->scene, rt_spawn_entity, rt);

	/* Second pass: joints/constraints, now that every body exists. */
	if (rt->physics)
		jce_scene_each_entity(rt->scene, rt_spawn_joint, rt);

	/* Navigation: load the editor-baked navmesh + stand up the agent set
	 * BEFORE the gameplay walk so NavAgent components can register into it. */
	rt_init_navmesh(rt, desc->navmesh_path);

	/* Third pass: gameplay subsystems (triggers / spawners / weapons). */
	jce_scene_each_entity(rt->scene, rt_spawn_gameplay, rt);

	/* Reverb zones: build the zone set from AudioReverbZone components now
	 * that transforms are finalized.  Driven each frame in rt_update_audio_3d. */
	rt_build_reverb_zones(rt);

	/* Behavior-tree action context (master-bridge: instantiate so games can
	 * register actions + load trees against a runtime-owned context).  Trees
	 * are not auto-ticked — see the bt_ctx field comment. */
	rt->bt_ctx = jce_bt_create();

	/* Save / snapshot (P2-save-snapshot): stand up a registry with the
	 * scene/ECS provider so SavePoint overlaps + game code can persist and
	 * restore the session.  saves_dir (optional) is the SavePoint write base. */
	rt->save_registry = jce_snapshot_registry_create();
	if (rt->save_registry)
		jce_save_register_scene_provider(rt->save_registry, rt->scene);
	if (desc->saves_dir && desc->saves_dir[0]) {
		size_t dn = strlen(desc->saves_dir);
		if (dn >= sizeof rt->saves_dir) dn = sizeof rt->saves_dir - 1;
		memcpy(rt->saves_dir, desc->saves_dir, dn);
		rt->saves_dir[dn] = '\0';
	}

	/* Networking: only pump an EXISTING session.  The runtime never auto-
	 * starts a host/client (that is a deliberate game decision); when a game
	 * has started one, jce_session_tick() is driven in the fixed loop. */
	rt->net_session_driven = (jce_session_mode() != JCE_SESSION_MODE_NONE);

	/* Bridge authored Net* components into the net runtime (adopt net
	 * objects, register transforms, bind world/scene).  No-op when no
	 * session is live. */
	rt_init_net_bridge(rt);

	jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
	              "runtime: physics=%s bodies=%d bodies2d=%d character=%s voices=%d",
	              rt->physics ? "on" : "off",
	              rt->body_count,
	              rt->body2d_count,
	              jce_character_valid(rt->character) ? "yes" : "no",
	              rt->voice_count);
	jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
	              "runtime: gameplay bridge -> triggers=%d spawners=%d weapons=%d "
	              "save_points=%d bt_ctx=%s behavior_trees=%d net_session=%s "
	              "(deferred: ai_steering, anim-events, ik, sprite-anim)",
	              rt->trigger_count,
	              rt->spawn_count,
	              rt->weapon_count,
	              rt->save_point_count,
	              rt->bt_ctx ? "on" : "off",
	              rt->bt_count,
	              rt->net_session_driven ? "driven" : "none");
	jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
	              "runtime: save snapshot -> registry=%s scene_provider=on saves_dir=%s",
	              rt->save_registry ? "on" : "off",
	              rt->saves_dir[0] ? rt->saves_dir : "(none)");
	return rt;
}

JCE_API void JCE_CALL jce_runtime_destroy(JceRuntime *rt)
{
	if (!rt) return;

	/* NOTE: jce_loc_shutdown is deliberately NOT called here.  The
	 * localization table is process-global and outlives any one runtime
	 * (the editor's preview locale must survive Play sessions; final
	 * cleanup happens at engine teardown). */

	/* Join any in-flight async audio decodes and drop their results. */
	for (int i = 0; i < rt->pending_audio_count; ++i) {
		RtPendingAudio *p = &rt->pending_audio[i];
		if (p->thr) jce_thread_join(p->thr);
		if (p->args) {
			jce_audio_cpu_free(p->args->cpu);
			if (p->args->done) jce_atomic_i32_destroy(p->args->done);
			jce_free(p->args);
		}
	}
	jce_free(rt->pending_audio);
	rt->pending_audio = NULL;
	rt->pending_audio_count = rt->pending_audio_cap = 0;

	if (rt->audio) {
		for (int i = 0; i < rt->voice_count; ++i)
			jce_audio_stop(rt->audio, rt->voices[i].voice);
		for (int i = 0; i < rt->voice_count; ++i)
			jce_audio_unload(rt->audio, rt->voices[i].sound);
	}
	if (rt->occ_tracker)
		jce_audio_occlusion_tracker_destroy(rt->occ_tracker);
	if (rt->mixer)
		jce_audio_mixer_destroy(rt->mixer);
	if (rt->reverb_zones)
		jce_reverb_zones_destroy(rt->reverb_zones);
	jce_free(rt->voices);

	if (rt->physics) {
		if (jce_character_valid(rt->character))
			jce_physics_character_destroy(rt->physics, rt->character);
		jce_physics_destroy(rt->physics);
	}
	jce_free(rt->bodies);

	if (rt->physics2d)
		jce_physics2d_destroy(rt->physics2d);
	jce_free(rt->bodies2d);

	/* ── Gameplay subsystems (P0-master-bridge) ── */
	for (int i = 0; i < rt->spawn_count; ++i)
		if (rt->spawns[i].mgr) jce_spawn_manager_destroy(rt->spawns[i].mgr);
	jce_free(rt->spawns);
	jce_free(rt->weapons);
	jce_free(rt->triggers);
	jce_free(rt->save_points);
	if (rt->trigger_world)
		jce_trigger_world_destroy(rt->trigger_world);
	/* Behavior trees + per-agent blackboards (P2-perception-bt-binding).
	 * Halt running trees before tearing the context down, then free each
	 * agent's blackboard and the entry array. */
	for (int i = 0; i < rt->bt_count; ++i) {
		if (rt->bt_ctx && jce_bt_tree_valid(rt->bts[i].tree))
			jce_bt_halt(rt->bt_ctx, rt->bts[i].tree);
		if (rt->bts[i].bb)
			jce_blackboard_destroy(rt->bts[i].bb);
	}
	jce_free(rt->bts);
	if (rt->bt_ctx)
		jce_bt_destroy(rt->bt_ctx);

	/* ── Save / snapshot (P2-save-snapshot) ── */
	if (rt->save_registry)
		jce_snapshot_registry_destroy(rt->save_registry);

	/* ── Navigation (P1-navmesh-chain) ── */
	jce_free(rt->nav_entries);
	if (rt->nav_agents)
		jce_nav_agent_set_destroy(rt->nav_agents);
	if (rt->nav_recast)
		jce_recast_destroy(rt->nav_recast);

	jce_free(rt);
}

JCE_API void JCE_CALL jce_runtime_step(JceRuntime *rt, float dt)
{
	if (!rt) return;
	if (dt <= 0.0f) dt = 1.0f / 60.0f;

	if (rt->physics) {
		/* Keep the physics cadence locked to the engine-wide clock so a
		 * jce_engine_set_fixed_hz() (or the FIXED_UPDATE phase rate) and the
		 * physics step can never desync (P1-fixed-clock-unify).  We copy the
		 * cadence into our own accumulator-bearing clock rather than stepping
		 * the global one directly, because jce_engine_iterate already banks
		 * the frame dt into the global clock for the FIXED_UPDATE phase —
		 * sharing it would double-count.  max_frame_dt tracks the cadence so
		 * the RT_MAX_FIXED_STEPS catch-up ceiling stays correct. */
		const double gdt = jce_fixed_clock_default()->fixed_dt;
		if (gdt > 0.0 && gdt != rt->clock.fixed_dt) {
			rt->clock.fixed_dt     = gdt;
			rt->clock.max_frame_dt = gdt * (double)RT_MAX_FIXED_STEPS;
			/* Don't let a now-oversized residual replay as a burst. */
			if (rt->clock.accumulator > gdt)
				rt->clock.accumulator = gdt;
		}

		/* Bank the frame time and run the simulation in whole fixed_dt
		 * chunks.  The fixed clock clamps frame_dt (spiral guard) so the
		 * tick count this frame is bounded by RT_MAX_FIXED_STEPS. */
		const float fixed_dt = (float)rt->clock.fixed_dt;
		uint32_t    steps    = jce_fixed_clock_advance(&rt->clock, (double)dt);

		/* Apply external (editor/script) TRS edits once before stepping so
		 * teleports land on the upcoming fixed ticks. */
		rt_push_external_transforms(rt);

		for (uint32_t s = 0; s < steps; ++s) {
			rt_drive_character(rt, fixed_dt);
			jce_physics_step(rt->physics, fixed_dt);
			if (rt->physics2d)
				jce_physics2d_step(rt->physics2d, fixed_dt);
			/* Networking on the fixed cadence (only when a session is live —
			 * checked per tick so a session started AFTER runtime create is
			 * still driven): poll the host, advance handshake/timeout,
			 * dispatch packets + replication.  No-op otherwise. */
			if (jce_session_mode() != JCE_SESSION_MODE_NONE) {
				jce_session_tick();
				/* Sample + broadcast transform snapshots, then encode the
				 * acked-baseline delta / late-joiner burst.  The tick id is
				 * the engine-wide fixed clock so client interpolation shares
				 * one timeline with the sim. */
				jce_net_transform_fixed_step();
				JceNetTick ntick =
				    (JceNetTick)jce_fixed_clock_default()->tick_count;
				jce_net_replication_tick(ntick);
			}
			jce_fixed_clock_tick(&rt->clock);
			/* Capture post-tick poses for render interpolation. */
			rt_capture_fixed_pose(rt);
			rt->have_prev = true;
		}

		/* Blend the last two fixed poses by the residual accumulator so the
		 * rendered transform is smooth even when steps==0 this frame. */
		rt_sync_transforms(rt, (float)rt->clock.alpha);
		/* 2D bodies aren't interpolated — write the latest Box2D pose. */
		if (rt->physics2d && steps > 0)
			rt_sync_transforms2d(rt);
	} else if (jce_session_mode() != JCE_SESSION_MODE_NONE) {
		/* No physics world → no fixed loop above, but a live session still
		 * needs pumping.  Drive one net tick per frame on the engine-wide
		 * fixed clock (advanced elsewhere) so transform snapshots + the
		 * delta / late-joiner stream keep flowing for physics-less games. */
		jce_session_tick();
		jce_net_transform_fixed_step();
		jce_net_replication_tick(
		    (JceNetTick)jce_fixed_clock_default()->tick_count);
	}
	if (rt->scene)
		jce_scene_update(rt->scene, dt);
	/* Networking render-step: write interpolated poses for non-owned
	 * networked objects + apply owned-object snap corrections.  Runs after
	 * scene_update (so gameplay-owned transforms are final) and before the
	 * gameplay/audio bridge below.  No-op without a live session. */
	if (rt->net_bridged && jce_session_mode() != JCE_SESSION_MODE_NONE)
		jce_net_transform_render_step((double)rt->clock.alpha);
	/* SequencePlayer: advance every playing .seq.json and apply evaluated
	 * track values to the bound entities' components.  Runs after
	 * scene_update (freshest transforms) and before video/particles so
	 * downstream systems see sequenced values this frame.  Runtime-only:
	 * the editor previews through the Sequencer panel instead. */
	if (rt->scene)
		jce_scene_sequencer_update(rt->scene, dt);
	/* VideoPlayer-as-texture: advance every playing clip one frame and
	 * upload it into its component texture (the scene renderer binds it as
	 * mesh albedo).  Runs once per runtime step; the editor drives the same
	 * call when NOT in play mode so Scene View previews video too. */
	if (rt->scene)
		jce_scene_video_update(rt->scene, (double)dt, NULL, NULL);
	/* Particle emitters: load authored *.particles.json into the scene's
	 * shared JceParticleSystem, sync emitter origins to entity world
	 * positions, step the sim, and debug-draw alive particles.  Runs after
	 * scene_update for the freshest transforms (mirrors video-as-texture). */
	if (rt->scene)
		jce_scene_particles_update(rt->scene, dt);
	/* Variable-rate gameplay bridge: trigger overlap, spawn density, weapon
	 * timers.  Runs after scene_update so it reads the freshest transforms. */
	rt_tick_gameplay(rt, dt);
	/* Upload + play any play_on_awake clips whose async decode finished. */
	rt_audio_poll(rt);
	rt_update_audio_3d(rt);
}

JCE_API float JCE_CALL jce_runtime_interpolation_alpha(const JceRuntime *rt)
{
	if (!rt || !rt->physics) return 0.0f;
	return (float)jce_fixed_clock_alpha(&rt->clock);
}

/* Internal trampoline: forwards engine contact events to the game callback. */
static void rt_contact_trampoline(const JceContactEvent *ev, void *ud)
{
	JceRuntime *rt = (JceRuntime *)ud;
	if (rt && rt->contact_cb) rt->contact_cb(ev, rt->contact_ud);
}

JCE_API void JCE_CALL jce_runtime_set_contact_listener(JceRuntime *rt,
                                                       jce_contact_listener_fn fn,
                                                       void *userdata)
{
	if (!rt) return;
	rt->contact_cb = fn;
	rt->contact_ud = userdata;
	/* Register the engine-side listener once, only when a game subscribes. */
	if (fn && rt->physics && !rt->contact_registered) {
		if (jce_physics_add_contact_listener(rt->physics,
		                                     rt_contact_trampoline, rt))
			rt->contact_registered = true;
	}
}

JCE_API void JCE_CALL jce_runtime_set_input(JceRuntime *rt,
                                             const JceRuntimeInput *in)
{
	if (!rt || !in) return;
	rt->input.walk_x      = in->walk_x;
	rt->input.walk_z      = in->walk_z;
	if (in->jump_pressed) rt->input.jump_pressed = true;   /* sticky */
	rt->input.speed_mult  = in->speed_mult > 0.0f ? in->speed_mult : 1.0f;
	rt->input.sprint      = in->sprint;
	rt->input.jump_held   = in->jump_held;
}

JCE_API bool JCE_CALL jce_runtime_get_player_position(const JceRuntime *rt,
                                                       jce_vec3 *out_pos)
{
	if (!rt || !rt->physics || !jce_character_valid(rt->character)) return false;
	/* Return the SMOOTHED entity transform that rt_sync_transforms wrote
	 * (interpolated between fixed ticks + projected to the feet), so a follow
	 * camera tracks the same smooth pose as the rendered mesh — not the raw
	 * per-tick physics center, which jitters between ticks at render rate. */
	if (rt->scene && rt->character_entity != 0) {
		JceTransform *tc = jce_scene_get_transform(rt->scene, rt->character_entity);
		if (tc) { if (out_pos) *out_pos = tc->position; return true; }
	}
	jce_vec3 p;
	jce_physics_character_get_position(rt->physics, rt->character, &p);
	p.y -= rt->character_half_height;   /* fallback: raw feet */
	if (out_pos) *out_pos = p;
	return true;
}

JCE_API void JCE_CALL jce_runtime_pause_audio(JceRuntime *rt)
{
	if (!rt || !rt->audio) return;
	for (int i = 0; i < rt->voice_count; ++i)
		jce_audio_pause(rt->audio, rt->voices[i].voice);
}

JCE_API void JCE_CALL jce_runtime_resume_audio(JceRuntime *rt)
{
	if (!rt || !rt->audio) return;
	for (int i = 0; i < rt->voice_count; ++i)
		jce_audio_resume(rt->audio, rt->voices[i].voice);
}

JCE_API JcePhysicsWorld *JCE_CALL jce_runtime_physics(const JceRuntime *rt)
{
	return rt ? rt->physics : NULL;
}

JCE_API JceAudio *JCE_CALL jce_runtime_audio(const JceRuntime *rt)
{
	return rt ? rt->audio : NULL;
}

JCE_API JceScene *JCE_CALL jce_runtime_scene(const JceRuntime *rt)
{
	return rt ? rt->scene : NULL;
}

JCE_API JceSnapshotRegistry *JCE_CALL jce_runtime_save_registry(const JceRuntime *rt)
{
	return rt ? rt->save_registry : NULL;
}

JCE_API JceBtContext *JCE_CALL jce_runtime_bt_context(const JceRuntime *rt)
{
	return rt ? rt->bt_ctx : NULL;
}

JCE_API int JCE_CALL jce_runtime_bt_count(const JceRuntime *rt)
{
	return rt ? rt->bt_count : 0;
}

JCE_API JceBlackboard *JCE_CALL jce_runtime_bt_blackboard(const JceRuntime *rt,
                                                          uint64_t entity)
{
	if (!rt) return NULL;
	for (int i = 0; i < rt->bt_count; ++i)
		if ((uint64_t)rt->bts[i].entity == entity)
			return rt->bts[i].bb;
	return NULL;
}

JCE_API bool JCE_CALL jce_runtime_bt_tree(const JceRuntime *rt, uint64_t entity,
                                          uint32_t *out_tree_idx)
{
	if (!rt || !out_tree_idx) return false;
	for (int i = 0; i < rt->bt_count; ++i) {
		if ((uint64_t)rt->bts[i].entity == entity &&
		    jce_bt_tree_valid(rt->bts[i].tree)) {
			*out_tree_idx = rt->bts[i].tree.idx;
			return true;
		}
	}
	return false;
}

JCE_API bool JCE_CALL jce_runtime_save_to_file(JceRuntime *rt, const char *path)
{
	if (!rt || !rt->save_registry || !path || !path[0]) return false;

	/* mkdir -p the parent directory so a fresh save slot just works. */
	const char *slash = strrchr(path, '/');
	const char *bslash = strrchr(path, '\\');
	if (bslash && (!slash || bslash > slash)) slash = bslash;
	if (slash && slash != path) {
		char dir[640];
		size_t dl = (size_t)(slash - path);
		if (dl < sizeof dir) {
			memcpy(dir, path, dl);
			dir[dl] = '\0';
			if (!jce_fs_host_exists_dir(dir))
				jce_fs_host_create_directory(dir);
		}
	}
	return jce_snapshot_save_to_file(rt->save_registry, path);
}

JCE_API void JCE_CALL jce_runtime_set_locale(JceRuntime *rt, const char *locale)
{
	(void)rt; /* jce_loc is process-global; rt kept for API symmetry */
	if (!locale || !locale[0]) return;
	jce_loc_set_locale(locale);
}
