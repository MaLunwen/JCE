/*
 * jce_anim_sm_to_layer.c  Push SM eval into layer stack slots 0/1.
 *
 * Maps the JceAnimSmEval (current clip + crossfade) onto the first
 * one or two layers of a JceAnimLayerStack so the layer pipeline can
 * blend SM output with override layers (aim, additive, mask) authored
 * via the JceAnimationLayerStateComponent.
 */

#include <jce/middleware/animation/jce_anim_sm_to_layer.h>

void jce_anim_sm_to_layer(const JceAnimSm *sm, JceAnimLayerStack *stack,
                          JceAnimClipResolveFn resolve, void *resolve_ud)
{
    if (!sm || !stack || !resolve) return;

    JceAnimSmEval ev;
    jce_anim_sm_eval(sm, &ev);

    if (ev.state_index < 0 || !ev.clip_path) {
        /* No active state — clear both slots. */
        jce_anim_layer_set(stack, 0, NULL, 0.0f, 0.0f, JCE_ANIM_BLEND_OVERRIDE);
        jce_anim_layer_set(stack, 1, NULL, 0.0f, 0.0f, JCE_ANIM_BLEND_OVERRIDE);
        return;
    }

    if (ev.transition_index >= 0 && ev.from_state >= 0 && ev.to_state >= 0) {
        /* Crossfade: layer 0 from-state, layer 1 to-state. */
        const char *from_clip = jce_anim_sm_state_clip(sm, ev.from_state);
        const char *to_clip   = jce_anim_sm_state_clip(sm, ev.to_state);
        const JceAnimClip *fa = from_clip ? resolve(from_clip, resolve_ud) : NULL;
        const JceAnimClip *fb = to_clip   ? resolve(to_clip,   resolve_ud) : NULL;
        float w_from = 1.0f - ev.blend;
        float w_to   = ev.blend;
        jce_anim_layer_set(stack, 0, fa, ev.state_time, w_from, JCE_ANIM_BLEND_OVERRIDE);
        jce_anim_layer_set(stack, 1, fb, ev.state_time, w_to,   JCE_ANIM_BLEND_OVERRIDE);
    } else {
        /* Single active state. */
        const JceAnimClip *clip = resolve(ev.clip_path, resolve_ud);
        jce_anim_layer_set(stack, 0, clip, ev.state_time, 1.0f, JCE_ANIM_BLEND_OVERRIDE);
        jce_anim_layer_set(stack, 1, NULL, 0.0f, 0.0f, JCE_ANIM_BLEND_OVERRIDE);
    }
}
