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
	 * every AudioSource clip instead of `jce_audio_load(audio, pak, clip)`.
	 * Use this to plug in a host-filesystem probe (editor in-tree assets)
	 * or any custom path resolution layer.  Return JCE_SOUND_INVALID to
	 * signal "could not load" — the runtime will warn-log and skip the
	 * voice.  `user_data` is forwarded verbatim. */
	uint32_t     (*audio_load_fn)(void *user_data,
	                              JceAudio *audio,
	                              const char *clip_path);
	void          *user_data;

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
} JceRuntimeDesc;

/* Player input applied to the scene's CharacterController each step.
 * walk_x / walk_z are in scene-space and typically clamped to [-1, +1].
 * speed_mult lets gameplay layer sprint/crouch on top.  jump_pressed
 * is edge-triggered — held inside the runtime until the next step()
 * consumes it via jce_physics_character_jump. */
typedef struct {
	float walk_x;
	float walk_z;
	bool  jump_pressed;
	float speed_mult;
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

/* Read the live character controller world position.  Returns false
 * when no CharacterController exists in the scene. */
JCE_API bool        JCE_CALL jce_runtime_get_player_position(const JceRuntime *rt,
                                                             jce_vec3 *out_pos);

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

JCE_EXTERN_C_END
#endif /* JCE_APPLICATION_RUNTIME_H */
