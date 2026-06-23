/*
 * jce_animation.h  Skeletal animation clips and playback.
 *
 * An animation clip contains channels that drive joint TRS properties
 * over time via keyframes. The animation player manages playback state
 * and produces joint matrices each frame.
 *
 * Layer: Animation (Layer 3).
 */

#ifndef JCE_ANIMATION_H
#define JCE_ANIMATION_H

#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Interpolation mode                                                  */
/* ================================================================== */

typedef enum {
    JCE_INTERP_STEP,          /* Constant value until next keyframe */
    JCE_INTERP_LINEAR,        /* Linear interpolation (lerp / slerp) */
    JCE_INTERP_CUBIC_SPLINE   /* Cubic spline (glTF 2.0 spec) */
} JceInterpolation;

/* ================================================================== */
/* Animation channel (one property of one joint)                       */
/* ================================================================== */

typedef enum {
    JCE_ANIM_TARGET_TRANSLATION,
    JCE_ANIM_TARGET_ROTATION,
    JCE_ANIM_TARGET_SCALE
} JceAnimTarget;

typedef struct {
    uint32_t         joint_index;
    JceAnimTarget    target;
    JceInterpolation interpolation;

    float           *timestamps;      /* [count] sorted ascending */
    uint32_t         count;           /* number of keyframes */

    /* Exactly one of these is non-NULL depending on target. */
    jce_vec3        *translations;    /* [count] for TRANSLATION */
    jce_quat        *rotations;       /* [count] for ROTATION */
    jce_vec3        *scales;          /* [count] for SCALE */
} JceAnimChannel;

/* ================================================================== */
/* Animation clip                                                      */
/* ================================================================== */

typedef struct JceAnimClip JceAnimClip;

/* Create an animation clip. Copies all channel data; caller retains ownership. */
JceAnimClip *jce_anim_clip_create(const char *name,
                                    const JceAnimChannel *channels,
                                    uint32_t num_channels,
                                    float duration);

void         jce_anim_clip_destroy(JceAnimClip *clip);
const char  *jce_anim_clip_name(const JceAnimClip *clip);
float        jce_anim_clip_duration(const JceAnimClip *clip);

/* Number of TRS channels in the clip (0 if NULL).  Used by importers / tests to
 * verify a clip carries joint tracks. */
uint32_t     jce_anim_clip_channel_count(const JceAnimClip *clip);

/* Sample the clip at a given time, writing per-joint local transforms.
 * out_locals: array of [num_joints] mat4 (typically skeleton joint count).
 * Joints not affected by this clip are left unchanged;
 * rest_t/rest_r/rest_s: per-joint rest-pose TRS from jce_skeleton_rest_trs().
 * Pass NULL for rest_t/rest_r/rest_s to use a zero-delta default. */
void jce_anim_clip_sample(const JceAnimClip *clip, float time,
                            jce_mat4 *out_locals, uint32_t num_joints,
                            const jce_vec3 *rest_t,
                            const jce_quat *rest_r,
                            const jce_vec3 *rest_s);

/* ================================================================== */
/* Animation player (per-entity playback state)                        */
/* ================================================================== */

typedef struct JceAnimPlayer JceAnimPlayer;

/* Create a player bound to a skeleton (for joint count / rest pose). */
JceAnimPlayer *jce_anim_player_create(const JceSkeleton *skel);
void           jce_anim_player_destroy(JceAnimPlayer *player);

/* Playback control. */
void  jce_anim_player_play(JceAnimPlayer *p, const JceAnimClip *clip,
                             bool loop, float speed);
void  jce_anim_player_stop(JceAnimPlayer *p);
void  jce_anim_player_pause(JceAnimPlayer *p, bool paused);
void  jce_anim_player_set_speed(JceAnimPlayer *p, float speed);
void  jce_anim_player_set_time(JceAnimPlayer *p, float time);
float jce_anim_player_get_time(const JceAnimPlayer *p);
bool  jce_anim_player_is_playing(const JceAnimPlayer *p);

/* Advance playback by dt seconds and produce skinning matrices.
 * out_joint_matrices: array of [max_joints] mat4, ready for GPU upload.
 * Returns the number of joints written. */
uint32_t jce_anim_player_update(JceAnimPlayer *p, float dt,
                                  jce_mat4 *out_joint_matrices,
                                  uint32_t max_joints);

/* ================================================================== */
/* Additive / layered blending (FEATURE 3.3)                           */
/* ------------------------------------------------------------------ */
/* Mirror of the PUBLIC animation header's layered-blend API so the
 * implementation TU and src-side consumers (renderer) can use them without
 * including the public header (whose forward-declared typedefs would collide
 * in C99).  See the public header for full documentation. */

typedef struct JceAvatarMask JceAvatarMask;

typedef enum {
    JCE_ANIM_LAYER_ADDITIVE = 0,
    JCE_ANIM_LAYER_OVERRIDE = 1
} JceAnimLayerMode;

typedef struct {
    const JceAnimClip   *clip;
    float                time;
    float                weight;
    JceAnimLayerMode     mode;
    const JceAvatarMask *mask;
    const JceAnimClip   *ref_clip;
    float                ref_time;
} JceAnimLayer;

JCE_API uint32_t JCE_CALL jce_anim_player_blend_additive(
    JceAnimPlayer       *p,
    const JceAnimClip   *base_clip,  float base_time,
    const JceAnimClip   *add_clip,   float add_time,
    const JceAnimClip   *ref_clip,   float ref_time,
    const JceAvatarMask *mask,       float weight,
    jce_mat4            *out_joint_matrices,
    uint32_t             max_joints);

JCE_API uint32_t JCE_CALL jce_anim_player_blend_layers(
    JceAnimPlayer      *p,
    const JceAnimClip  *base_clip, float base_time,
    const JceAnimLayer *layers,    uint32_t num_layers,
    jce_mat4           *out_joint_matrices,
    uint32_t            max_joints);

/* ================================================================== */
/* Root motion (FEATURE 3.2)                                           */
/* ------------------------------------------------------------------ */
/* These mirror the declarations in the PUBLIC animation header
 * (<jce/middleware/animation/jce_animation.h>) so the implementation TU and
 * src-side consumers can use them without including the public header (whose
 * forward-declared typedefs would collide in C99).  See the public header for
 * the full documentation comments. */

typedef struct {
    jce_vec3 translation;   /* root-joint local-space translation delta   */
    float    yaw_delta;     /* root-joint Y-axis (yaw) rotation delta, rad */
    bool     valid;         /* false when clip/skeleton/root were invalid  */
} JceAnimRootDelta;

typedef enum {
    JCE_ROOT_MOTION_NONE     = 0,
    JCE_ROOT_MOTION_RECENTER = 1 << 0,
    JCE_ROOT_MOTION_YAW      = 1 << 1
} JceAnimRootMotionFlags;

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

JCE_API void JCE_CALL jce_anim_player_set_root_motion(JceAnimPlayer *p,
                                                      bool enabled,
                                                      uint32_t root_joint);

JCE_API JceAnimRootDelta JCE_CALL jce_anim_player_consume_root_motion(JceAnimPlayer *p);

#ifdef __cplusplus
}
#endif

#endif /* JCE_ANIMATION_H */
