/*
 * jce_anim_ik.c  Two-bone IK + frame-event dispatch (Sprint 4 #16).
 */

#include <jce/middleware/animation/jce_anim_ik.h>

#include <jce/os/core/jce_math.h>   /* jce_q_slerp (shortest-arc), jce_quat */

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

    /* Degenerate geometry guard: a zero-length first/second bone (root==mid or
     * mid==end) or a target coincident with the root makes the law-of-cosines
     * denominator (2*L1*D) zero → NaN/Inf.  Reachable now that kind==1 can
     * dispatch arbitrary joint triples.  Leave the input joints untouched. */
    if (L1 < 1e-8f || L2 < 1e-8f || D < 1e-8f) {
        out->mid_pos[0] = in->mid_pos[0];
        out->mid_pos[1] = in->mid_pos[1];
        out->mid_pos[2] = in->mid_pos[2];
        out->end_pos[0] = in->end_pos[0];
        out->end_pos[1] = in->end_pos[1];
        out->end_pos[2] = in->end_pos[2];
        return 0;
    }

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

/* ────────── Aim solver ────────── */

JCE_API int JCE_CALL
jce_anim_ik_aim_solve(const JceIkAimInput *in, float out_forward[3])
{
    if (!in || !out_forward) return 0;

    /* Current forward (normalised). */
    float fwd[3] = { in->forward[0], in->forward[1], in->forward[2] };
    if (v3_len(fwd) < 1e-8f) {
        out_forward[0] = out_forward[1] = out_forward[2] = 0.0f;
        return 0;
    }
    v3_norm(fwd);

    /* Desired aim direction: pivot → target. */
    float aim[3]; v3_sub(aim, in->target, in->pivot);
    if (v3_len(aim) < 1e-6f) {
        /* Target coincides with the pivot: nothing to aim at. */
        out_forward[0] = fwd[0]; out_forward[1] = fwd[1]; out_forward[2] = fwd[2];
        return 0;
    }
    v3_norm(aim);

    /* NOTE: this solver returns a blended aim DIRECTION only — no twist/roll is
     * resolved, so the caller's `up` vector is intentionally not consulted here.
     * (A previous version built a right/up orthonormal basis from `up`, but it
     * was never used and is omitted to keep the result honest.) */

    /* Blend between the input forward and the aim direction by weight, then
     * renormalise (slerp-ish via normalised lerp is adequate for an aim axis). */
    float w = in->weight; if (w < 0) w = 0; if (w > 1) w = 1;
    float res[3];
    v3_lerp(res, fwd, aim, w);
    if (v3_len(res) < 1e-6f) {
        /* fwd and aim antiparallel at w≈0.5 → fall back to the aim. */
        res[0]=aim[0]; res[1]=aim[1]; res[2]=aim[2];
    }
    v3_norm(res);
    out_forward[0]=res[0]; out_forward[1]=res[1]; out_forward[2]=res[2];
    return 1;
}

/* ────────── N-bone chain solvers ────────── */

/* Cyclic Coordinate Descent: iterate from the tip-most movable joint back to
 * the root, rotating each joint so the end effector swings toward the target.
 * Pure geometry over the joint-position array; bone lengths are implied by the
 * input spacing and preserved because each step is a pure rotation about a
 * joint about-point. */
