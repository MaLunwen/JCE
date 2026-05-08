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

/* Animation event — fired when playback time crosses `time`.
 * Mirror of the public API definition in
 * <jce/middleware/animation/jce_animation.h>. */
#ifndef JCE_ANIM_MAX_EVENTS
#define JCE_ANIM_MAX_EVENTS 32
#endif
typedef struct {
    float time;
    char  name[48];
    int   int_payload;
    float float_payload;
} JceAnimEvent;
typedef void (*JceAnimEventFn)(const JceAnimEvent *evt, void *user_data);

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

#ifdef __cplusplus
}
#endif

#endif /* JCE_ANIMATION_H */
