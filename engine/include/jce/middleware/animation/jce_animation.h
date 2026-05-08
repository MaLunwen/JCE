/*
 * jce_animation.h  Public animation playback API.
 *
 * Provides animation player for skeletal animation of JceModel instances.
 * Layer: Animation (Layer 3) — public.
 */

#ifndef JCE_ANIMATION_PUBLIC_H
#define JCE_ANIMATION_PUBLIC_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceAnimClip   JceAnimClip;
typedef struct JceAnimPlayer JceAnimPlayer;
typedef struct JceSkeleton   JceSkeleton;

/* -- Clip queries -------------------------------------------------- */

JCE_API const char *jce_anim_clip_name(const JceAnimClip *clip);
JCE_API float       jce_anim_clip_duration(const JceAnimClip *clip);

/* -- Animation player ---------------------------------------------- */

/* Create a player bound to a skeleton. */
JCE_API JceAnimPlayer *jce_anim_player_create(const JceSkeleton *skel);
JCE_API void           jce_anim_player_destroy(JceAnimPlayer *player);

/* Start playing a clip. loop=true for continuous, speed=1.0 for normal. */
JCE_API void JCE_CALL jce_anim_player_play(JceAnimPlayer *p, const JceAnimClip *clip,
                                           bool loop, float speed);
JCE_API void  jce_anim_player_stop(JceAnimPlayer *p);
JCE_API void  jce_anim_player_pause(JceAnimPlayer *p, bool paused);
JCE_API void  jce_anim_player_set_speed(JceAnimPlayer *p, float speed);
JCE_API void  jce_anim_player_set_time(JceAnimPlayer *p, float time);
JCE_API float jce_anim_player_get_time(const JceAnimPlayer *p);
JCE_API bool  jce_anim_player_is_playing(const JceAnimPlayer *p);

/* Advance by dt seconds and produce joint matrices for GPU upload.
 * Returns number of joints written to out_joint_matrices. */
uint32_t jce_anim_player_update(JceAnimPlayer *p, float dt,
                                jce_mat4 *out_joint_matrices,
                                uint32_t max_joints);

/* Blend two clips into the skeleton without mutating the player's
 * playback state.  Useful for animator state-machine cross-fades and
 * blend-tree (e.g. JceAnimBlendTree) evaluation.
 *
 * Samples `clip_a` at `time_a` and `clip_b` at `time_b`, then blends
 * the resulting per-joint TRS using the supplied weights (normalised
 * internally if they don't sum to 1).  Either clip may be NULL — that
 * branch simply contributes zero weight.  When both are NULL the
 * skeleton evaluates against the rest pose.
 *
 * Returns the number of joints written to `out_joint_matrices`. */
JCE_API uint32_t jce_anim_player_blend(JceAnimPlayer    *p,
                                        const JceAnimClip *clip_a,
                                        float              time_a,
                                        float              weight_a,
                                        const JceAnimClip *clip_b,
                                        float              time_b,
                                        float              weight_b,
                                        jce_mat4         *out_joint_matrices,
                                        uint32_t           max_joints);

/* -- Root motion --------------------------------------------------- *
 *
 * When enabled, joint 0 (the root) is treated as movement of the
 * entity rather than the mesh itself.  Each call to player_update
 * accumulates the root delta (translation since previous frame and
 * rotation around the up axis); the local root transform is then
 * zeroed so the mesh stays at the entity origin.  Game code reads back
 * the accumulated motion via consume_root_motion() and applies it to
 * the entity's Transform.
 *
 * Mirrors Unity's `Animator.applyRootMotion = true` + the
 * deltaPosition / deltaRotation properties read in OnAnimatorMove. */

JCE_API void jce_anim_player_set_root_motion(JceAnimPlayer *p, bool enabled);
JCE_API bool jce_anim_player_get_root_motion(const JceAnimPlayer *p);

/* Read and reset the accumulated root motion since the last call.
 * Either out parameter may be NULL.  Returned translation is in the
 * skeleton's coordinate space — typically the model's local space, so
 * game code should rotate it by the entity's current orientation
 * before adding to position. */
JCE_API void jce_anim_player_consume_root_motion(JceAnimPlayer *p,
                                                 jce_vec3 *out_translation,
                                                 jce_quat *out_rotation);

/* -- Animation events ---------------------------------------------- *
 *
 * Discrete callbacks fired when the playback time crosses configured
 * timestamps.  Mirrors Unity's `AnimationEvent`.  Typical use cases:
 * footstep sounds, weapon swing impact frames, particle spawns.
 *
 * The player owns a copy of the event array — caller can free its
 * source after the call.  Up to JCE_ANIM_MAX_EVENTS events per clip.
 * Looping clips re-fire events on each loop. */

#define JCE_ANIM_MAX_EVENTS 32

typedef struct {
    float time;          /* seconds into clip */
    char  name[48];      /* user identifier */
    int   int_payload;
    float float_payload;
} JceAnimEvent;

/* fn is invoked with the user-data passed to set_event_callback and a
 * pointer to the event that just fired (must not be retained beyond
 * the callback). */
typedef void (*JceAnimEventFn)(const JceAnimEvent *evt, void *user_data);

JCE_API void jce_anim_player_set_events(JceAnimPlayer *p,
                                        const JceAnimEvent *events,
                                        uint32_t count);

JCE_API void jce_anim_player_set_event_callback(JceAnimPlayer *p,
                                                JceAnimEventFn fn,
                                                void *user_data);

/* -- Skeleton queries ---------------------------------------------- */

/* Return the number of joints in a skeleton. */
JCE_API uint32_t jce_skeleton_joint_count(const JceSkeleton *skel);

/* Return the name of a skeleton joint by index (or NULL). */
JCE_API const char *jce_skeleton_joint_name(const JceSkeleton *skel, uint32_t index);

JCE_EXTERN_C_END

#endif /* JCE_ANIMATION_PUBLIC_H */
