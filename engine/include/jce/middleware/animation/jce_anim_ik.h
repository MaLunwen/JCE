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
 *     cur_t], handling looping and reverse playback.  The editor's
 *     animation editor saves these into the clip's .anim.json sidecar.
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

/* ── Look-At IK (Unity LookAt constraint) ───────────────────────── *
 *
 * Compute the rotation quaternion (world-space) that orients a bone's
 * forward axis toward `target` while keeping its up axis as close as
 * possible to `world_up`.  Applied to a head / eye / turret bone to
 * track an entity at runtime.
 *
 * The bone's authored forward and up axes (in bone-local space) are
 * passed in so different rigs / coordinate systems work without
 * special-casing inside the helper.  Defaults for typical glTF rigs:
 *   forward = (0, 0, 1)
 *   up      = (0, 1, 0)
 *   world_up = (0, 1, 0)
 *
 * `weight` ∈ [0, 1] blends from "no rotation change" (current_rot)
 * toward the look-at solution (slerp).  Returns the blended rotation
 * as a quaternion (xyzw).
 */
typedef struct {
    float bone_pos     [3];   /* world-space bone origin */
    float current_rot  [4];   /* xyzw current bone rotation (world-space) */
    float target_pos   [3];   /* world-space look target */
    float bone_forward [3];   /* +axis in bone-local space */
    float bone_up      [3];   /* up axis in bone-local space */
    float world_up     [3];   /* world-space up reference */
    float weight;             /* 0..1 blend toward solution */
} JceIkLookAtInput;

JCE_API void JCE_CALL
jce_anim_ik_look_at_solve(const JceIkLookAtInput *in, float out_rot_xyzw[4]);

/* ── FABRIK n-bone chain solver ─────────────────────────────────── *
 *
 * Forward And Backward Reaching Inverse Kinematics.  Iteratively
 * adjusts joint positions (preserving segment lengths) so the last
 * joint reaches `target`.  Works for any chain length ≥ 2 and is
 * O(N × iterations) per call.  Typical use: spine bend, multi-bone
 * tentacle, mech arm.
 *
 * `joints` is an array of `joint_count` xyz positions; the function
 * mutates it in place.  Segment lengths are computed from the input
 * positions before the first iteration.  `iterations` 8–16 is plenty
 * for visually smooth results. */
JCE_API void JCE_CALL
jce_anim_ik_fabrik_solve(float        *joints,    /* joint_count × 3 floats */
                         uint32_t      joint_count,
                         const float   target[3],
                         uint32_t      iterations);

/* ── Frame events ──────────────────────────────────────────────────── */

typedef struct {
    float    time;       /* seconds within clip */
    uint32_t id;         /* user-defined event id (e.g. footstep, hitbox-on) */
    float    f0, f1;     /* small payload (avoid heap) */
    int      i0;
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