JCE_API int JCE_CALL
jce_anim_ik_ccd_solve(float *joints, int count, const float target[3],
                      int max_iters, float tolerance)
{
    if (!joints || count < 2 || !target) return 0;
    if (max_iters <= 0) max_iters = 16;
    float tol = (tolerance > 0.0f) ? tolerance : 1e-3f;

    int end = count - 1;
    int iters = 0;
    for (int it = 0; it < max_iters; ++it) {
        float *ep = &joints[end * 3];
        float to_end[3]; v3_sub(to_end, ep, target);
        if (v3_len(to_end) <= tol) break;
        ++iters;

        /* From the joint just below the end effector down to the root. */
        for (int j = end - 1; j >= 0; --j) {
            float *jp = &joints[j * 3];
            float *cur_end = &joints[end * 3];

            float to_cur[3]; v3_sub(to_cur, cur_end, jp);
            float to_tgt[3]; v3_sub(to_tgt, target,  jp);
            float lc = v3_len(to_cur), lt = v3_len(to_tgt);
            if (lc < 1e-8f || lt < 1e-8f) continue;
            v3_norm(to_cur); v3_norm(to_tgt);

            float cosA = v3_dot(to_cur, to_tgt);
            if (cosA > 1.0f) cosA = 1.0f; else if (cosA < -1.0f) cosA = -1.0f;
            if (cosA > 0.9999999f) continue;          /* already aligned */
            float angle = acosf(cosA);

            float axis[3]; v3_cross(axis, to_cur, to_tgt);
            if (v3_len(axis) < 1e-8f) {
                /* 180° flip: pick an arbitrary perpendicular axis. */
                float up[3] = {0,1,0};
                v3_cross(axis, to_cur, up);
                if (v3_len(axis) < 1e-8f) {
                    float rx[3] = {1,0,0};
                    v3_cross(axis, to_cur, rx);
                }
            }
            v3_norm(axis);

            /* Rotate every joint from j+1..end about `axis` through `angle`,
             * pivoting at jp (Rodrigues' rotation). */
            float c = cosf(angle), s = sinf(angle);
            for (int k = j + 1; k <= end; ++k) {
                float *kp = &joints[k * 3];
                float v[3]; v3_sub(v, kp, jp);
                float kxv[3]; v3_cross(kxv, axis, v);
                float kdv = v3_dot(axis, v);
                /* v_rot = v*c + (axis×v)*s + axis*(axis·v)*(1-c) */
                float r[3];
                r[0] = v[0]*c + kxv[0]*s + axis[0]*kdv*(1.0f-c);
                r[1] = v[1]*c + kxv[1]*s + axis[1]*kdv*(1.0f-c);
                r[2] = v[2]*c + kxv[2]*s + axis[2]*kdv*(1.0f-c);
                kp[0] = jp[0] + r[0];
                kp[1] = jp[1] + r[1];
                kp[2] = jp[2] + r[2];
            }
        }
    }
    return iters;
}

/* Forward And Backward Reaching Inverse Kinematics. */
JCE_API int JCE_CALL
jce_anim_ik_fabrik_solve(float *joints, int count, const float target[3],
                         int max_iters, float tolerance)
{
    if (!joints || count < 2 || !target) return 0;
    if (count > JCE_IK_FABRIK_MAX) count = JCE_IK_FABRIK_MAX;
    if (max_iters <= 0) max_iters = 16;
    float tol = (tolerance > 0.0f) ? tolerance : 1e-3f;

    int n = count;
    int end = n - 1;

    /* Capture original bone lengths and the immutable root position. */
    float len[JCE_IK_FABRIK_MAX];
    float total = 0.0f;
    for (int i = 0; i < end; ++i) {
        float d[3]; v3_sub(d, &joints[(i+1)*3], &joints[i*3]);
        len[i] = v3_len(d);
        total += len[i];
    }
    float root[3] = { joints[0], joints[1], joints[2] };

    /* Unreachable: stretch straight toward the target, clamped at max reach. */
    float to_t[3]; v3_sub(to_t, target, root);
    float dist = v3_len(to_t);
    if (dist > total) {
        float dir[3] = { to_t[0], to_t[1], to_t[2] };
        v3_norm(dir);
        float acc = 0.0f;
        for (int i = 0; i < end; ++i) {
            acc += len[i];
            joints[(i+1)*3+0] = root[0] + dir[0]*acc;
            joints[(i+1)*3+1] = root[1] + dir[1]*acc;
            joints[(i+1)*3+2] = root[2] + dir[2]*acc;
        }
        joints[0]=root[0]; joints[1]=root[1]; joints[2]=root[2];
        return 1;
    }

    int iters = 0;
    for (int it = 0; it < max_iters; ++it) {
        float *ep = &joints[end * 3];
        float te[3]; v3_sub(te, ep, target);
        if (v3_len(te) <= tol) break;
        ++iters;

        /* ── Backward reach: set end to target, pull each joint toward it. */
        joints[end*3+0] = target[0];
        joints[end*3+1] = target[1];
        joints[end*3+2] = target[2];
        for (int i = end - 1; i >= 0; --i) {
            float *pi  = &joints[i*3];
            float *pi1 = &joints[(i+1)*3];
            float d[3]; v3_sub(d, pi, pi1);
            float dl = v3_len(d);
            float r = (dl > 1e-8f) ? (len[i] / dl) : 0.0f;
            float np[3];
            v3_lerp(np, pi1, pi, r); /* pi1 + (pi-pi1)*r */
            pi[0]=np[0]; pi[1]=np[1]; pi[2]=np[2];
        }

        /* ── Forward reach: pin root back, push each joint outward. */
        joints[0]=root[0]; joints[1]=root[1]; joints[2]=root[2];
        for (int i = 0; i < end; ++i) {
            float *pi  = &joints[i*3];
            float *pi1 = &joints[(i+1)*3];
            float d[3]; v3_sub(d, pi1, pi);
            float dl = v3_len(d);
            float r = (dl > 1e-8f) ? (len[i] / dl) : 0.0f;
            float np[3];
            v3_lerp(np, pi, pi1, r); /* pi + (pi1-pi)*r */
            pi1[0]=np[0]; pi1[1]=np[1]; pi1[2]=np[2];
        }
    }
    return iters;
}

