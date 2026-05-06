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
