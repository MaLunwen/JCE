/*
 * jce_morph.c  Morph targets / blendshapes — bgfx-free core (FEATURE 3.1).
 *
 * Owns imported per-target POSITION/NORMAL delta arrays, the glTF base weights,
 * and an optional keyframed weights track sampled from the "weights" animation
 * channel.  Provides the CPU evaluator that applies weighted deltas to a base
 * vertex buffer (out = base + sum_i weight_i * delta_i) before skinning.
 */

#include <jce/middleware/animation/jce_morph.h>

#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

#define LOG_TAG "jce_morph"

/* ================================================================== */
/* Morph delta storage                                                 */
/* ================================================================== */

struct JceMorphData {
    uint32_t  num_targets;
    uint32_t  num_verts;
    bool      has_normals;
    jce_vec3 *positions;    /* [num_targets * num_verts] target-major, owned */
    jce_vec3 *normals;      /* [num_targets * num_verts] or NULL,      owned */
    float    *base_weights; /* [num_targets] glTF default weights,     owned */
};

JceMorphData *JCE_CALL jce_morph_data_create(uint32_t num_targets,
                                             uint32_t num_verts,
                                             bool has_normals)
{
    if (num_targets == 0 || num_verts == 0) return NULL;

    /* Guard the target-major delta-array element count against 32-bit overflow
     * before allocating (untrusted glTF can claim huge counts). */
    uint64_t elems = (uint64_t)num_targets * (uint64_t)num_verts;
    if (elems > (uint64_t)(SIZE_MAX / sizeof(jce_vec3)))
        return NULL;

    JceMorphData *m = (JceMorphData *)JCE_CALLOC(1, sizeof(*m));
    if (!m) return NULL;

    m->num_targets = num_targets;
    m->num_verts   = num_verts;
    m->has_normals = has_normals;

    m->positions = (jce_vec3 *)JCE_CALLOC((size_t)elems, sizeof(jce_vec3));
    if (!m->positions) { jce_morph_data_destroy(m); return NULL; }

    if (has_normals) {
        m->normals = (jce_vec3 *)JCE_CALLOC((size_t)elems, sizeof(jce_vec3));
        if (!m->normals) { jce_morph_data_destroy(m); return NULL; }
    }

    m->base_weights = (float *)JCE_CALLOC(num_targets, sizeof(float));
    if (!m->base_weights) { jce_morph_data_destroy(m); return NULL; }

    return m;
}

void JCE_CALL jce_morph_data_destroy(JceMorphData *m)
{
    if (!m) return;
    JCE_FREE(m->positions);
    JCE_FREE(m->normals);
    JCE_FREE(m->base_weights);
    JCE_FREE(m);
}

uint32_t JCE_CALL jce_morph_target_count(const JceMorphData *m)
{
    return m ? m->num_targets : 0;
}

uint32_t JCE_CALL jce_morph_vertex_count(const JceMorphData *m)
{
    return m ? m->num_verts : 0;
}

bool JCE_CALL jce_morph_has_normals(const JceMorphData *m)
{
    return (m && m->normals) ? true : false;
}

void JCE_CALL jce_morph_set_position_deltas(JceMorphData *m, uint32_t target,
                                            const jce_vec3 *deltas,
                                            uint32_t count)
{
    if (!m || !deltas || target >= m->num_targets) return;
    uint32_t n = count < m->num_verts ? count : m->num_verts;
    memcpy(&m->positions[(size_t)target * m->num_verts], deltas,
           (size_t)n * sizeof(jce_vec3));
}

void JCE_CALL jce_morph_set_normal_deltas(JceMorphData *m, uint32_t target,
                                          const jce_vec3 *deltas,
                                          uint32_t count)
{
    if (!m || !m->normals || !deltas || target >= m->num_targets) return;
    uint32_t n = count < m->num_verts ? count : m->num_verts;
    memcpy(&m->normals[(size_t)target * m->num_verts], deltas,
           (size_t)n * sizeof(jce_vec3));
}

const jce_vec3 *JCE_CALL jce_morph_position_deltas(const JceMorphData *m,
                                                   uint32_t target)
{
    if (!m || target >= m->num_targets) return NULL;
    return &m->positions[(size_t)target * m->num_verts];
}

