/*
 * jce_anim_ik.h  Two-bone IK + animation event dispatch (Sprint 4 #16).
 *
 * Implements:
 *   - Two-bone IK solver (analytic): given root + mid + end bone positions
 *     and a target, computes new mid/end positions that reach the target
 *     while preserving bone lengths and lying on the plane defined by the
 *     pole vector.  Runtime per-frame call after the animation pose is
 *     evaluated.
 *   - Frame-event dispatcher: holds a sorted list of (time, event_id,
 *     payload) tuples per clip; jce_anim_events_advance(prev_t, cur_t,
 *     callback, user) fires every event whose time falls in (prev_t,
 *     cur_t], handling looping and reverse playback.  Events are authored
 *     by the editor's Animation Editor into the <skeleton>.anim.json
 *     sidecar ({ "<clipName>": [ { time, name?, id?, f0?, f1?, i0? } ] })
 *     that jce_scene_renderer.c lazily loads per SkeletalAnimator.
 */

#ifndef JCE_ANIM_IK_H
#define JCE_ANIM_IK_H

#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ── Two-bone IK ───────────────────────────────────────────────────── */

typedef struct {
    float root_pos[3];
    float mid_pos [3];
    float end_pos [3];
    float target  [3];   /* desired end-effector position (world) */
    float pole    [3];   /* pole vector (world); used to disambiguate plane */
    float weight;        /* 0..1 blend toward IK solution */
} JceIkTwoBoneInput;

typedef struct {
    float mid_pos[3];    /* solved mid joint position */
    float end_pos[3];    /* solved end joint position */
} JceIkTwoBoneOutput;

/* Solve and write to out.  Returns 1 if solution reached target,
   0 if clamped (target out of reach: bones laid out straight toward target). */
JCE_API int JCE_CALL
jce_anim_ik_two_bone_solve(const JceIkTwoBoneInput *in, JceIkTwoBoneOutput *out);

/* ── Aim solver ────────────────────────────────────────────────────── */

/* Rotate a single bone so its `forward` axis points at `target`, keeping the
 * bone pivot fixed.  `up` (a.k.a. pole) biases the twist about the aim axis so
 * the result is deterministic.  All vectors are world (or any single) space;
 * the solver is pure geometry over the inputs.
 *
 *   pivot     : bone origin (rotation centre), unchanged by the solve
 *   forward   : current world-space direction the bone "looks" along (e.g. the
 *               vector from the bone to its child); need not be unit length
 *   up        : preferred world-space up/pole direction (twist reference)
 *   target    : world point the forward axis should aim at
 *   weight    : 0..1 blend between the input forward and the aimed forward
 *
 * Writes the resulting unit aim direction into out_forward (forward rotated to
 * point pivot->target, blended by weight).  Returns 1 on success, 0 if the
 * inputs were degenerate (NULL, target == pivot, or zero-length forward) — in
 * which case out_forward is left as the normalised input forward (or zero). */
typedef struct {
    float pivot  [3];
    float forward[3];
    float up     [3];
    float target [3];
    float weight;
} JceIkAimInput;

JCE_API int JCE_CALL
jce_anim_ik_aim_solve(const JceIkAimInput *in, float out_forward[3]);

/* ── N-bone chain solvers (CCD / FABRIK) ───────────────────────────── */

/* Upper bound on chain length the FABRIK solver buffers internally (bone-
 * length scratch on the stack).  Chains longer than this are clamped. */
#define JCE_IK_FABRIK_MAX 256

/* Both chain solvers operate IN PLACE on an array of `count` joint positions
 * (joints[0] is the fixed root/base, joints[count-1] is the end effector).
 * They preserve the original bone lengths (segment[i] = |joints[i+1]-joints[i]|
 * measured from the input) and move the joints so the end effector reaches
 * `target`.  For an unreachable target the chain is laid out straight toward
 * the target, clamping the end effector at max reach.
 *
 *   joints     : count*3 floats, modified in place (root stays put)
 *   count      : number of joints (>= 2)
 *   target     : desired end-effector position
 *   max_iters  : iteration budget (<=0 → a sane default is used)
 *   tolerance  : stop once |end - target| <= tolerance (<=0 → small default)
 *
 * Returns the number of iterations performed (0 if already within tolerance or
 * the inputs were degenerate). */
JCE_API int JCE_CALL
jce_anim_ik_ccd_solve(float *joints, int count, const float target[3],
                      int max_iters, float tolerance);

JCE_API int JCE_CALL
jce_anim_ik_fabrik_solve(float *joints, int count, const float target[3],
                         int max_iters, float tolerance);

/* ── Single-target constraint solvers (Position / Rotation / MultiParent) ──
 *
 * Pure value-blend helpers backing the rigging-IK kinds that are NOT chain-
 * reach problems: kind 3 (Position), kind 4 (Rotation), and kind 2
 * (MultiParent = both at once).  They simply drive one transform component
 * toward a single target by `weight`, mirroring how a parent constraint
 * follows its (single) parent.  No skeleton/scene dependency — plain math
 * over POD float arrays, exactly like the chain solvers above.
 *
 * Position: out = lerp(cur, target, clamp01(weight)).  NaN-free; a non-finite
 *           weight is treated as 0 (keep the current position). */
JCE_API void JCE_CALL
jce_anim_ik_position_solve(const float cur_pos[3], const float target_pos[3],
                           float weight, float out_pos[3]);

/* Rotation: out = shortest-arc slerp(cur, target, clamp01(weight)), always
 * returned unit length.  Inputs are normalised first; a zero-length (or non-
 * finite) quaternion falls back to identity so the result is never degenerate.
 * The slerp picks the short way around (it negates `target` when the two
 * quaternions are more than 180° apart). */
JCE_API void JCE_CALL
jce_anim_ik_rotation_solve(const float cur_quat[4], const float target_quat[4],
                           float weight, float out_quat[4]);

/* MultiParent (kind 2) reuses BOTH of the above: with the component's single
 * target_entity it is a full parent constraint — blend position AND rotation
 * toward that one target by `weight`.  (Multi-source weighting across several
 * parents is not representable by JceIkConstraint, which carries one target.) */

/* ── Frame events ──────────────────────────────────────────────────── */

typedef struct {
    float    time;       /* seconds within clip */
    uint32_t id;         /* user-defined event id (e.g. footstep, hitbox-on) */
    float    f0, f1;     /* small payload (avoid heap) */
    int      i0;
    char     name[48];   /* optional label ("" = unnamed); numeric-only
                            sidecar events leave this empty */
} JceAnimEvent;

typedef struct {
    JceAnimEvent *events; /* externally owned; sorted by time */
    int           count;
    float         clip_duration;
} JceAnimEventTrack;

typedef void (*JceAnimEventFn)(const JceAnimEvent *ev, void *user);

/* Fire callbacks for events in the (prev_t, cur_t] interval, supporting
   wrap-around when cur_t < prev_t (looped clip). */
JCE_API void JCE_CALL
jce_anim_events_advance(const JceAnimEventTrack *track,
                        float prev_t, float cur_t,
                        JceAnimEventFn fn, void *user);

JCE_EXTERN_C_END

#endif /* JCE_ANIM_IK_H */
