/*
 * jce_predict_locomotion.h — a PURE, DETERMINISTIC kinematic locomotion
 *                            step usable as a JcePredictionStepFn.
 *
 * Why this module exists
 * ----------------------
 * jce_net_prediction.h is a GENERIC client-prediction ring + rollback core:
 * it stores opaque {input, state} blobs and re-derives "now" by replaying a
 * caller-supplied pure step function.  It deliberately has NO opinion about
 * what a state or input is.
 *
 * To wire client prediction into the runtime we need a CONCRETE,
 * unit-testable step function.  Production Bullet character resimulation is
 * NOT deterministic (contact solver iteration order, broadphase, etc.), so it
 * cannot satisfy the rollback/replay determinism contract.  This module
 * instead supplies a small, closed-form KINEMATIC integrator over plain
 * floats — no ECS, no physics, no Bullet, no globals — so that:
 *   - it is fully deterministic: out = f(prev, input) with no hidden inputs,
 *   - it is 100% unit-testable + portable (math.h only),
 *   - rollback + replay reproduce the timeline bit-for-bit on every peer.
 *
 * A production Bullet-resim prediction path (deterministic fixed-iteration
 * solver, or a server-authoritative "trust + visually-blend" scheme) is an
 * explicit documented follow-up; the runtime composes THIS step today.
 *
 * Layer: L4 (middleware/net).  Pure: depends only on <math.h> + jce_defs.h.
 */

#ifndef JCE_PREDICT_LOCOMOTION_H
#define JCE_PREDICT_LOCOMOTION_H

#include <jce/os/core/jce_defs.h>

#include <stdint.h>
#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Input / state blobs                                                 */
/* ================================================================== */

/*
 * Per-tick player input fed to the locomotion step.  Mirrors the movement
 * subset of JceRuntimeInput (the runtime builds one of these from rt->input).
 *   walk_x / walk_z : scene-space move direction (not necessarily unit; the
 *                     step normalizes it).
 *   speed_mult      : extra gameplay multiplier (crouch / slow zones; <=0
 *                     treated as 1 inside the step).
 *   jump  / sprint  : edge / held booleans packed as bytes for a fixed,
 *                     padding-stable wire layout.
 */
typedef struct JcePredictInput {
    float   walk_x;
    float   walk_z;
    float   speed_mult;
    uint8_t jump;
    uint8_t sprint;
    uint8_t _pad[2];
} JcePredictInput;

/*
 * Predicted kinematic state.  pos/vel are world-space; yaw is the facing
 * angle (radians) the step turns toward the move direction while moving.
 */
typedef struct JcePredictState {
    float pos[3];
    float vel[3];
    float yaw;
} JcePredictState;

/* ================================================================== */
/* Step parameters (the JcePredictionStepFn `user` cookie)             */
/* ================================================================== */

/*
 * Tuning passed as the step's `user` pointer.  Must stay constant across a
 * predict + replay window (the determinism contract): same params + same
 * input sequence ⇒ same states.
 *
 * Sane defaults (documented; see jce_predict_loco_params_default):
 *   dt          = 1/60       fixed tick seconds
 *   move_speed  = 5.0        m/s ground speed at unit input
 *   sprint_mult = 1.6        speed multiplier while `sprint`
 *   gravity     = 20.0       m/s² downward accel
 *   jump_speed  = 7.0        m/s upward impulse on a grounded jump
 *   ground_y    = 0.0        flat ground plane height (pos.y floor)
 */
typedef struct JcePredictLocoParams {
    float dt;
    float move_speed;
    float sprint_mult;
    float gravity;
    float jump_speed;
    float ground_y;
} JcePredictLocoParams;

/* Fill `p` with the documented defaults above.  NULL-safe (no-op on NULL). */
JCE_API void JCE_CALL
jce_predict_loco_params_default(JcePredictLocoParams *p);

/* ================================================================== */
/* Step + compare (JcePredictionStepFn / JcePredictionCompareFn ABI)   */
/* ================================================================== */

/*
 * Deterministic kinematic locomotion step.  Signature-compatible with
 * JcePredictionStepFn from jce_net_prediction.h:
 *
 *   prev_state : const JcePredictState*
 *   input      : const JcePredictInput*
 *   out_state  : JcePredictState*       (fully overwritten)
 *   user       : JcePredictLocoParams*  (NULL -> internal defaults)
 *
 * Closed-form per-tick integration (pure float math):
 *   horiz vel = normalize(walk_x,walk_z) * move_speed
 *               * (sprint?sprint_mult:1) * max(speed_mult,1-floor) ;
 *   pos.xz   += vel.xz * dt ;
 *   vel.y    -= gravity * dt ;
 *   on_ground = pos.y <= ground_y + eps ;
 *   if (jump && on_ground) vel.y = jump_speed ;
 *   pos.y    += vel.y * dt ;
 *   if (pos.y < ground_y) { pos.y = ground_y; vel.y = 0; }
 *   yaw       = atan2(walk_x, walk_z) while moving, else keep prev yaw.
 *
 * Returns 0 always on valid pointers; non-zero (1) only when prev_state /
 * out_state is NULL (so the prediction core treats it as a step failure).
 */
JCE_API int JCE_CALL
jce_predict_locomotion_step(const void *prev_state,
                            const void *input,
                            void       *out_state,
                            void       *user);

/*
 * Tolerant state comparator.  Signature-compatible with
 * JcePredictionCompareFn (returns true == EQUAL).  Equal iff the two states'
 * positions are within an epsilon (default 1cm) so reconcile does NOT correct
 * on harmless float noise; `user` may point to a float epsilon override
 * (NULL -> default).
 */
JCE_API bool JCE_CALL
jce_predict_loco_compare(const void *a, const void *b, void *user);

/* Default position epsilon (metres) used by jce_predict_loco_compare when its
 * `user` is NULL. */
#define JCE_PREDICT_LOCO_POS_EPSILON  0.01f

JCE_EXTERN_C_END

#endif /* JCE_PREDICT_LOCOMOTION_H */
