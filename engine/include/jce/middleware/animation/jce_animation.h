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
void  jce_anim_player_play(JceAnimPlayer *p, const JceAnimClip *clip,
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

/* -- Skeleton queries ---------------------------------------------- */

/* Return the number of joints in a skeleton. */
JCE_API uint32_t jce_skeleton_joint_count(const JceSkeleton *skel);

/* Return the name of a skeleton joint by index (or NULL). */
JCE_API const char *jce_skeleton_joint_name(const JceSkeleton *skel, uint32_t index);

JCE_EXTERN_C_END

#endif /* JCE_ANIMATION_PUBLIC_H */
