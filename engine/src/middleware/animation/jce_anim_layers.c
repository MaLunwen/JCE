/*
 * jce_anim_layers.c  Multi-layer animation stack.
 *
 * Each layer samples its clip into a per-stack scratch buffer, then
 * blends element-wise into the running result with weight * mask[i].
 *
 * The blend is performed on TRS-decomposed transforms to avoid the
 * artefacts of element-wise lerping rotation matrices.  Translation
 * lerps linearly, rotation slerps (via the existing math helpers),
 * scale lerps linearly.
 */

#include <jce/middleware/animation/jce_anim_layers.h>

#include "jce_animation.h"          /* jce_anim_clip_sample */
#include <jce/middleware/animation/jce_skeleton.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

typedef struct {
    const JceAnimClip *clip;
    float              time;
    float              weight;
    JceAnimBlendMode   mode;
    float             *bone_mask;     /* NULL = full body (1.0 everywhere) */
} Layer;

struct JceAnimLayerStack {
    uint32_t  num_joints;
    Layer     layers[JCE_ANIM_LAYER_MAX];
    /* Scratch sized to num_joints, reused across layers within one
     * evaluate().  Allocated lazily. */
    jce_mat4 *scratch;
};

/* ── helpers ─────────────────────────────────────────────────────── */

static jce_vec3 mat4_translation(const jce_mat4 *m)
{
    return jce_v3(m->raw[3][0], m->raw[3][1], m->raw[3][2]);
}

static jce_quat mat4_rotation(const jce_mat4 *m)
{
    return jce_m4_to_quat(m);
}

static jce_vec3 mat4_scale(const jce_mat4 *m)
{
    return jce_m4_extract_scale(m);
}

static jce_vec3 vec3_lerp(jce_vec3 a, jce_vec3 b, float t)
{
    jce_vec3 r;
    r.x = a.x + (b.x - a.x) * t;
    r.y = a.y + (b.y - a.y) * t;
    r.z = a.z + (b.z - a.z) * t;
    return r;
}

static jce_quat quat_slerp(jce_quat a, jce_quat b, float t)
{
    /* Take shortest path. */
    float dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    if (dot < 0.0f) {
        b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w;
        dot = -dot;
    }
    if (dot > 0.9995f) {
        /* Near-parallel — fall back to nlerp. */
        jce_quat r;
        r.x = a.x + (b.x - a.x) * t;
        r.y = a.y + (b.y - a.y) * t;
        r.z = a.z + (b.z - a.z) * t;
        r.w = a.w + (b.w - a.w) * t;
        return jce_q_normalize(r);
    }
    float theta = acosf(dot);
    float sin_t = sinf(theta);
    float wa = sinf((1.0f - t) * theta) / sin_t;
    float wb = sinf(t * theta) / sin_t;
    jce_quat r;
    r.x = a.x*wa + b.x*wb;
    r.y = a.y*wa + b.y*wb;
    r.z = a.z*wa + b.z*wb;
    r.w = a.w*wa + b.w*wb;
    return jce_q_normalize(r);
}

/* ── public ──────────────────────────────────────────────────────── */

JceAnimLayerStack *jce_anim_layer_stack_create(uint32_t num_joints)
{
    if (num_joints == 0) return NULL;
    JceAnimLayerStack *s = (JceAnimLayerStack *)JCE_CALLOC(1, sizeof(*s));
    if (!s) return NULL;
    s->num_joints = num_joints;
    /* scratch is lazy-allocated on first evaluate to keep create
     * cheap when many entities have empty layer stacks. */
    return s;
}

void jce_anim_layer_stack_destroy(JceAnimLayerStack *s)
{
    if (!s) return;
    for (uint32_t i = 0; i < JCE_ANIM_LAYER_MAX; ++i)
        JCE_FREE(s->layers[i].bone_mask);
    JCE_FREE(s->scratch);
    JCE_FREE(s);
}

uint32_t jce_anim_layer_stack_count(const JceAnimLayerStack *s)
{
    return s ? JCE_ANIM_LAYER_MAX : 0u;
}

