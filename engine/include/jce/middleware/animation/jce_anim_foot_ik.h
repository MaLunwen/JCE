/*
 * jce_anim_foot_ik.h  Ground-adaptive foot placement (Foot IK).
 *
 * A PURE, dependency-light solver that lifts/lowers an animated biped's feet
 * so they plant on uneven terrain / stairs instead of floating or clipping.
 * It is built ON TOP of the analytic two-bone IK solver (jce_anim_ik.h):
 * Foot IK is the APPLICATION (where to put each foot + how much to drop the
 * hips); the two-bone solver is the geometry (bend the leg to reach there).
 *
 * Layer: Animation (middleware).  Depends ONLY on jce_defs / jce_math (for the
 * vector POD + finite helpers) and jce_anim_ik.h (for the two-bone types +
 * solver).  It does NOT include scene, physics, or renderer headers — the
 * caller (the scene renderer) supplies the already-sampled foot positions and
 * the ground heights/normals it gathered from a raycast hook, in ONE
 * consistent space (model or world).  All positions in a single solve call
 * MUST be in the same space; the solver is pure geometry over them.
 *
 * ── ALGORITHM ────────────────────────────────────────────────────────────
 * Given the current animated hip / knee / ankle of each leg and the sampled
 * ground height + normal under each foot:
 *
 *  (a) Per grounded leg, the DESIRED ankle height is
 *          desired_ankle_y = ground_y + foot_offset
 *      (foot_offset lifts the ankle above the sole so the foot rests ON the
 *      surface rather than sinking into it).  The required RAISE is
 *          raise = desired_ankle_y - current_ankle_y
 *      (positive when the ground is ABOVE the animated foot — e.g. a step up).
 *
 *  (b) PELVIS DROP CONVENTION (the test pins this): the hips drop to the
 *      LOWEST planted foot so the highest planted foot can still reach the
 *      ground without overstretching the leg.  Concretely, for each grounded
 *      leg measure how far the animated ankle is ABOVE its desired ground
 *      contact:
 *          over[i] = max(0, current_ankle_y - desired_ankle_y[i])
 *      (over[i] > 0 means the foot would dangle above / the ground is BELOW
 *      the animated foot, e.g. stepping down).  Then
 *          pelvis_offset_y = -clamp( max_i over[i], 0, max_step_height )
 *      i.e. a NEGATIVE (downward) pelvis shift whose magnitude is the largest
 *      such gap, clamped to max_step_height.  A foot that needs to be RAISED
 *      (ground above the animated foot) contributes over[i]==0, so raising one
 *      foot does NOT drop the hips.
 *
 *  (c) Per leg, shift the hip by pelvis_offset_y, then build a
 *      JceIkTwoBoneInput { root = shifted hip, mid = knee, end = ankle,
 *      target = (ankle.x, clamp(desired_ankle_y), ankle.z), pole = knee } and
 *      call jce_anim_ik_two_bone_solve to bend the leg so the ankle reaches
 *      the (possibly clamped) ground contact while bone lengths are preserved.
 *      The target's vertical raise relative to the (shifted) ankle is clamped
 *      to ±max_step_height so a wild ground value cannot teleport a foot.
 *
 *  (d) Everything is scaled by `blend` (0 = animated pose passed through,
 *      1 = full IK): the solved knee/ankle are lerped from the animated
 *      knee/ankle, and pelvis_offset_y is multiplied by blend.
 *
 *  (e) An ungrounded leg (grounded==false), blend<=0, or a leg with a
 *      degenerate bone length leaves solved=false and its joints untouched
 *      (the caller passes the animated pose through for those).  All math is
 *      finite-guarded (zero-length bones, NaN/Inf ground values are ignored).
 */

#ifndef JCE_ANIM_FOOT_IK_H
#define JCE_ANIM_FOOT_IK_H

#include <jce/os/core/jce_defs.h>
#include <jce/middleware/animation/jce_anim_ik.h>  /* two-bone solver + types */

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* Number of legs the solver supports (biped). */
#define JCE_FOOT_IK_MAX_LEGS 2

/* One leg's current animated joints + the ground sampled under its foot.
 * Positions are in the caller's chosen single space (model or world). */
typedef struct {
    float hip  [3];          /* current animated hip   (upper-leg root) */
    float knee [3];          /* current animated knee  (mid joint)      */
    float ankle[3];          /* current animated ankle (end effector)   */
    float ground_y;          /* sampled ground height under this foot   */
    float ground_normal[3];  /* sampled ground normal (for foot roll)   */
    bool  grounded;          /* true if the ground ray hit a surface    */
} JceFootIkLeg;

typedef struct {
    float        pelvis[3];                    /* current animated pelvis pos  */
    JceFootIkLeg legs[JCE_FOOT_IK_MAX_LEGS];   /* 0 = left, 1 = right          */
    int          leg_count;                    /* number of valid legs (1..2)  */
    float        max_step_height;              /* clamp pelvis drop + foot raise */
    float        foot_offset;                  /* lift ankle above the sole    */
    float        blend;                        /* 0..1 overall IK weight       */
} JceFootIkInput;

typedef struct {
    float pelvis_offset_y;   /* signed vertical pelvis shift to apply (<= 0) */
    struct {
        float knee [3];      /* solved knee  (valid iff solved)             */
        float ankle[3];      /* solved ankle (valid iff solved)             */
        bool  solved;        /* false => pass the animated pose through      */
    } legs[JCE_FOOT_IK_MAX_LEGS];
} JceFootIkOutput;

/* Solve foot placement + pelvis drop.  Pure geometry; deterministic; no
 * allocation.  Safe with NULL in/out (writes a zeroed, all-pass-through
 * output when out != NULL).  See the algorithm note above for conventions. */
JCE_API void JCE_CALL
jce_anim_foot_ik_solve(const JceFootIkInput *in, JceFootIkOutput *out);

JCE_EXTERN_C_END

#endif /* JCE_ANIM_FOOT_IK_H */
