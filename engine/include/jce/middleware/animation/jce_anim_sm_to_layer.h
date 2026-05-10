/*
 * jce_anim_sm_to_layer.h  Bridge AnimSm output → AnimLayerStack input.
 *
 * The animation state machine (jce_anim_sm.h) reports the current
 * state's clip + time + crossfade info via JceAnimSmEval.  The layer
 * stack (jce_anim_layers.h) wants (clip, time, weight, blend_mode) per
 * layer slot.  This thin helper writes the SM's output into layer 0
 * (and 1 during crossfades) so the same skeleton can be driven by the
 * SM with optional layered overrides on top (e.g. layer 2 = aim
 * override on upper body).
 *
 * Caller-supplied resolver maps clip path strings to JceAnimClip*.
 *
 * Layer: middleware / animation (Layer 3) — public.
 */

#ifndef JCE_ANIM_SM_TO_LAYER_H
#define JCE_ANIM_SM_TO_LAYER_H

#include <jce/middleware/animation/jce_anim_layers.h>
#include <jce/middleware/animation/jce_anim_layer_apply.h>
#include <jce/middleware/animation/jce_anim_sm.h>
#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

/* Apply the SM's current eval to layers 0 and 1 of `stack`.  During a
 * crossfade, layer 0 carries the from-state at weight (1 - blend) and
 * layer 1 carries the to-state at weight `blend`; otherwise layer 0
 * is the active clip at weight 1 and layer 1 is cleared.  Game/editor
 * code remains free to populate layers 2..7 with custom overrides. */
JCE_API void jce_anim_sm_to_layer(const JceAnimSm     *sm,
                                  JceAnimLayerStack   *stack,
                                  JceAnimClipResolveFn resolve,
                                  void                *resolve_ud);

JCE_EXTERN_C_END

#endif /* JCE_ANIM_SM_TO_LAYER_H */