const jce_vec3 *JCE_CALL jce_morph_normal_deltas(const JceMorphData *m,
                                                 uint32_t target)
{
    if (!m || !m->normals || target >= m->num_targets) return NULL;
    return &m->normals[(size_t)target * m->num_verts];
}

void JCE_CALL jce_morph_set_base_weight(JceMorphData *m, uint32_t target, float w)
{
    if (!m || target >= m->num_targets) return;
    m->base_weights[target] = w;
}

float JCE_CALL jce_morph_base_weight(const JceMorphData *m, uint32_t target)
{
    if (!m || target >= m->num_targets) return 0.0f;
    return m->base_weights[target];
}

uint32_t JCE_CALL jce_morph_copy_base_weights(const JceMorphData *m,
                                              float *out, uint32_t max_out)
{
    if (!m || !out) return 0;
    uint32_t n = m->num_targets < max_out ? m->num_targets : max_out;
    if (n > 0) memcpy(out, m->base_weights, (size_t)n * sizeof(float));
    return n;
}

/* ================================================================== */
/* CPU evaluator                                                       */
/* ================================================================== */

bool JCE_CALL jce_morph_apply(const JceMorphData *m,
                              const float *weights, uint32_t num_weights,
                              const void *base, void *out,
                              uint32_t num_verts, uint32_t stride,
                              int32_t pos_offset, int32_t normal_offset)
{
    if (!base || !out || num_verts == 0 || stride == 0) return false;
    if (pos_offset < 0) return false;

    const uint8_t *src = (const uint8_t *)base;
    uint8_t       *dst = (uint8_t *)out;

    /* Without morph data or weights, the operation is a straight copy (still
     * useful so callers can unconditionally route through this path). */
    bool do_morph = (m != NULL && m->num_targets > 0 && weights != NULL &&
                     num_weights > 0);

    uint32_t nt = 0;
    if (do_morph) {
        nt = m->num_targets < num_weights ? m->num_targets : num_weights;
        /* Vertex count must agree with the imported deltas. */
        if (num_verts > m->num_verts) num_verts = m->num_verts;
    }

    bool morph_normals = do_morph && m->normals && normal_offset >= 0;

    for (uint32_t v = 0; v < num_verts; ++v) {
        const uint8_t *sv = src + (size_t)v * stride;
        uint8_t       *dv = dst + (size_t)v * stride;

        /* Position. */
        const float *sp = (const float *)(sv + pos_offset);
        float        px = sp[0], py = sp[1], pz = sp[2];

        if (do_morph) {
            for (uint32_t t = 0; t < nt; ++t) {
                float w = weights[t];
                if (w == 0.0f) continue;
                const jce_vec3 *dp = &m->positions[(size_t)t * m->num_verts + v];
                px += w * dp->x;
                py += w * dp->y;
                pz += w * dp->z;
            }
        }

        float *dp_out = (float *)(dv + pos_offset);
        dp_out[0] = px; dp_out[1] = py; dp_out[2] = pz;

        /* Normal (optional). */
        if (normal_offset >= 0) {
            const float *sn = (const float *)(sv + normal_offset);
            float nx = sn[0], ny = sn[1], nz = sn[2];

            if (morph_normals) {
                for (uint32_t t = 0; t < nt; ++t) {
                    float w = weights[t];
                    if (w == 0.0f) continue;
                    const jce_vec3 *dn =
                        &m->normals[(size_t)t * m->num_verts + v];
                    nx += w * dn->x;
                    ny += w * dn->y;
                    nz += w * dn->z;
                }
                float len = sqrtf(nx * nx + ny * ny + nz * nz);
                if (len > 1e-8f) { nx /= len; ny /= len; nz /= len; }
            }

            float *dn_out = (float *)(dv + normal_offset);
            dn_out[0] = nx; dn_out[1] = ny; dn_out[2] = nz;
        }
    }

    return true;
}

/* ================================================================== */
/* Per-instance weight resolution (track ⊕ authored static weights)    */
/* ================================================================== */

