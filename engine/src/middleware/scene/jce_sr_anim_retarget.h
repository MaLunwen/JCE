/*
 * jce_sr_anim_retarget.h -- playing a clip authored for ANOTHER rig.
 *
 * WHY THIS IS A SEPARATE FILE.  jce_sr_anim.c crossed AGENTS.md's 3000-line
 * cap when the role-based retarget path landed in it.  That happened twice to
 * jce_ui_canvas.c and the answer was the same both times: raising the cap
 * would have been cheaper and wrong, so the newest self-contained question is
 * the code that leaves.
 *
 * THE SEAM IS SEVEN VALUES AND NO FRAME STRUCT, which is this module's own
 * test for a seam (see jce_ui_canvas_fitter.h).  Everything the function calls
 * -- sr_get_model, the animation player, both retargeters -- is already
 * declared for the whole module in jce_sr_internal.h.
 */
#ifndef JCE_SR_ANIM_RETARGET_H
#define JCE_SR_ANIM_RETARGET_H

#include "jce_sr_internal.h"

/* Play the active clip through a retarget when the component names a SOURCE
 * skeleton different from the entity's own.
 *
 * Returns true when it handled the instance -- including when it could not
 * finish this frame (model still loading, map build failed), because falling
 * back to legacy playback there would sample a clip against the WRONG rig.
 * False means there is no retarget to do and the caller should play normally. */
bool sr_anim_try_retarget(JceSceneRenderer *sr,
                          JceScene *scene, JceEntity e,
                          JceSkeletalAnimatorComponent *sa,
                          SrModelCache *dst_mc, SrAnimInstance *ai,
                          float dt_sec);

#endif /* JCE_SR_ANIM_RETARGET_H */
