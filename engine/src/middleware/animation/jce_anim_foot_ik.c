/*
 * jce_anim_foot_ik.c  Ground-adaptive foot placement (Foot IK).
 *
 * Pure solver: reuses the analytic two-bone IK solver (jce_anim_ik.h) to bend
 * each leg toward its ground-contact target, and applies the standard "lower
 * the hips to the lowest foot" pelvis drop.  No scene/physics/renderer.
 * See jce_anim_foot_ik.h for the full algorithm + the pelvis-drop convention.
 */

#include <jce/middleware/animation/jce_anim_foot_ik.h>

#include <jce/os/core/jce_math.h>

#include <math.h>
#include <string.h>

/* Local finite guard (NaN / Inf reject).  isfinite is C99 <math.h>. */
static int fik_finite1(float v) { return isfinite(v) != 0; }
static int fik_finite3(const float *v)
{
    return fik_finite1(v[0]) && fik_finite1(v[1]) && fik_finite1(v[2]);
}

static float fik_clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void JCE_CALL
jce_anim_foot_ik_solve(const JceFootIkInput *in, JceFootIkOutput *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));   /* all legs solved=false, pelvis_offset 0 */
    if (!in) return;

    int legc = in->leg_count;
    if (legc < 0)                     legc = 0;
    if (legc > JCE_FOOT_IK_MAX_LEGS)  legc = JCE_FOOT_IK_MAX_LEGS;

    float blend = in->blend;
    if (!fik_finite1(blend)) blend = 0.0f;
    blend = fik_clampf(blend, 0.0f, 1.0f);

    /* max_step_height bounds both the pelvis drop and the per-foot raise; a
     * non-finite / negative value disables the IK (acts as a 0 clamp). */
    float max_step = in->max_step_height;
    if (!fik_finite1(max_step) || max_step < 0.0f) max_step = 0.0f;

    float foot_off = in->foot_offset;
    if (!fik_finite1(foot_off)) foot_off = 0.0f;

    /* blend==0 or nothing to do -> all pass-through (already memset). */
    if (blend <= 0.0f || legc == 0)
        return;

    /* ── (b) pelvis drop: -clamp(max over grounded legs of how far the
     * animated ankle sits ABOVE its desired ground contact, 0, max_step). A
     * foot that must be RAISED (ground above the animated foot) yields over==0
     * and never drops the hips. ───────────────────────────────────────── */
    float worst_over = 0.0f;
    for (int i = 0; i < legc; i++) {
        const JceFootIkLeg *lg = &in->legs[i];
        if (!lg->grounded) continue;
        if (!fik_finite3(lg->ankle) || !fik_finite1(lg->ground_y)) continue;
        float desired_ankle_y = lg->ground_y + foot_off;
        float over = lg->ankle[1] - desired_ankle_y;   /* >0 => foot dangles */
        if (over > worst_over) worst_over = over;
    }
    float pelvis_off = -fik_clampf(worst_over, 0.0f, max_step);
    out->pelvis_offset_y = pelvis_off * blend;

    /* ── (c)/(d) per-leg two-bone solve toward the ground contact ──────── */
    for (int i = 0; i < legc; i++) {
        const JceFootIkLeg *lg = &in->legs[i];

        /* Ungrounded or non-finite leg -> pass through (solved stays false). */
        if (!lg->grounded) continue;
        if (!fik_finite3(lg->hip) || !fik_finite3(lg->knee) ||
            !fik_finite3(lg->ankle) || !fik_finite1(lg->ground_y))
            continue;

        /* Degenerate bone lengths -> the two-bone solver would no-op; skip so
         * the animated pose is preserved. */
        float ux = lg->knee[0] - lg->hip[0];
        float uy = lg->knee[1] - lg->hip[1];
        float uz = lg->knee[2] - lg->hip[2];
        float lx = lg->ankle[0] - lg->knee[0];
        float ly = lg->ankle[1] - lg->knee[1];
        float lz = lg->ankle[2] - lg->knee[2];
        float upper_len = sqrtf(ux * ux + uy * uy + uz * uz);
        float lower_len = sqrtf(lx * lx + ly * ly + lz * lz);
        if (upper_len < 1e-6f || lower_len < 1e-6f) continue;

        /* Shift the hip down by the FULL (un-blended) pelvis offset so the
         * two-bone solve produces the fully-solved pose; `blend` is applied
         * ONCE at the end as a lerp from animated -> fully-solved (otherwise
         * blend compounds: once on the pelvis shift, once on the final lerp).
         * The blended pelvis shift is still emitted via out->pelvis_offset_y. */
        float shifted_hip[3] = {
            lg->hip[0], lg->hip[1] + pelvis_off, lg->hip[2]
        };

        /* Desired ankle Y = ground + foot offset, but the RAISE relative to
         * the (shifted) animated ankle is clamped to ±max_step so a wild
         * ground reading cannot teleport the foot. */
        float shifted_ankle_y = lg->ankle[1] + pelvis_off;
        float desired_ankle_y = lg->ground_y + foot_off;
        float raise = desired_ankle_y - shifted_ankle_y;
        raise = fik_clampf(raise, -max_step, max_step);
        float target_ankle_y = shifted_ankle_y + raise;

        JceIkTwoBoneInput tb;
        tb.root_pos[0] = shifted_hip[0];
        tb.root_pos[1] = shifted_hip[1];
        tb.root_pos[2] = shifted_hip[2];
        tb.mid_pos[0]  = lg->knee[0];
        tb.mid_pos[1]  = lg->knee[1] + pelvis_off;
        tb.mid_pos[2]  = lg->knee[2];
        tb.end_pos[0]  = lg->ankle[0];
        tb.end_pos[1]  = shifted_ankle_y;
        tb.end_pos[2]  = lg->ankle[2];
        tb.target[0]   = lg->ankle[0];
        tb.target[1]   = target_ankle_y;
        tb.target[2]   = lg->ankle[2];
        /* Pole/hint = current knee (after the pelvis shift) keeps the knee
         * bending in its animated direction. */
        tb.pole[0]     = tb.mid_pos[0];
        tb.pole[1]     = tb.mid_pos[1];
        tb.pole[2]     = tb.mid_pos[2];
        tb.weight      = 1.0f;   /* blend applied below (lerp from animated) */

        JceIkTwoBoneOutput tbo;
        jce_anim_ik_two_bone_solve(&tb, &tbo);

        if (!fik_finite3(tbo.mid_pos) || !fik_finite3(tbo.end_pos))
            continue;   /* defensive: solver produced garbage -> pass through */

        /* (d) lerp from the ANIMATED knee/ankle (un-shifted) to the solved
         * pose by blend.  At blend==1 this is the full solved pose; at
         * blend==0.5 it is exactly halfway. */
        out->legs[i].knee[0] = lg->knee[0] + (tbo.mid_pos[0] - lg->knee[0]) * blend;
        out->legs[i].knee[1] = lg->knee[1] + (tbo.mid_pos[1] - lg->knee[1]) * blend;
        out->legs[i].knee[2] = lg->knee[2] + (tbo.mid_pos[2] - lg->knee[2]) * blend;
        out->legs[i].ankle[0] = lg->ankle[0] + (tbo.end_pos[0] - lg->ankle[0]) * blend;
        out->legs[i].ankle[1] = lg->ankle[1] + (tbo.end_pos[1] - lg->ankle[1]) * blend;
        out->legs[i].ankle[2] = lg->ankle[2] + (tbo.end_pos[2] - lg->ankle[2]) * blend;
        out->legs[i].solved = true;
    }
}