uint32_t JCE_CALL jce_morph_resolve_weights(
    const float *track, const float *authored, uint32_t override_mask,
    uint32_t count, float *out, uint32_t max_out)
{
    uint32_t n;
    uint32_t t;

    if (!out) return 0;

    n = count;
    if (n > JCE_MORPH_MAX_WEIGHTS) n = JCE_MORPH_MAX_WEIGHTS;
    if (n > max_out)               n = max_out;

    for (t = 0; t < n; ++t) {
        bool overridden = (authored != NULL) &&
                          ((override_mask & (UINT32_C(1) << t)) != 0);
        out[t] = overridden ? authored[t] : (track ? track[t] : 0.0f);
    }
    return n;
}

/* ================================================================== */
/* Keyframed weights track                                            */
/* ================================================================== */

struct JceMorphWeightTrack {
    uint32_t        num_targets;
    uint32_t        num_keys;
    float          *timestamps;  /* [num_keys] ascending,          owned */
    float          *values;      /* [num_keys * num_targets] key-major, owned */
    JceMorphInterp  interp;
    float           duration;
};

JceMorphWeightTrack *JCE_CALL jce_morph_weight_track_create(
    uint32_t num_targets, uint32_t num_keys,
    const float *timestamps, const float *values,
    JceMorphInterp interp)
{
    if (num_targets == 0 || num_keys == 0 || !timestamps || !values)
        return NULL;

    uint64_t vcount = (uint64_t)num_keys * (uint64_t)num_targets;
    if (vcount > (uint64_t)(SIZE_MAX / sizeof(float)))
        return NULL;

    JceMorphWeightTrack *t =
        (JceMorphWeightTrack *)JCE_CALLOC(1, sizeof(*t));
    if (!t) return NULL;

    t->num_targets = num_targets;
    t->num_keys    = num_keys;
    t->interp      = interp;

    t->timestamps = (float *)JCE_MALLOC((size_t)num_keys * sizeof(float));
    t->values     = (float *)JCE_MALLOC((size_t)vcount * sizeof(float));
    if (!t->timestamps || !t->values) {
        jce_morph_weight_track_destroy(t);
        return NULL;
    }
    memcpy(t->timestamps, timestamps, (size_t)num_keys * sizeof(float));
    memcpy(t->values, values, (size_t)vcount * sizeof(float));

    t->duration = t->timestamps[num_keys - 1];
    return t;
}

void JCE_CALL jce_morph_weight_track_destroy(JceMorphWeightTrack *t)
{
    if (!t) return;
    JCE_FREE(t->timestamps);
    JCE_FREE(t->values);
    JCE_FREE(t);
}

uint32_t JCE_CALL jce_morph_weight_track_targets(const JceMorphWeightTrack *t)
{
    return t ? t->num_targets : 0;
}

uint32_t JCE_CALL jce_morph_weight_track_keys(const JceMorphWeightTrack *t)
{
    return t ? t->num_keys : 0;
}

float JCE_CALL jce_morph_weight_track_duration(const JceMorphWeightTrack *t)
{
    return t ? t->duration : 0.0f;
}

/* Largest index k where timestamps[k] <= time (mirrors find_keyframe in
 * jce_animation.c). */
static uint32_t morph_find_key(const float *ts, uint32_t count, float time)
{
    if (count == 0) return 0;
    if (time <= ts[0]) return 0;
    if (time >= ts[count - 1]) return count - 1;

    uint32_t lo = 0, hi = count - 1;
    while (lo + 1 < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (ts[mid] <= time) lo = mid; else hi = mid;
    }
    return lo;
}

uint32_t JCE_CALL jce_morph_weight_track_sample(const JceMorphWeightTrack *t,
                                                float time,
                                                float *out, uint32_t max_out)
{
    if (!t || !out || max_out == 0) return 0;

    uint32_t nt = t->num_targets < max_out ? t->num_targets : max_out;
    uint32_t k  = morph_find_key(t->timestamps, t->num_keys, time);

    const float *vk = &t->values[(size_t)k * t->num_targets];

    if (t->interp == JCE_MORPH_INTERP_STEP || k + 1 >= t->num_keys) {
        memcpy(out, vk, (size_t)nt * sizeof(float));
        return nt;
    }

    float dt   = t->timestamps[k + 1] - t->timestamps[k];
    float frac = (dt > 1e-8f) ? (time - t->timestamps[k]) / dt : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    const float *vk1 = &t->values[(size_t)(k + 1) * t->num_targets];
    for (uint32_t i = 0; i < nt; ++i)
        out[i] = vk[i] + (vk1[i] - vk[i]) * frac;

    return nt;
}
