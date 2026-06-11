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
