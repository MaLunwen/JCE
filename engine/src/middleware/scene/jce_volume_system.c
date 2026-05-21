/*
 * jce_volume_system.c  Post-FX volume blending system.
 *
 * Each frame, iterates all entities that carry a JceVolumeComponent,
 * computes a blend weight for each one relative to the camera position,
 * and folds the per-field overrides into an output JcePostFXParams.
 *
 * Blending rules
 * ──────────────
 * • Global volumes (is_global = true) always contribute at weight * global_w.
 * • Box volumes: distance = AABB surface distance from cam to volume box.
 * • Sphere volumes: distance = max(0, dist_to_center - radius).
 * • t = saturate(1 - distance / blend_distance) * weight.
 * • Up to JCE_VOLUME_MAX_LOCAL non-global volumes are evaluated; extras skipped.
 * • For each overridden field, the final value is lerped from the base
 *   param toward the override value by t.  Multiple volumes accumulate
 *   additively; the result is divided by total_weight at the end.
 *
 * Layer: Renderer (L3) — called from jce_scene_renderer.c before postfx.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_postfx.h>

#include <math.h>

#define JCE_VOLUME_MAX_LOCAL 16

/* ── Internal blend accumulator ──────────────────────────────────── */

typedef struct {
    float exposure;
    float gamma;
    float bloom_threshold;
    float bloom_intensity;
    float fxaa_span_max;
    float fxaa_reduce_min;
    float fxaa_reduce_mul;
    float vignette_intensity;
    float vignette_smoothness;
    float chromatic_strength;
    float weight_sum[10]; /* per-field accumulated weights */
} BlendAccum;

static void accum_apply(BlendAccum *acc,
                        const JceVolumeProfile *prof,
                        float t)
{
    if (t <= 0.0f) return;

#define FIELD(IDX, BIT, FIELD_NAME)                             \
    if (prof->enabled_mask & (BIT)) {                           \
        acc->FIELD_NAME      += prof->values.FIELD_NAME * t;    \
        acc->weight_sum[IDX] += t;                              \
    }

    FIELD(0, JCE_VOLUME_OVERRIDE_EXPOSURE,            exposure)
    FIELD(1, JCE_VOLUME_OVERRIDE_GAMMA,               gamma)
    FIELD(2, JCE_VOLUME_OVERRIDE_BLOOM_THRESHOLD,     bloom_threshold)
    FIELD(3, JCE_VOLUME_OVERRIDE_BLOOM_INTENSITY,     bloom_intensity)
    FIELD(4, JCE_VOLUME_OVERRIDE_FXAA_SPAN_MAX,       fxaa_span_max)
    FIELD(5, JCE_VOLUME_OVERRIDE_FXAA_REDUCE_MIN,     fxaa_reduce_min)
    FIELD(6, JCE_VOLUME_OVERRIDE_FXAA_REDUCE_MUL,     fxaa_reduce_mul)
    FIELD(7, JCE_VOLUME_OVERRIDE_VIGNETTE_INTENSITY,  vignette_intensity)
    FIELD(8, JCE_VOLUME_OVERRIDE_VIGNETTE_SMOOTHNESS, vignette_smoothness)
    FIELD(9, JCE_VOLUME_OVERRIDE_CHROMATIC_STRENGTH,  chromatic_strength)
#undef FIELD
}

static void accum_resolve(const BlendAccum *acc, JcePostFXParams *out)
{
#define RESOLVE(IDX, FIELD_NAME)                                    \
    if (acc->weight_sum[IDX] > 0.0f)                               \
        out->FIELD_NAME = out->FIELD_NAME                           \
            + (acc->FIELD_NAME / acc->weight_sum[IDX]              \
               - out->FIELD_NAME) * (acc->weight_sum[IDX] > 1.0f  \
                                     ? 1.0f : acc->weight_sum[IDX]);

    RESOLVE(0, exposure)
    RESOLVE(1, gamma)
    RESOLVE(2, bloom_threshold)
    RESOLVE(3, bloom_intensity)
    RESOLVE(4, fxaa_span_max)
    RESOLVE(5, fxaa_reduce_min)
    RESOLVE(6, fxaa_reduce_mul)
    RESOLVE(7, vignette_intensity)
    RESOLVE(8, vignette_smoothness)
    RESOLVE(9, chromatic_strength)
#undef RESOLVE
}

/* ── Distance helpers ────────────────────────────────────────────── */

/* Unsigned distance from point p to an axis-aligned box centred at
 * origin with half-extents ext.  Returns 0 when inside. */
static float dist_to_box(jce_vec3 p, jce_vec3 ext)
{
    float dx = fabsf(p.x) - ext.x;
    float dy = fabsf(p.y) - ext.y;
    float dz = fabsf(p.z) - ext.z;
    float ox = dx > 0.0f ? dx : 0.0f;
    float oy = dy > 0.0f ? dy : 0.0f;
    float oz = dz > 0.0f ? dz : 0.0f;
    return sqrtf(ox * ox + oy * oy + oz * oz);
}

/* ── Callback context ────────────────────────────────────────────── */

typedef struct {
    jce_vec3     cam_pos;
    BlendAccum   acc;
    int          local_count;
} VolumeTickCtx;

static void volume_entity_cb(JceScene *s, JceEntity e, void *ud)
{
    VolumeTickCtx *ctx = (VolumeTickCtx *)ud;

    const JceVolumeComponent *vol = jce_scene_get_volume(s, e);
    if (!vol) return;

    float t;

    if (vol->is_global) {
        t = vol->weight > 1.0f ? 1.0f : (vol->weight < 0.0f ? 0.0f : vol->weight);
    } else {
        if (ctx->local_count >= JCE_VOLUME_MAX_LOCAL) return;
        ctx->local_count++;

        /* Get entity world position from its transform. */
        const JceTransform *tr = jce_scene_get_transform(s, e);
        jce_vec3 origin = tr ? tr->position : jce_v3(0.0f, 0.0f, 0.0f);
        jce_vec3 local  = jce_v3_sub(ctx->cam_pos, origin);

        float dist;
        if (vol->shape == JCE_VOLUME_SHAPE_SPHERE) {
            float r = vol->extents.x > 0.0f ? vol->extents.x : 0.0f;
            float d = jce_v3_len(local);
            dist = d - r;
            if (dist < 0.0f) dist = 0.0f;
        } else {
            dist = dist_to_box(local, vol->extents);
        }

        float bd = vol->blend_distance > 0.0f ? vol->blend_distance : 1.0f;
        float raw = 1.0f - dist / bd;
        if (raw <= 0.0f) return;
        if (raw > 1.0f) raw = 1.0f;
        t = raw * (vol->weight < 0.0f ? 0.0f : (vol->weight > 1.0f ? 1.0f : vol->weight));
    }

    accum_apply(&ctx->acc, &vol->profile, t);
}

/* ── Public entry point ──────────────────────────────────────────── */

void jce_volume_system_tick(JceScene *scene,
                            jce_vec3 cam_pos,
                            JcePostFXParams *inout_params)
{
    if (!scene || !inout_params) return;

    VolumeTickCtx ctx;
    ctx.cam_pos     = cam_pos;
    ctx.local_count = 0;

    /* Zero-initialise the accumulator. */
    int i;
    float *raw = (float *)&ctx.acc;
    for (i = 0; i < (int)(sizeof(ctx.acc) / sizeof(float)); i++)
        raw[i] = 0.0f;

    jce_scene_each_entity(scene, volume_entity_cb, &ctx);
    accum_resolve(&ctx.acc, inout_params);
}