/* ────────── Single-target constraint solvers ────────── */

/* clamp01 with a non-finite guard (NaN/Inf weight → 0, i.e. keep current). */
static float clamp01_finite(float w)
{
    if (!(w == w)) return 0.0f;          /* NaN */
    if (w < 0.0f) return 0.0f;
    if (w > 1.0f) return 1.0f;
    return w;
}

JCE_API void JCE_CALL
jce_anim_ik_position_solve(const float cur_pos[3], const float target_pos[3],
                           float weight, float out_pos[3])
{
    if (!out_pos) return;
    if (!cur_pos) {
        /* No current position to blend from: snap to target (or zero). */
        if (target_pos) { out_pos[0]=target_pos[0]; out_pos[1]=target_pos[1]; out_pos[2]=target_pos[2]; }
        else            { out_pos[0]=out_pos[1]=out_pos[2]=0.0f; }
        return;
    }
    if (!target_pos) {
        out_pos[0]=cur_pos[0]; out_pos[1]=cur_pos[1]; out_pos[2]=cur_pos[2];
        return;
    }
    float w = clamp01_finite(weight);
    out_pos[0] = cur_pos[0] + (target_pos[0] - cur_pos[0]) * w;
    out_pos[1] = cur_pos[1] + (target_pos[1] - cur_pos[1]) * w;
    out_pos[2] = cur_pos[2] + (target_pos[2] - cur_pos[2]) * w;
}

/* Normalise a quaternion; a zero-length / non-finite quat falls back to
 * identity so downstream slerp never sees a degenerate input. */
static jce_quat q_sanitize(const float q4[4])
{
    if (!q4) return jce_q_identity();
    float x=q4[0], y=q4[1], z=q4[2], w=q4[3];
    float len2 = x*x + y*y + z*z + w*w;
    if (!(len2 == len2) || len2 < 1e-12f)   /* NaN or ~zero */
        return jce_q_identity();
    return jce_q_normalize(jce_v4(x, y, z, w));
}

JCE_API void JCE_CALL
jce_anim_ik_rotation_solve(const float cur_quat[4], const float target_quat[4],
                           float weight, float out_quat[4])
{
    if (!out_quat) return;
    jce_quat a = q_sanitize(cur_quat);
    jce_quat b = q_sanitize(target_quat);
    float w = clamp01_finite(weight);
    /* jce_q_slerp picks the shortest arc (negates b when a·b < 0) and degrades
     * to a normalised lerp for nearly-parallel quats; result is unit length. */
    jce_quat r = jce_q_normalize(jce_q_slerp(a, b, w));
    out_quat[0]=r.x; out_quat[1]=r.y; out_quat[2]=r.z; out_quat[3]=r.w;
}

/* ────────── Event dispatch ────────── */

JCE_API void JCE_CALL
jce_anim_events_advance(const JceAnimEventTrack *track,
                        float prev_t, float cur_t,
                        JceAnimEventFn fn, void *user)
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
