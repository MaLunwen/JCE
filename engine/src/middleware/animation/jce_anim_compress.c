/*
 * jce_anim_compress.c — lossy keyframe reduction (Ramer–Douglas–Peucker).
 *
 * Per track: keep the endpoints, then recursively keep the interior key with
 * the largest reconstruction error against the straight (lerp) / shortest-arc
 * (nlerp) interpolation of its kept neighbours, until every elided key is within
 * tolerance.  Compaction is in place; see the header for the contract.
 *
 * Pure: depends only on jce_math (vec3/quat) + the engine allocator.  No clip /
 * ECS / GPU.  Fully deterministic + headless.
 */

#include <jce/middleware/animation/jce_anim_compress.h>

#include <jce/os/core/jce_math.h>
#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

/* ── Global opt-in state ──────────────────────────────────────────────── */

static bool                  s_enabled;
static JceAnimCompressParams s_params;
static bool                  s_params_set;

void JCE_CALL jce_anim_compress_params_default(JceAnimCompressParams *out)
{
    if (!out) return;
    out->vec3_tolerance     = 0.001f;                 /* ~1 mm                */
    out->quat_tolerance_rad = 0.0087266f;             /* ~0.5 degrees         */
}

void JCE_CALL jce_anim_compress_set_enabled(bool enabled) { s_enabled = enabled; }
bool JCE_CALL jce_anim_compress_is_enabled(void)          { return s_enabled; }

void JCE_CALL jce_anim_compress_set_params(const JceAnimCompressParams *p)
{
    if (!p) return;
    s_params     = *p;
    s_params_set = true;
}

void JCE_CALL jce_anim_compress_get_params(JceAnimCompressParams *out)
{
    if (!out) return;
    if (s_params_set) *out = s_params;
    else              jce_anim_compress_params_default(out);
}

/* ── Reconstruction error helpers ─────────────────────────────────────── */

static float vec3_error(const jce_vec3 *v, uint32_t lo, uint32_t k, uint32_t hi,
                        const float *times)
{
    float denom = times[hi] - times[lo];
    float s     = (denom > 1e-9f) ? (times[k] - times[lo]) / denom : 0.0f;
    jce_vec3 interp = jce_v3_lerp(v[lo], v[hi], s);
    return jce_v3_len(jce_v3_sub(v[k], interp));
}

/* Shortest-arc nlerp of two quats + the angle (radians) between it and `q[k]`. */
static float quat_error(const jce_quat *q, uint32_t lo, uint32_t k, uint32_t hi,
                        const float *times)
{
    float denom = times[hi] - times[lo];
    float s     = (denom > 1e-9f) ? (times[k] - times[lo]) / denom : 0.0f;

    jce_quat a = q[lo], b = q[hi];
    float dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    if (dot < 0.0f) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; } /* shortest path */

    jce_quat m;
    m.x = a.x + (b.x - a.x) * s;
    m.y = a.y + (b.y - a.y) * s;
    m.z = a.z + (b.z - a.z) * s;
    m.w = a.w + (b.w - a.w) * s;
    float len = sqrtf(m.x*m.x + m.y*m.y + m.z*m.z + m.w*m.w);
    if (len < 1e-12f) return 0.0f;
    m.x /= len; m.y /= len; m.z /= len; m.w /= len;

    jce_quat c = q[k];
    float d = fabsf(c.x*m.x + c.y*m.y + c.z*m.z + c.w*m.w);
    if (d > 1.0f) d = 1.0f;
    return 2.0f * acosf(d);    /* geodesic angle between the two rotations */
}

/* ── Track reduction ──────────────────────────────────────────────────── */

uint32_t JCE_CALL
jce_anim_compress_track(float                       *timestamps,
                        void                        *values,
                        uint32_t                     count,
                        JceAnimCompressTarget        target,
                        const JceAnimCompressParams *params)
{
    JceAnimCompressParams p;
    bool      *keep;
    uint32_t  *stack;     /* segment endpoints, pairs (lo,hi) */
    uint32_t   sp = 0;
    uint32_t   i, w;
    const jce_vec3 *vv = (const jce_vec3 *)values;
    const jce_quat *qv = (const jce_quat *)values;
    float      tol;

    if (!timestamps || !values || count <= 2u)
        return count;

    if (params) p = *params;
    else        jce_anim_compress_params_default(&p);
    tol = (target == JCE_ANIM_COMPRESS_QUAT) ? p.quat_tolerance_rad
                                             : p.vec3_tolerance;
    if (tol < 0.0f) tol = 0.0f;

    keep  = (bool *)JCE_CALLOC(count, sizeof(bool));
    stack = (uint32_t *)JCE_CALLOC((size_t)count * 2u, sizeof(uint32_t));
    if (!keep || !stack) {           /* OOM -> leave the track untouched */
        JCE_FREE(keep);
        JCE_FREE(stack);
        return count;
    }

    keep[0] = true;
    keep[count - 1u] = true;
    stack[sp++] = 0u;
    stack[sp++] = count - 1u;

    while (sp >= 2u) {
        uint32_t hi  = stack[--sp];
        uint32_t lo  = stack[--sp];
        float    max = -1.0f;
        uint32_t arg = 0u;
        uint32_t k;

        if (hi <= lo + 1u) continue;     /* no interior keys */

        for (k = lo + 1u; k < hi; ++k) {
            float e = (target == JCE_ANIM_COMPRESS_QUAT)
                          ? quat_error(qv, lo, k, hi, timestamps)
                          : vec3_error(vv, lo, k, hi, timestamps);
            if (e > max) { max = e; arg = k; }
        }

        if (max > tol) {                 /* this key must stay; split around it */
            keep[arg] = true;
            stack[sp++] = lo;  stack[sp++] = arg;
            stack[sp++] = arg; stack[sp++] = hi;
        }
        /* else: every interior key in (lo,hi) is within tolerance -> drop them */
    }

    /* Compact survivors to the front of both arrays. */
    w = 0u;
    for (i = 0u; i < count; ++i) {
        if (!keep[i]) continue;
        if (i != w) {
            timestamps[w] = timestamps[i];
            if (target == JCE_ANIM_COMPRESS_QUAT)
                ((jce_quat *)values)[w] = ((jce_quat *)values)[i];
            else
                ((jce_vec3 *)values)[w] = ((jce_vec3 *)values)[i];
        }
        ++w;
    }

    JCE_FREE(keep);
    JCE_FREE(stack);
    return w;
}
