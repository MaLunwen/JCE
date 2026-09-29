/*
 * api_animation.h  Layer 3 — Animation system.
 *
 * Skeletal animation, skinned meshes, animation playback and blending.
 * Uses ozz-animation behind a C99 bridge for sampling and blending.
 */

#ifndef JCE_API_ANIMATION_H
#define JCE_API_ANIMATION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/middleware/animation/jce_animation.h>
#include <jce/middleware/animation/jce_anim_clip_io.h>
#include <jce/middleware/animation/jce_anim_compress.h>
#include <jce/middleware/animation/jce_anim_fbbik.h>
#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/renderer/jce_skinned_mesh.h>

/* Completed 2026-08-31.  These headers export JCE_API symbols and were
 * reachable from NO umbrella, so §4's promise -- `#include <jce/api.h>`
 * gives you the whole engine -- did not hold for them.  Several are named
 * in §4's own table by capability.
 */
#include <jce/middleware/animation/jce_anim_blend_tree.h>
#include <jce/middleware/animation/jce_anim_foot_ik.h>
#include <jce/middleware/animation/jce_anim_sm.h>
#include <jce/middleware/animation/jce_anim_sm_binding.h>
#include <jce/middleware/animation/jce_avatar.h>
#include <jce/middleware/animation/jce_humanoid.h>
#include <jce/middleware/animation/jce_avatar_mask.h>



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_ANIMATION_H */
