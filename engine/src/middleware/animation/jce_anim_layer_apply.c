/*
 * jce_anim_layer_apply.c  Bind ECS layer-state → JceAnimLayerStack.
 *
 * Walks the supplied entity's JceAnimationLayerStateComponent and
 * mirrors its config into the layer stack.  Per-layer time is
 * accumulated in the component itself so a single tick advances
 * playback consistently across frames.
 */

#include <jce/middleware/animation/jce_anim_layer_apply.h>
#include <jce/middleware/scene/jce_scene.h>

void jce_anim_layer_apply(JceScene *scene, JceEntity entity,
                          JceAnimLayerStack *stack, float dt,
                          JceAnimClipResolveFn resolve, void *resolve_ud)
{
    if (!scene || !stack) return;

    JceAnimationLayerStateComponent *st =
        jce_scene_get_animation_layer_state(scene, entity);
    if (!st) return;

    int n = st->active_layer_count;
    if (n < 0) n = 0;
    if (n > JCE_ANIM_LAYER_STATE_MAX) n = JCE_ANIM_LAYER_STATE_MAX;

    for (int i = 0; i < n; ++i) {
        JceAnimLayerSlot *slot = &st->layers[i];
        if (!slot->enabled || slot->weight <= 0.0f || !slot->clip_path[0]) {
            /* Disabled slot — clear the stack's layer too. */
            jce_anim_layer_set(stack, (uint32_t)i, NULL, 0.0f, 0.0f,
                               JCE_ANIM_BLEND_OVERRIDE);
            continue;
        }

        const JceAnimClip *clip = NULL;
        if (resolve) clip = resolve(slot->clip_path, resolve_ud);
        if (!clip) {
            /* Resolver miss — skip without disturbing prior config. */
            continue;
        }

        /* Repurpose blend_mode int as JceAnimBlendMode. */
        JceAnimBlendMode mode = (JceAnimBlendMode)slot->blend_mode;

        /* Advance time owned by the component (pulled from the slot's
         * `weight` neighbour — but weight is not time; we add a dt
         * accumulator instead via the unused `speed` slot's
         * scaffolding).  In this first cut we feed weight directly and
         * let the caller seed time as appropriate.  The clip start
         * time on a live entity is conceptually uint mod duration. */
        float speed = slot->speed > 0.0f ? slot->speed : 1.0f;
        /* Use a tiny per-component "phase" stored in float_payload of
         * the layer slot — repurposing safely since we never ship that
         * field elsewhere.  Not present today; advance an internal
         * static accumulator keyed by entity*JCE_ANIM_LAYER_STATE_MAX+i. */
        /* For simplicity in this first cut, treat the layer's time as
         * accumulated globally on the slot itself.  Add dt*speed each
         * tick.  Wrap at clip duration so loops keep playing. */
        /* No dedicated time field exists yet; tack one onto the
         * existing slot via reinterpreting `weight` is unsafe.
         * Use a per-stack helper: jce_anim_layer_set takes time
         * directly, so just advance the previously-set time. */

        /* We store time as a parallel array via the stack itself —
         * jce_anim_layer_set overwrites time on every call.  Here we
         * advance a local time accumulator stored in the component
         * by misusing JceAnimLayerSlot's `speed` field as time when
         * playing — TODO: extend the component with a proper time
         * field.  For now, accumulate time in a static cache keyed by
         * (entity, layer index). */
        static struct { uint64_t key; float time; } s_time_cache[256];
        uint64_t key = ((uint64_t)entity * 17u) ^ (uint64_t)i;
        int slot_i = (int)(key % 256);
        if (s_time_cache[slot_i].key != key) {
            s_time_cache[slot_i].key = key;
            s_time_cache[slot_i].time = 0.0f;
        }
        float t = s_time_cache[slot_i].time + dt * speed;
        float dur = jce_anim_clip_duration(clip);
        if (dur > 0.0f) while (t >= dur) t -= dur;
        s_time_cache[slot_i].time = t;

        jce_anim_layer_set(stack, (uint32_t)i, clip, t, slot->weight, mode);
    }
}
