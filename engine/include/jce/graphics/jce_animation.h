/*
 * jce_animation.h  Public animation playback API.
 *
 * Provides animation player for skeletal animation of JceModel instances.
 * Layer: Graphics (Layer 3) — public.
 */

#ifndef JCE_ANIMATION_PUBLIC_H
#define JCE_ANIMATION_PUBLIC_H

#include <jce/core/jce_math.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceAnimClip   JceAnimClip;
typedef struct JceAnimPlayer JceAnimPlayer;
typedef struct JceSkeleton   JceSkeleton;

/* -- Clip queries -------------------------------------------------- */

const char *jce_anim_clip_name(const JceAnimClip *clip);
float       jce_anim_clip_duration(const JceAnimClip *clip);

/* -- Animation player ---------------------------------------------- */

/* Create a player bound to a skeleton. */
JceAnimPlayer *jce_anim_player_create(const JceSkeleton *skel);
void           jce_anim_player_destroy(JceAnimPlayer *player);

/* Start playing a clip. loop=true for continuous, speed=1.0 for normal. */
void  jce_anim_player_play(JceAnimPlayer *p, const JceAnimClip *clip,
                           bool loop, float speed);
void  jce_anim_player_stop(JceAnimPlayer *p);
void  jce_anim_player_pause(JceAnimPlayer *p, bool paused);
void  jce_anim_player_set_speed(JceAnimPlayer *p, float speed);
float jce_anim_player_get_time(const JceAnimPlayer *p);
bool  jce_anim_player_is_playing(const JceAnimPlayer *p);

/* Advance by dt seconds and produce joint matrices for GPU upload.
 * Returns number of joints written to out_joint_matrices. */
uint32_t jce_anim_player_update(JceAnimPlayer *p, float dt,
                                jce_mat4 *out_joint_matrices,
                                uint32_t max_joints);

/* -- Skeleton queries ---------------------------------------------- */

/* Return the number of joints in a skeleton. */
uint32_t jce_skeleton_joint_count(const JceSkeleton *skel);

/* Return the name of a skeleton joint by index (or NULL). */
const char *jce_skeleton_joint_name(const JceSkeleton *skel, uint32_t index);

#ifdef __cplusplus
}
#endif

#endif /* JCE_ANIMATION_PUBLIC_H */
