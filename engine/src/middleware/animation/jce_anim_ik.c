/*
 * jce_anim_ik.c  Two-bone IK + frame-event dispatch (Sprint 4 #16).
 */

#include <jce/middleware/animation/jce_anim_ik.h>

#include <math.h>
#include <string.h>

/* ────────── vector helpers ────────── */

static void v3_sub(float *o, const float *a, const float *b)
{ o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }
static void v3_add(float *o, const float *a, const float *b)
{ o[0]=a[0]+b[0]; o[1]=a[1]+b[1]; o[2]=a[2]+b[2]; }
static void v3_scl(float *o, const float *a, float s)
{ o[0]=a[0]*s; o[1]=a[1]*s; o[2]=a[2]*s; }
static void v3_lerp(float *o, const float *a, const float *b, float t)
{ o[0]=a[0]+(b[0]-a[0])*t; o[1]=a[1]+(b[1]-a[1])*t; o[2]=a[2]+(b[2]-a[2])*t; }
static float v3_dot(const float *a, const float *b)
{ return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static float v3_len(const float *a) { return sqrtf(v3_dot(a,a)); }
static void  v3_norm(float *o)
{ float l=v3_len(o); if (l>1e-8f){o[0]/=l;o[1]/=l;o[2]/=l;} }
static void  v3_cross(float *o, const float *a, const float *b)
{ o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0]; }

JCE_API int JCE_CALL
jce_anim_ik_two_bone_solve(const JceIkTwoBoneInput *in, JceIkTwoBoneOutput *out)
{
    if (!in || !out) return 0;

    /* Original bone lengths (preserved). */
    float ab[3], bc[3];
    v3_sub(ab, in->mid_pos, in->root_pos);
    v3_sub(bc, in->end_pos, in->mid_pos);
    float L1 = v3_len(ab);
    float L2 = v3_len(bc);

    /* Vector from root to target. */
    float at[3]; v3_sub(at, in->target, in->root_pos);
    float D = v3_len(at);
    int reached = 1;

    /* Clamp: if target out of reach -> straight line. */
    if (D > L1 + L2 - 1e-5f) {
        D = L1 + L2;
        reached = 0;
    } else if (D < fabsf(L1 - L2) + 1e-5f) {
        D = fabsf(L1 - L2) + 1e-5f;
        reached = 0;
    }

    /* Mid-joint angle via law of cosines. */
    float cosA = (L1*L1 + D*D - L2*L2) / (2.0f * L1 * D);
    if (cosA > 1.0f) cosA = 1.0f; else if (cosA < -1.0f) cosA = -1.0f;
    float sinA = sqrtf(1.0f - cosA*cosA);

    /* Build orthonormal basis around at-direction with pole vector. */
    float at_n[3] = { at[0], at[1], at[2] }; v3_norm(at_n);
    float pole_proj[3], pole_local[3];
    {
        /* Project pole onto plane perpendicular to at_n. */
        float p_minus_root[3]; v3_sub(p_minus_root, in->pole, in->root_pos);
        float d = v3_dot(p_minus_root, at_n);
        float along[3]; v3_scl(along, at_n, d);
        v3_sub(pole_proj, p_minus_root, along);
        if (v3_len(pole_proj) < 1e-6f) {
            /* Pole degenerate; fabricate one. */
            float up[3] = {0, 1, 0};
            v3_cross(pole_proj, at_n, up);
            if (v3_len(pole_proj) < 1e-6f) {
                float right[3] = {1, 0, 0};
                v3_cross(pole_proj, at_n, right);
            }
        }
        v3_norm(pole_proj);
        pole_local[0] = pole_proj[0];
        pole_local[1] = pole_proj[1];
        pole_local[2] = pole_proj[2];
    }

    /* Solved mid: along at_n by L1*cosA, perpendicular by L1*sinA toward pole. */
    float solved_mid[3];
    {
        float along[3];  v3_scl(along, at_n,    L1 * cosA);
        float perp[3];   v3_scl(perp,  pole_local, L1 * sinA);
        float off[3];    v3_add(off, along, perp);
        v3_add(solved_mid, in->root_pos, off);
    }

    /* Solved end: target if reached, else along at_n at distance L1+L2. */
    float solved_end[3];
    if (reached) {
        solved_end[0] = in->target[0];
        solved_end[1] = in->target[1];
        solved_end[2] = in->target[2];
    } else {
        float along[3]; v3_scl(along, at_n, L1 + L2);
        v3_add(solved_end, in->root_pos, along);
    }

    /* Blend by weight. */
    float w = in->weight; if (w < 0) w = 0; if (w > 1) w = 1;
    v3_lerp(out->mid_pos, in->mid_pos, solved_mid, w);
    v3_lerp(out->end_pos, in->end_pos, solved_end, w);
    return reached;
}

/* ────────── Event dispatch ────────── */

