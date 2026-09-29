/*
 * jce_animation.h  Skeletal animation clip authoring (engine-internal).
 *
 * An animation clip contains channels that drive joint TRS properties
 * over time via keyframes. Importers build clips from those channels and
 * sample them; playback (JceAnimPlayer) is the PUBLIC header's business.
 *
 * Layer: Animation (Layer 3).
 */

#ifndef JCE_ANIMATION_H
#define JCE_ANIMATION_H

/* The PUBLIC animation header is the single source of truth for everything
 * shared with SDK consumers: the JceAnimClip / JceAnimPlayer handles, playback
 * control, layered blending and root motion.  This header only ADDS the
 * engine-internal clip AUTHORING and sampling API on top of it — nothing
 * declared there may be repeated here, or the two silently drift. */
#include <jce/middleware/animation/jce_animation.h>
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

/* Create an animation clip. Copies all channel data; caller retains ownership. */
JceAnimClip *jce_anim_clip_create(const char *name,
                                    const JceAnimChannel *channels,
                                    uint32_t num_channels,
                                    float duration);

void         jce_anim_clip_destroy(JceAnimClip *clip);

/* Number of TRS channels in the clip (0 if NULL).  Used by importers / tests to
 * verify a clip carries joint tracks. */
uint32_t     jce_anim_clip_channel_count(const JceAnimClip *clip);

/* The clip's channels, or NULL.  Valid until jce_anim_clip_destroy.
 *
 * Added so jce_anim_clip_io.c can WRITE a clip without a second copy of
 * `struct JceAnimClip`: the struct is private to jce_animation.c, and a
 * serialiser that redeclared it would be a second definition of the layout --
 * which compiles, and then disagrees the first time a field moves. */
const JceAnimChannel *jce_anim_clip_channels(const JceAnimClip *clip);

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

#ifdef __cplusplus
}
#endif

#endif /* JCE_ANIMATION_H */
