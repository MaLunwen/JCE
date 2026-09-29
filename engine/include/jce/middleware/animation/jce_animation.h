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

/* The clip this player is on, or NULL before anything plays.
 *
 * You could set a clip and read the time but not ask WHICH clip, which makes
 * the time unusable on its own: a playhead in seconds means nothing without
 * the duration, and the duration belongs to the clip.  Anything that has to
 * express the playhead as a POSITION -- a network snapshot, a UI scrubber --
 * needs both. */
JCE_API const JceAnimClip *jce_anim_player_get_clip(const JceAnimPlayer *p);
JCE_API bool  jce_anim_player_is_playing(const JceAnimPlayer *p);

/* Advance by dt seconds and produce joint matrices for GPU upload.
 * Returns number of joints written to out_joint_matrices. */
JCE_API uint32_t jce_anim_player_update(JceAnimPlayer *p, float dt,
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

/* -- Additive / layered blending (FEATURE 3.3) --------------------- */

typedef struct JceAvatarMask JceAvatarMask;

/* How a layer composes onto the accumulated (base) pose. */
typedef enum {
    /* delta(clip,ref) is ADDED onto the base pose, scaled per bone by the
     * mask weight (and the layer weight).  ref is the pose at which the clip
     * contributes nothing; pass a NULL ref clip to use the skeleton rest pose
     * (typical for additive idle/lean/recoil layers authored as deltas). */
    JCE_ANIM_LAYER_ADDITIVE = 0,
    /* the layer's sampled pose REPLACES the base pose, masked per bone — a
     * masked lerp from base toward the layer pose by (mask*weight).  Useful
     * for "upper body plays clip X while lower body keeps the base". */
    JCE_ANIM_LAYER_OVERRIDE = 1
} JceAnimLayerMode;

/* One layer in a small layered-animation stack. */
typedef struct {
    const JceAnimClip   *clip;       /* clip sampled for this layer (NULL = skip) */
    float                time;       /* sample time for `clip` */
    float                weight;     /* global layer weight, clamped [0,1] */
    JceAnimLayerMode     mode;
    const JceAvatarMask *mask;       /* optional per-bone weight (NULL = all 1) */
    /* Additive reference pose (ignored for OVERRIDE).  NULL clip = rest pose. */
    const JceAnimClip   *ref_clip;
    float                ref_time;
} JceAnimLayer;

/* Additive blend: accumulate the per-joint DELTA of (clip @ time) relative to
 * a reference pose onto a BASE pose, scaled per bone by `mask` and by `weight`.
 *
 *   base_clip @ base_time   -> the underlying pose (NULL = rest pose)
 *   add_clip  @ add_time    -> the additive clip
 *   ref_clip  @ ref_time    -> the pose at which add contributes nothing
 *                              (NULL = skeleton rest pose)
 *   mask                    -> per-bone weight (NULL = weight 1 for all bones)
 *   weight                  -> global additive weight, clamped [0,1]
 *
 * Per joint the delta is (rotation: ref^-1 * add, slerped from identity by the
 * effective weight then post-multiplied onto base; translation: add - ref added
 * to base; scale: add/ref multiplied onto base), with effective weight =
 * weight * mask[joint].  At effective weight 0 a bone is byte-identical to the
 * base pose; at 1 it takes the full delta.  Does not mutate playback state.
 * Returns the number of joints written. */
JCE_API uint32_t JCE_CALL jce_anim_player_blend_additive(
    JceAnimPlayer       *p,
    const JceAnimClip   *base_clip,  float base_time,
    const JceAnimClip   *add_clip,   float add_time,
    const JceAnimClip   *ref_clip,   float ref_time,
    const JceAvatarMask *mask,       float weight,
    jce_mat4            *out_joint_matrices,
    uint32_t             max_joints);

/* Layered blend: start from a base pose (base_clip @ base_time, NULL = rest
 * pose) and compose `num_layers` layers on top, in order, each additive or
 * override and each optionally masked.  With num_layers == 0 (and a base clip)
 * this is exactly a single-clip evaluation, byte-identical to today.  Does not
 * mutate playback state.  Returns the number of joints written. */
JCE_API uint32_t JCE_CALL jce_anim_player_blend_layers(
    JceAnimPlayer      *p,
    const JceAnimClip  *base_clip, float base_time,
    const JceAnimLayer *layers,    uint32_t num_layers,
    jce_mat4           *out_joint_matrices,
    uint32_t            max_joints);

/* -- Root motion --------------------------------------------------- */

/* Result of jce_anim_extract_root_delta: the motion the ROOT joint
 * accumulated between two sample times, expressed in the entity's LOCAL
 * (object) space.  The caller rotates `translation` by the entity's
 * current world orientation before adding it to the entity position. */
typedef struct {
    jce_vec3 translation;   /* root-joint local-space translation delta   */
    float    yaw_delta;     /* root-joint Y-axis (yaw) rotation delta, rad */
    bool     valid;         /* false when clip/skeleton/root were invalid  */
} JceAnimRootDelta;

/* Behaviour flags for jce_anim_extract_root_delta. */
typedef enum {
    JCE_ROOT_MOTION_NONE     = 0,
    /* Also re-center the supplied pose: subtract the root-joint translation
     * sampled at `cur_time` from out_pose[root] so the mesh stays in place
     * while the entity (driven by the returned delta) moves.  Requires a
     * non-NULL out_pose of at least (root_joint+1) entries already filled by
     * jce_anim_clip_sample at the SAME cur_time. */
    JCE_ROOT_MOTION_RECENTER = 1 << 0,
    /* Extract only the yaw component of the root delta into translation-free
     * motion (translation still computed; just a hint for consumers). */
    JCE_ROOT_MOTION_YAW      = 1 << 1
} JceAnimRootMotionFlags;

/* Extract the root-joint motion of `clip` between `prev_time` and `cur_time`.
 *
 * Samples the clip's root joint (index `root_joint`) at both times via the
 * REAL jce_anim_clip_sample path and returns the translation delta in the
 * root joint's local space, plus the yaw (Y-axis) rotation delta.
 *
 * Loop handling: when `loop` is true and `cur_time < prev_time` (the playhead
 * wrapped past the clip end this frame), the delta is accumulated as
 *   (pose(end) - pose(prev)) + (pose(cur) - pose(start))
 * so a single forward step never reads as a large backward jump.
 *
 * Re-centering: pass JCE_ROOT_MOTION_RECENTER together with `out_pose` (the
 * per-joint local matrices already produced by jce_anim_clip_sample at
 * `cur_time`, length >= root_joint+1) to strip the root translation from the
 * pose — the mesh then stays at the origin of its transform while the entity
 * moves by the returned delta.  Pass NULL out_pose / NONE to only measure.
 *
 * `skel` may be NULL; it is only consulted for the rest pose when sampling
 * (kept symmetric with jce_anim_clip_sample, which tolerates NULL rest TRS). */
JCE_API JceAnimRootDelta JCE_CALL jce_anim_extract_root_delta(
    const JceAnimClip  *clip,
    const JceSkeleton  *skel,
    uint32_t            root_joint,
    float               prev_time,
    float               cur_time,
    bool                loop,
    uint32_t            flags,
    jce_mat4           *out_pose,
    uint32_t            out_pose_joints);

/* Enable/disable root-motion extraction on a player.  When enabled,
 * jce_anim_player_update extracts the root joint's per-frame translation/yaw
 * delta (loop-wrap aware) and RE-CENTERS the sampled pose (strips the root
 * translation) before producing skinning matrices, so the mesh renders in
 * place while the consumer drives the entity by the delta.  `root_joint` is
 * the skeleton index of the root (typically the joint with parent -1, e.g.
 * the hips).  Disabled by default — playback is unchanged until enabled. */
JCE_API void JCE_CALL jce_anim_player_set_root_motion(JceAnimPlayer *p,
                                                      bool enabled,
                                                      uint32_t root_joint);

/* Consume the root-motion delta produced by the most recent
 * jce_anim_player_update (zeroed each update that doesn't advance, e.g. paused).
 * Returns the delta in the root joint's LOCAL space; the caller rotates it by
 * the entity's current world orientation before adding it to the position. */
JCE_API JceAnimRootDelta JCE_CALL jce_anim_player_consume_root_motion(JceAnimPlayer *p);

/* -- Skeleton queries ---------------------------------------------- */

/* Return the number of joints in a skeleton. */
JCE_API uint32_t jce_skeleton_joint_count(const JceSkeleton *skel);

/* Return the name of a skeleton joint by index (or NULL). */
JCE_API const char *jce_skeleton_joint_name(const JceSkeleton *skel, uint32_t index);

JCE_EXTERN_C_END

#endif /* JCE_ANIMATION_PUBLIC_H */