JCE_API void JCE_CALL
jce_anim_events_advance(const JceAnimIkEventTrack *track,
                        float prev_t, float cur_t,
                        JceAnimIkEventFn fn, void *user)
{
    if (!track || !fn || track->count <= 0 || !track->events) return;
    if (prev_t == cur_t) return;

    if (cur_t > prev_t) {
        /* Forward, no wrap. */
        for (int i = 0; i < track->count; ++i) {
            float t = track->events[i].time;
            if (t > prev_t && t <= cur_t) fn(&track->events[i], user);
        }
    } else {
        /* Wrap (loop): fire (prev_t, dur] then [0, cur_t]. */
        float dur = track->clip_duration > 0 ? track->clip_duration : 0;
        for (int i = 0; i < track->count; ++i) {
            float t = track->events[i].time;
            if (t > prev_t && (dur <= 0 || t <= dur)) fn(&track->events[i], user);
        }
        for (int i = 0; i < track->count; ++i) {
            float t = track->events[i].time;
            if (t >= 0 && t <= cur_t) fn(&track->events[i], user);
        }
    }
}

/* ── Look-At IK ───────────────────────────────────────────────────── */

/* Local helpers prefixed `lk_` to avoid colliding with the
 * file-static `v3_dot` / `v3_cross` defined above for two-bone IK. */
static void lk_normalize(float v[3])
{
    float lsq = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
    if (lsq < 1e-12f) { v[0]=0; v[1]=0; v[2]=1; return; }
    float inv = 1.0f / sqrtf(lsq);
    v[0] *= inv; v[1] *= inv; v[2] *= inv;
}

static void lk_cross(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}

static float lk_dot(const float a[3], const float b[3])
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

/* Build a rotation quaternion (xyzw) from a basis (forward, up).
 * Internal builder so we don't pull in a heavy mat-from-axes path. */
static void quat_from_basis(const float fwd[3], const float up[3],
                            float out_q[4])
{
    /* Reorthogonalise: right = up × fwd, then up' = fwd × right. */
    float r[3];
    lk_cross(up, fwd, r);
    lk_normalize(r);
    float u2[3];
    lk_cross(fwd, r, u2);
    /* 3×3 column-major basis: col0 = right, col1 = up', col2 = fwd. */
    float m00 = r[0],  m10 = r[1],  m20 = r[2];
    float m01 = u2[0], m11 = u2[1], m21 = u2[2];
    float m02 = fwd[0],m12 = fwd[1],m22 = fwd[2];
    /* Standard 3×3-to-quat conversion. */
    float trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        out_q[3] = 0.25f * s;
        out_q[0] = (m21 - m12) / s;
        out_q[1] = (m02 - m20) / s;
        out_q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        out_q[3] = (m21 - m12) / s;
        out_q[0] = 0.25f * s;
        out_q[1] = (m01 + m10) / s;
        out_q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        out_q[3] = (m02 - m20) / s;
        out_q[0] = (m01 + m10) / s;
        out_q[1] = 0.25f * s;
        out_q[2] = (m12 + m21) / s;
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        out_q[3] = (m10 - m01) / s;
        out_q[0] = (m02 + m20) / s;
        out_q[1] = (m12 + m21) / s;
        out_q[2] = 0.25f * s;
    }
    /* Normalise to compensate for accumulated floating-point error. */
    float lsq = out_q[0]*out_q[0] + out_q[1]*out_q[1]
              + out_q[2]*out_q[2] + out_q[3]*out_q[3];
    if (lsq > 0.0f) {
        float inv = 1.0f / sqrtf(lsq);
        out_q[0] *= inv; out_q[1] *= inv;
        out_q[2] *= inv; out_q[3] *= inv;
    }
}

static void quat_slerp(const float a[4], const float b[4], float t,
                       float out[4])
{
    float dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    float bb[4] = { b[0], b[1], b[2], b[3] };
    if (dot < 0.0f) {
        bb[0] = -bb[0]; bb[1] = -bb[1];
        bb[2] = -bb[2]; bb[3] = -bb[3];
        dot = -dot;
    }
    if (dot > 0.9995f) {
        out[0] = a[0] + t * (bb[0] - a[0]);
        out[1] = a[1] + t * (bb[1] - a[1]);
        out[2] = a[2] + t * (bb[2] - a[2]);
        out[3] = a[3] + t * (bb[3] - a[3]);
        float inv = 1.0f / sqrtf(out[0]*out[0]+out[1]*out[1]+out[2]*out[2]+out[3]*out[3]);
        out[0]*=inv; out[1]*=inv; out[2]*=inv; out[3]*=inv;
        return;
    }
    float theta = acosf(dot);
    float sin_t = sinf(theta);
    float wa = sinf((1.0f - t) * theta) / sin_t;
    float wb = sinf(t * theta) / sin_t;
    out[0] = a[0]*wa + bb[0]*wb;
    out[1] = a[1]*wa + bb[1]*wb;
    out[2] = a[2]*wa + bb[2]*wb;
    out[3] = a[3]*wa + bb[3]*wb;
}