bool jce_anim_layer_set(JceAnimLayerStack *s, uint32_t idx,
                        const JceAnimClip *clip,
                        float time, float weight,
                        JceAnimBlendMode mode)
{
    if (!s || idx >= JCE_ANIM_LAYER_MAX) return false;
    s->layers[idx].clip   = clip;
    s->layers[idx].time   = time;
    s->layers[idx].weight = weight < 0.0f ? 0.0f : (weight > 1.0f ? 1.0f : weight);
    s->layers[idx].mode   = mode;
    return true;
}

bool jce_anim_layer_set_bone_mask(JceAnimLayerStack *s, uint32_t idx,
                                  const float *mask)
{
    if (!s || idx >= JCE_ANIM_LAYER_MAX) return false;
    if (!mask) {
        JCE_FREE(s->layers[idx].bone_mask);
        s->layers[idx].bone_mask = NULL;
        return true;
    }
    if (!s->layers[idx].bone_mask) {
        s->layers[idx].bone_mask =
            (float *)JCE_CALLOC(s->num_joints, sizeof(float));
        if (!s->layers[idx].bone_mask) return false;
    }
    memcpy(s->layers[idx].bone_mask, mask, s->num_joints * sizeof(float));
    return true;
}

void jce_anim_layer_stack_evaluate(JceAnimLayerStack    *s,
                                   const JceSkeleton    *skel,
                                   const jce_mat4       *base_locals,
                                   jce_mat4             *out_locals)
{
    if (!s || !skel || !base_locals || !out_locals) return;

    if (out_locals != base_locals)
        memcpy(out_locals, base_locals, s->num_joints * sizeof(jce_mat4));

    /* Lazy scratch alloc. */
    if (!s->scratch) {
        s->scratch = (jce_mat4 *)JCE_MALLOC(s->num_joints * sizeof(jce_mat4));
        if (!s->scratch) return;
    }

    const jce_vec3 *rt = NULL;
    const jce_quat *rr = NULL;
    const jce_vec3 *rs = NULL;
    jce_skeleton_rest_trs(skel, &rt, &rr, &rs);

    for (uint32_t li = 0; li < JCE_ANIM_LAYER_MAX; ++li) {
        const Layer *L = &s->layers[li];
        if (!L->clip || L->weight <= 0.0f) continue;

        /* Sample layer clip on top of rest pose. */
        const jce_mat4 *rest_pose = jce_skeleton_rest_pose(skel);
        if (rest_pose)
            memcpy(s->scratch, rest_pose, s->num_joints * sizeof(jce_mat4));
        jce_anim_clip_sample(L->clip, L->time, s->scratch, s->num_joints,
                             rt, rr, rs);

        /* Compose into out per joint. */
        for (uint32_t j = 0; j < s->num_joints; ++j) {
            float w = L->weight;
            if (L->bone_mask) w *= L->bone_mask[j];
            if (w <= 0.0f) continue;

            jce_vec3 ot = mat4_translation(&out_locals[j]);
            jce_quat or_ = mat4_rotation   (&out_locals[j]);
            jce_vec3 os = mat4_scale       (&out_locals[j]);

            jce_vec3 lt = mat4_translation(&s->scratch[j]);
            jce_quat lr_ = mat4_rotation  (&s->scratch[j]);
            jce_vec3 ls = mat4_scale      (&s->scratch[j]);

            jce_vec3 nt = vec3_lerp(ot, lt, w);
            jce_quat nr = quat_slerp(or_, lr_, w);
            jce_vec3 ns = vec3_lerp(os, ls, w);

            /* OVERRIDE and ADDITIVE produce the same blend in this
             * first-cut implementation; ADDITIVE is reserved for a
             * future delta-from-rest formulation that requires
             * per-clip authoring conventions to be settled. */
            (void)L->mode;

            out_locals[j] = jce_m4_from_trs(nt, nr, ns);
        }
    }
}
