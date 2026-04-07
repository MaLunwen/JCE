/*
 * jce_animation.h  Skeletal animation clips and playback.
 *
 * An animation clip contains channels that drive joint TRS properties
 * over time via keyframes. The animation player manages playback state
 * and produces joint matrices each frame.
 *
 * Layer: Graphics (Layer 3).
 */

#ifndef JCE_ANIMATION_H
#define JCE_ANIMATION_H

#include <jce/core/jce_math.h>
#include "jce_skeleton.h"
#include <stdint.h>
#include <stdbool.h>

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

/* Sample the clip at a given time, writing per-joint local transforms.
 * out_locals: array of [num_joints] mat4 (typically skeleton joint count).
 * Joints not affected by this clip are left unchanged;
 * caller should initialize with the skeleton rest pose. */
void jce_anim_clip_sample(const JceAnimClip *clip, float time,
                            jce_mat4 *out_locals, uint32_t num_joints);

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
float jce_anim_player_get_time(const JceAnimPlayer *p);
bool  jce_anim_player_is_playing(const JceAnimPlayer *p);

/* Advance playback by dt seconds and produce skinning matrices.
 * out_joint_matrices: array of [max_joints] mat4, ready for GPU upload.
 * Returns the number of joints written. */
uint32_t jce_anim_player_update(JceAnimPlayer *p, float dt,
                                  jce_mat4 *out_joint_matrices,
                                  uint32_t max_joints);

#ifdef __cplusplus
}
#endif

#endif /* JCE_ANIMATION_H */