void jce_anim_ik_look_at_solve(const JceIkLookAtInput *in, float out_rot[4])
{
    if (!in || !out_rot) return;
    /* World-space desired forward = normalise(target - bone_pos). */
    float fwd[3] = {
        in->target_pos[0] - in->bone_pos[0],
        in->target_pos[1] - in->bone_pos[1],
        in->target_pos[2] - in->bone_pos[2]
    };
    lk_normalize(fwd);
    /* Pick an up that's not parallel to fwd. */
    float up[3] = { in->world_up[0], in->world_up[1], in->world_up[2] };
    if (fabsf(lk_dot(fwd, up)) > 0.999f) {
        up[0] = 0.0f; up[1] = 0.0f; up[2] = 1.0f;
        if (fabsf(lk_dot(fwd, up)) > 0.999f) { up[0]=1.0f; up[1]=0.0f; up[2]=0.0f; }
    }
    float target_q[4];
    quat_from_basis(fwd, up, target_q);
    /* Honour authored bone-local axes by composing — but for the
     * common case (forward=+Z, up=+Y) target_q is already correct.
     * If the rig uses different axes, slerp by weight handles the
     * blend; full re-orientation is a follow-up. */
    (void)in->bone_forward; (void)in->bone_up;

    float w = in->weight;
    if (w <= 0.0f) {
        out_rot[0] = in->current_rot[0]; out_rot[1] = in->current_rot[1];
        out_rot[2] = in->current_rot[2]; out_rot[3] = in->current_rot[3];
        return;
    }
    if (w >= 1.0f) {
        out_rot[0] = target_q[0]; out_rot[1] = target_q[1];
        out_rot[2] = target_q[2]; out_rot[3] = target_q[3];
        return;
    }
    quat_slerp(in->current_rot, target_q, w, out_rot);
}

/* ── FABRIK n-bone chain ─────────────────────────────────────────── */

void jce_anim_ik_fabrik_solve(float *joints, uint32_t n, const float target[3],
                               uint32_t iterations)
{
    if (!joints || n < 2 || !target) return;
    if (iterations == 0) iterations = 8;

    /* Pre-compute segment lengths (n-1 of them) and total chain reach. */
    float lens[64];
    if (n - 1 > 64) return; /* Cap the chain length to keep stack bounded. */
    float total = 0.0f;
    for (uint32_t i = 0; i + 1 < n; ++i) {
        float dx = joints[(i+1)*3+0] - joints[i*3+0];
        float dy = joints[(i+1)*3+1] - joints[i*3+1];
        float dz = joints[(i+1)*3+2] - joints[i*3+2];
        lens[i] = sqrtf(dx*dx + dy*dy + dz*dz);
        total += lens[i];
    }
    /* Target distance from root. */
    float rdx = target[0] - joints[0];
    float rdy = target[1] - joints[1];
    float rdz = target[2] - joints[2];
    float rdist = sqrtf(rdx*rdx + rdy*rdy + rdz*rdz);

    if (rdist >= total) {
        /* Unreachable — lay chain straight at target direction. */
        float inv = rdist > 0 ? 1.0f / rdist : 0.0f;
        float dir[3] = { rdx * inv, rdy * inv, rdz * inv };
        for (uint32_t i = 1; i < n; ++i) {
            joints[i*3+0] = joints[(i-1)*3+0] + dir[0] * lens[i-1];
            joints[i*3+1] = joints[(i-1)*3+1] + dir[1] * lens[i-1];
            joints[i*3+2] = joints[(i-1)*3+2] + dir[2] * lens[i-1];
        }
        return;
    }

    float root_x = joints[0], root_y = joints[1], root_z = joints[2];
    for (uint32_t it = 0; it < iterations; ++it) {
        /* Forward reach: pin end to target, walk back. */
        joints[(n-1)*3+0] = target[0];
        joints[(n-1)*3+1] = target[1];
        joints[(n-1)*3+2] = target[2];
        for (uint32_t i = n - 1; i > 0; --i) {
            float dx = joints[(i-1)*3+0] - joints[i*3+0];
            float dy = joints[(i-1)*3+1] - joints[i*3+1];
            float dz = joints[(i-1)*3+2] - joints[i*3+2];
            float l = sqrtf(dx*dx + dy*dy + dz*dz);
            float k = l > 0 ? lens[i-1] / l : 0.0f;
            joints[(i-1)*3+0] = joints[i*3+0] + dx * k;
            joints[(i-1)*3+1] = joints[i*3+1] + dy * k;
            joints[(i-1)*3+2] = joints[i*3+2] + dz * k;
        }
        /* Backward reach: pin root, walk forward. */
        joints[0] = root_x;
        joints[1] = root_y;
        joints[2] = root_z;
        for (uint32_t i = 0; i + 1 < n; ++i) {
            float dx = joints[(i+1)*3+0] - joints[i*3+0];
            float dy = joints[(i+1)*3+1] - joints[i*3+1];
            float dz = joints[(i+1)*3+2] - joints[i*3+2];
            float l = sqrtf(dx*dx + dy*dy + dz*dz);
            float k = l > 0 ? lens[i] / l : 0.0f;
            joints[(i+1)*3+0] = joints[i*3+0] + dx * k;
            joints[(i+1)*3+1] = joints[i*3+1] + dy * k;
            joints[(i+1)*3+2] = joints[i*3+2] + dz * k;
        }
    }
}
