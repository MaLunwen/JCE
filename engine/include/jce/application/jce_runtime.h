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
#include <stdbool.h>

JCE_EXTERN_C_BEGIN

typedef struct JceRuntime       JceRuntime;
typedef struct JceScene         JceScene;
typedef struct JcePakArchive    JcePakArchive;
typedef struct JceAudio         JceAudio;
typedef struct JcePhysicsWorld  JcePhysicsWorld;

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

/* Advance one frame:
 *   1. drive character controller from the most recent set_input()
 *   2. jce_physics_step(world, dt)
 *   3. write physics transforms back into scene Transform components
 *   4. jce_scene_update(scene, dt)
 *   5. update audio 3D listener + spatial voice positions
 * Safe with NULL or dt <= 0. */
JCE_API void        JCE_CALL jce_runtime_step(JceRuntime *rt, float dt);

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

/* Sub-system accessors so projects can layer custom systems (raycasts,
 * ad-hoc voices) on top of the runtime.  NULL when the matching feature
 * wasn't enabled. */
JCE_API JcePhysicsWorld *JCE_CALL jce_runtime_physics(const JceRuntime *rt);
JCE_API JceAudio        *JCE_CALL jce_runtime_audio  (const JceRuntime *rt);
JCE_API JceScene        *JCE_CALL jce_runtime_scene  (const JceRuntime *rt);

JCE_EXTERN_C_END
#endif /* JCE_APPLICATION_RUNTIME_H */
